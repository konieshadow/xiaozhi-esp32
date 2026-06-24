#include "kids_english_protocol.h"

#include "application.h"
#include "assets/lang_config.h"
#include "audio_service.h"
#include "board.h"

#include <cJSON.h>
#include <esp_log.h>
#include <http.h>

#include <algorithm>
#include <cstring>

#define TAG "KidsEnglish"

#ifndef CONFIG_KIDS_ENGLISH_SERVER_URL
#define CONFIG_KIDS_ENGLISH_SERVER_URL ""
#endif

namespace {
constexpr int kHealthTimeoutMs = 3000;
constexpr int kStartSessionTimeoutMs = 5000;
constexpr int kUploadTimeoutMs = 20000;
constexpr int kTtsDownloadTimeoutMs = 10000;
constexpr char kMultipartBoundary[] = "----xiaozhi-kids-english-boundary";
constexpr int kPracticeAudioSampleRate = 16000;
constexpr int kPracticeAudioChannels = 1;
constexpr int kPracticeAudioBitsPerSample = 16;
constexpr int kMaxPracticeAudioDurationSeconds = 10;
constexpr size_t kMaxPracticeAudioBytes = 5 * 1024 * 1024;
constexpr size_t kWavHeaderBytes = 44;

std::string JsonString(const cJSON* item) {
    return cJSON_IsString(item) ? item->valuestring : "";
}

int JsonInt(const cJSON* item, int fallback = -1) {
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

double JsonDouble(const cJSON* item, double fallback = -1.0) {
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

void AppendLe16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
}

void AppendLe32(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 24) & 0xff));
}

uint16_t ReadLe16(const char* data) {
    return static_cast<uint16_t>(static_cast<uint8_t>(data[0])) |
           (static_cast<uint16_t>(static_cast<uint8_t>(data[1])) << 8);
}

uint32_t ReadLe32(const char* data) {
    return static_cast<uint32_t>(static_cast<uint8_t>(data[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[3])) << 24);
}
}  // namespace

KidsEnglishProtocol::KidsEnglishProtocol()
    : base_url_(TrimTrailingSlash(CONFIG_KIDS_ENGLISH_SERVER_URL)) {
    server_sample_rate_ = kPracticeAudioSampleRate;
    server_frame_duration_ = OPUS_FRAME_DURATION_MS;
}

KidsEnglishProtocol::~KidsEnglishProtocol() {
    CloseAudioChannel(false);
}

bool KidsEnglishProtocol::Start() {
    if (on_connected_ != nullptr) {
        on_connected_();
    }
    return true;
}

bool KidsEnglishProtocol::OpenAudioChannel() {
    error_occurred_ = false;
    if (base_url_.empty()) {
        SetError("Kids English server URL is empty");
        return false;
    }

    if (!CheckHealth()) {
        return false;
    }
    if (!StartSession()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_audio_.clear();
        channel_opened_ = true;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    EmitPromptMessage();
    return true;
}

void KidsEnglishProtocol::CloseAudioChannel(bool send_goodbye) {
    std::string session_to_end;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!channel_opened_) {
            pending_audio_.clear();
            return;
        }
        channel_opened_ = false;
        pending_audio_.clear();
        session_to_end = session_id_;
    }

    if (send_goodbye && !session_to_end.empty()) {
        std::string path = "/api/sessions/" + session_to_end + "/end";
        cJSON* response = nullptr;
        RequestJson("POST", path, "", &response, kStartSessionTimeoutMs);
        if (response != nullptr) {
            cJSON_Delete(response);
        }
    }

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool KidsEnglishProtocol::IsAudioChannelOpened() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return channel_opened_ && !error_occurred_;
}

bool KidsEnglishProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (packet == nullptr || packet->payload.empty()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!channel_opened_ || upload_in_progress_) {
        return false;
    }
    pending_audio_.push_back(std::move(packet));
    return true;
}

bool KidsEnglishProtocol::SendPcmAudio(std::vector<int16_t>&& pcm) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!channel_opened_ || upload_in_progress_) {
        return false;
    }
    pending_pcm_ = std::move(pcm);
    return true;
}

void KidsEnglishProtocol::SendStartListening(ListeningMode mode) {
    (void)mode;
    std::lock_guard<std::mutex> lock(mutex_);
    pending_audio_.clear();
    pending_pcm_.clear();
}

void KidsEnglishProtocol::SendStopListening() {
    std::vector<std::unique_ptr<AudioStreamPacket>> audio_to_upload;
    std::vector<int16_t> pcm_to_upload;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!channel_opened_ || upload_in_progress_) {
            return;
        }
        audio_to_upload = std::move(pending_audio_);
        pcm_to_upload = std::move(pending_pcm_);
        pending_audio_.clear();
        pending_pcm_.clear();
        upload_in_progress_ = true;
    }

    if (audio_to_upload.empty() && pcm_to_upload.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        upload_in_progress_ = false;
        ESP_LOGW(TAG, "No audio captured for current prompt");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_audio_ = std::move(audio_to_upload);
        pending_pcm_ = std::move(pcm_to_upload);
    }

    bool ok = UploadPendingAudio();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_audio_.clear();
        pending_pcm_.clear();
        upload_in_progress_ = false;
    }

    if (!ok) {
        SetError(Lang::Strings::SERVER_ERROR);
    }
}

bool KidsEnglishProtocol::SendText(const std::string& text) {
    ESP_LOGI(TAG, "Ignoring text command in kids English HTTP protocol: %s", text.c_str());
    return true;
}

std::string KidsEnglishProtocol::BuildUrl(const char* path) const {
    return base_url_ + path;
}

bool KidsEnglishProtocol::RequestJson(const std::string& method, const std::string& path,
                                      const std::string& body, cJSON** response_root,
                                      int timeout_ms) {
    if (response_root != nullptr) {
        *response_root = nullptr;
    }

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return false;
    }

    http->SetTimeout(timeout_ms);
    http->SetHeader("Accept", "application/json");
    if (!body.empty()) {
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::string(body));
    } else if (method == "POST" || method == "PUT") {
        http->SetContent(std::string());
    }

    auto url = BuildUrl(path.c_str());
    ESP_LOGI(TAG, "%s %s", method.c_str(), url.c_str());
    if (!http->Open(method, url)) {
        ESP_LOGE(TAG, "HTTP open failed, error=%d", http->GetLastError());
        return false;
    }

    int status = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP status %d, body: %s", status, response.c_str());
        return false;
    }

    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse JSON response: %s", response.c_str());
        return false;
    }

    auto ok = cJSON_GetObjectItem(root, "ok");
    if (!cJSON_IsTrue(ok)) {
        ESP_LOGE(TAG, "Server returned error: %s", GetErrorMessage(root).c_str());
        cJSON_Delete(root);
        return false;
    }

    if (response_root != nullptr) {
        *response_root = root;
    } else {
        cJSON_Delete(root);
    }
    return true;
}

bool KidsEnglishProtocol::CheckHealth() {
    cJSON* root = nullptr;
    bool ok = RequestJson("GET", "/health", "", &root, kHealthTimeoutMs);
    if (root != nullptr) {
        cJSON_Delete(root);
    }
    if (!ok) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
    }
    return ok;
}

bool KidsEnglishProtocol::StartSession() {
    cJSON* root = nullptr;
    if (!RequestJson("POST", "/api/sessions/start", "{}", &root, kStartSessionTimeoutMs)) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    bool ok = ParseStartSessionResponse(root);
    cJSON_Delete(root);
    if (!ok) {
        SetError(Lang::Strings::SERVER_ERROR);
    }
    return ok;
}

bool KidsEnglishProtocol::UploadPendingAudio() {
    std::vector<int16_t> pcm;
    std::string session_id;
    std::string prompt_id;
    std::string device_id = Board::GetInstance().GetUuid();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_id = session_id_;
        prompt_id = prompt_id_;
        pending_audio_.clear();
        pcm = std::move(pending_pcm_);
    }

    if (session_id.empty() || prompt_id.empty()) {
        ESP_LOGE(TAG, "Missing session or prompt id");
        return false;
    }
    if (pcm.empty()) {
        ESP_LOGE(TAG, "No PCM audio captured; WAV upload requires 16 kHz mono PCM");
        return false;
    }

    size_t pcm_bytes = pcm.size() * sizeof(int16_t);
    size_t wav_bytes = kWavHeaderBytes + pcm_bytes;
    int duration_ms = static_cast<int>((pcm.size() * 1000) / kPracticeAudioSampleRate);
    if (duration_ms > kMaxPracticeAudioDurationSeconds * 1000) {
        ESP_LOGW(TAG, "Trimming Kids English recording from %d ms to %d seconds", duration_ms,
                 kMaxPracticeAudioDurationSeconds);
        pcm.resize(kPracticeAudioSampleRate * kMaxPracticeAudioDurationSeconds);
        pcm_bytes = pcm.size() * sizeof(int16_t);
        wav_bytes = kWavHeaderBytes + pcm_bytes;
        duration_ms = static_cast<int>((pcm.size() * 1000) / kPracticeAudioSampleRate);
    }
    if (wav_bytes > kMaxPracticeAudioBytes) {
        ESP_LOGE(TAG, "Kids English WAV too large: %u bytes > %u bytes", (unsigned)wav_bytes,
                 (unsigned)kMaxPracticeAudioBytes);
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return false;
    }

    http->SetTimeout(kUploadTimeoutMs);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Content-Type", std::string("multipart/form-data; boundary=") + kMultipartBoundary);

    std::string wav = CreateWavFile(pcm, kPracticeAudioSampleRate);
    auto url = BuildUrl("/api/practice/audio");
    ESP_LOGI(TAG,
             "Uploading Kids English audio: deviceId=%s sessionId=%s promptId=%s pcmBytes=%u "
             "wavBytes=%u durationMs=%d format=wav/%dHz/%dch/%dbit url=%s",
             device_id.c_str(), session_id.c_str(), prompt_id.c_str(), (unsigned)pcm_bytes,
             (unsigned)wav.size(), duration_ms, kPracticeAudioSampleRate, kPracticeAudioChannels,
             kPracticeAudioBitsPerSample, url.c_str());
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "HTTP upload open failed, error=%d", http->GetLastError());
        return false;
    }

    bool write_ok = WriteMultipartField(http.get(), kMultipartBoundary, "sessionId", session_id) &&
                    WriteMultipartField(http.get(), kMultipartBoundary, "promptId", prompt_id) &&
                    WriteMultipartField(http.get(), kMultipartBoundary, "deviceId", device_id) &&
                    WriteMultipartAudio(http.get(), kMultipartBoundary, wav) &&
                    WriteString(http.get(), std::string("--") + kMultipartBoundary + "--\r\n") &&
                    FinishChunkedBody(http.get());
    if (!write_ok) {
        http->Close();
        ESP_LOGE(TAG, "Failed to write multipart request");
        return false;
    }

    int status = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "Kids English audio upload HTTP status=%d responseBytes=%u", status,
             (unsigned)response.size());
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "Upload failed, status=%d, body=%s", status, response.c_str());
        return false;
    }

    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse upload response: %s", response.c_str());
        return false;
    }

    auto ok = cJSON_GetObjectItem(root, "ok");
    if (!cJSON_IsTrue(ok)) {
        ESP_LOGE(TAG, "Upload returned error: %s", GetErrorMessage(root).c_str());
        cJSON_Delete(root);
        return false;
    }

    PracticeResult result;
    bool parsed = ParsePracticeResult(root, result);
    cJSON_Delete(root);
    if (!parsed) {
        return false;
    }

    ESP_LOGI(TAG,
             "Kids English ASR result: deviceId=%s sessionId=%s promptId=%s requestId=%s "
             "recordingBytes=%u httpStatus=%d recognizedText=\"%s\" score=%d "
             "asrDurationMs=%d audioBytes=%d audioDurationSeconds=%.2f",
             device_id.c_str(), session_id.c_str(), prompt_id.c_str(), result.request_id.c_str(),
             (unsigned)pcm_bytes, status, result.recognized_text.c_str(), result.score,
             result.asr_duration_ms, result.audio_bytes, result.audio_duration_seconds);
    EmitPracticeResult(result);
    return true;
}

bool KidsEnglishProtocol::ParseStartSessionResponse(const cJSON* root) {
    auto data = cJSON_GetObjectItem(root, "data");
    auto session_id = cJSON_GetObjectItem(data, "sessionId");
    auto prompt = cJSON_GetObjectItem(data, "prompt");
    auto prompt_id = cJSON_GetObjectItem(prompt, "id");
    auto prompt_text = cJSON_GetObjectItem(prompt, "text");
    auto prompt_level = cJSON_GetObjectItem(prompt, "level");
    if (!cJSON_IsString(session_id) || !cJSON_IsString(prompt_id) || !cJSON_IsString(prompt_text)) {
        ESP_LOGE(TAG, "Invalid start session response");
        return false;
    }

    session_id_ = session_id->valuestring;
    prompt_id_ = prompt_id->valuestring;
    prompt_text_ = prompt_text->valuestring;
    prompt_level_ = JsonString(prompt_level);
    ESP_LOGI(TAG, "Practice session %s, prompt %s: %s", session_id_.c_str(), prompt_id_.c_str(),
             prompt_text_.c_str());
    return true;
}

bool KidsEnglishProtocol::ParsePracticeResult(const cJSON* root, PracticeResult& result) {
    auto data = cJSON_GetObjectItem(root, "data");
    auto recognized_text = cJSON_GetObjectItem(data, "recognizedText");
    auto score = cJSON_GetObjectItem(data, "score");
    auto feedback = cJSON_GetObjectItem(data, "feedback");
    auto correction = cJSON_GetObjectItem(data, "correction");
    auto next_prompt = cJSON_GetObjectItem(data, "nextPrompt");
    auto next_prompt_id = cJSON_GetObjectItem(next_prompt, "id");
    auto next_prompt_text = cJSON_GetObjectItem(next_prompt, "text");
    auto tts_text = cJSON_GetObjectItem(data, "ttsText");
    auto tts_audio_url = cJSON_GetObjectItem(data, "ttsAudioUrl");
    auto diagnostics = cJSON_GetObjectItem(data, "diagnostics");
    auto request_id = cJSON_GetObjectItem(diagnostics, "requestId");
    auto provider = cJSON_GetObjectItem(diagnostics, "provider");
    auto mode = cJSON_GetObjectItem(diagnostics, "mode");
    auto asr_duration_ms = cJSON_GetObjectItem(diagnostics, "asrDurationMs");
    auto tts_duration_ms = cJSON_GetObjectItem(diagnostics, "ttsDurationMs");
    auto total_duration_ms = cJSON_GetObjectItem(diagnostics, "totalDurationMs");
    auto audio_bytes = cJSON_GetObjectItem(diagnostics, "audioBytes");
    auto audio_duration_seconds = cJSON_GetObjectItem(diagnostics, "audioDurationSeconds");
    auto audio_mime_type = cJSON_GetObjectItem(diagnostics, "audioMimeType");

    if (!cJSON_IsString(recognized_text) || !cJSON_IsNumber(score) || !cJSON_IsString(feedback) ||
        !cJSON_IsString(next_prompt_id) || !cJSON_IsString(next_prompt_text)) {
        ESP_LOGE(TAG, "Invalid practice result response");
        return false;
    }

    result.recognized_text = recognized_text->valuestring;
    result.score = score->valueint;
    result.feedback = feedback->valuestring;
    result.correction = JsonString(correction);
    result.next_prompt_id = next_prompt_id->valuestring;
    result.next_prompt_text = next_prompt_text->valuestring;
    result.tts_text = JsonString(tts_text);
    result.tts_audio_url = JsonString(tts_audio_url);
    result.request_id = JsonString(request_id);
    result.provider = JsonString(provider);
    result.mode = JsonString(mode);
    result.asr_duration_ms = JsonInt(asr_duration_ms);
    result.tts_duration_ms = JsonInt(tts_duration_ms);
    result.total_duration_ms = JsonInt(total_duration_ms);
    result.audio_bytes = JsonInt(audio_bytes);
    result.audio_duration_seconds = JsonDouble(audio_duration_seconds);
    result.audio_mime_type = JsonString(audio_mime_type);
    return true;
}

std::string KidsEnglishProtocol::CreateWavFile(const std::vector<int16_t>& pcm, int sample_rate) const {
    std::string wav;
    uint32_t data_size = pcm.size() * sizeof(int16_t);
    wav.reserve(44 + data_size);
    wav.append("RIFF", 4);
    AppendLe32(wav, 36 + data_size);
    wav.append("WAVE", 4);
    wav.append("fmt ", 4);
    AppendLe32(wav, 16);
    AppendLe16(wav, 1);
    AppendLe16(wav, 1);
    AppendLe32(wav, sample_rate);
    AppendLe32(wav, sample_rate * sizeof(int16_t));
    AppendLe16(wav, sizeof(int16_t));
    AppendLe16(wav, 16);
    wav.append("data", 4);
    AppendLe32(wav, data_size);
    wav.append(reinterpret_cast<const char*>(pcm.data()), data_size);
    return wav;
}

bool KidsEnglishProtocol::ParseWavPcm16Mono(const std::string& wav, std::vector<int16_t>& pcm,
                                            int& sample_rate) const {
    if (wav.size() < 44 || std::memcmp(wav.data(), "RIFF", 4) != 0 ||
        std::memcmp(wav.data() + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Invalid WAV header");
        return false;
    }

    size_t offset = 12;
    bool found_fmt = false;
    bool found_data = false;
    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t data_offset = 0;
    uint32_t data_size = 0;
    sample_rate = 0;

    while (offset + 8 <= wav.size()) {
        const char* chunk = wav.data() + offset;
        uint32_t chunk_size = ReadLe32(chunk + 4);
        offset += 8;
        if (offset + chunk_size > wav.size()) {
            ESP_LOGE(TAG, "Truncated WAV chunk");
            return false;
        }

        if (std::memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
            audio_format = ReadLe16(wav.data() + offset);
            channels = ReadLe16(wav.data() + offset + 2);
            sample_rate = ReadLe32(wav.data() + offset + 4);
            bits_per_sample = ReadLe16(wav.data() + offset + 14);
            found_fmt = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data_offset = offset;
            data_size = chunk_size;
            found_data = true;
        }

        offset += chunk_size + (chunk_size & 1);
    }

    if (!found_fmt || !found_data || audio_format != 1 || channels != 1 || bits_per_sample != 16 ||
        sample_rate <= 0 || (data_size % sizeof(int16_t)) != 0) {
        ESP_LOGE(TAG, "Unsupported WAV format: format=%u channels=%u sample_rate=%d bits=%u",
                 audio_format, channels, sample_rate, bits_per_sample);
        return false;
    }

    pcm.resize(data_size / sizeof(int16_t));
    std::memcpy(pcm.data(), wav.data() + data_offset, data_size);
    return true;
}

bool KidsEnglishProtocol::DownloadAndPlayTtsAudio(const std::string& url) {
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create HTTP client for TTS download");
        return false;
    }

    http->SetTimeout(kTtsDownloadTimeoutMs);
    http->SetHeader("Accept", "audio/wav,*/*");
    ESP_LOGI(TAG, "Downloading TTS audio: %s", url.c_str());
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "TTS download open failed, error=%d", http->GetLastError());
        return false;
    }

    int status = http->GetStatusCode();
    std::string content_type = http->GetResponseHeader("Content-Type");
    std::string body;
    bool read_ok = ReadHttpBody(http.get(), body);
    http->Close();
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "TTS download failed, status=%d, bytes=%u", status, (unsigned)body.size());
        return false;
    }
    if (!read_ok) {
        ESP_LOGE(TAG, "Failed to read TTS audio body");
        return false;
    }

    std::vector<int16_t> pcm;
    int sample_rate = 0;
    if (!ParseWavPcm16Mono(body, pcm, sample_rate)) {
        ESP_LOGE(TAG, "TTS response is not playable WAV, content_type=%s", content_type.c_str());
        return false;
    }

    ESP_LOGI(TAG, "Downloaded TTS WAV: content_type=%s sample_rate=%d samples=%u",
             content_type.c_str(), sample_rate, (unsigned)pcm.size());
    AudioService& audio_service = Application::GetInstance().GetAudioService();
    audio_service.PushPcmToPlaybackQueue(std::move(pcm), sample_rate);
    return true;
}

bool KidsEnglishProtocol::ReadHttpBody(Http* http, std::string& body) {
    body.clear();
    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        body = http->ReadAll();
        return !body.empty();
    }

    body.resize(content_length);
    size_t total_read = 0;
    while (total_read < content_length) {
        int read = http->Read(body.data() + total_read, content_length - total_read);
        if (read <= 0) {
            ESP_LOGE(TAG, "HTTP body read failed at %u/%u bytes", (unsigned)total_read,
                     (unsigned)content_length);
            body.resize(total_read);
            return false;
        }
        total_read += read;
    }
    return true;
}

bool KidsEnglishProtocol::WriteMultipartField(Http* http, const std::string& boundary,
                                              const std::string& name, const std::string& value) {
    std::string data = "--" + boundary + "\r\n";
    data += "Content-Disposition: form-data; name=\"" + name + "\"\r\n\r\n";
    data += value + "\r\n";
    return WriteString(http, data);
}

bool KidsEnglishProtocol::WriteMultipartAudio(Http* http, const std::string& boundary,
                                              const std::string& wav) {
    std::string header = "--" + boundary + "\r\n";
    header += "Content-Disposition: form-data; name=\"audio\"; filename=\"practice.wav\"\r\n";
    header += "Content-Type: audio/wav\r\n\r\n";
    if (!WriteString(http, header)) {
        return false;
    }
    if (http->Write(wav.data(), wav.size()) <= 0) {
        return false;
    }
    return WriteString(http, "\r\n");
}

bool KidsEnglishProtocol::WriteString(Http* http, const std::string& data) {
    return http->Write(data.data(), data.size()) > 0;
}

bool KidsEnglishProtocol::FinishChunkedBody(Http* http) {
    return http->Write(nullptr, 0) > 0;
}

void KidsEnglishProtocol::EmitPromptMessage() {
    EmitEmotion("happy");
    EmitAssistantMessage("Say: " + prompt_text_);
}

void KidsEnglishProtocol::EmitPracticeResult(const PracticeResult& result) {
    EmitSttMessage(result.recognized_text);

    ESP_LOGI(TAG,
             "Practice diagnostics: requestId=%s provider=%s mode=%s asrDurationMs=%d "
             "ttsDurationMs=%d totalDurationMs=%d audioBytes=%d audioDurationSeconds=%.2f "
             "audioMimeType=%s",
             result.request_id.c_str(), result.provider.c_str(), result.mode.c_str(),
             result.asr_duration_ms, result.tts_duration_ms, result.total_duration_ms,
             result.audio_bytes, result.audio_duration_seconds, result.audio_mime_type.c_str());

    std::string message = result.feedback;
    message += " Score: " + std::to_string(result.score);
    if (!result.correction.empty()) {
        message += ". " + result.correction;
    }
    if (!result.next_prompt_text.empty()) {
        message += "\nNext: " + result.next_prompt_text;
    }

    EmitTtsMessage("start");
    EmitAssistantMessage(message);
    if (!result.tts_audio_url.empty()) {
        ESP_LOGI(TAG, "Practice ttsAudioUrl: %s", result.tts_audio_url.c_str());
        if (!DownloadAndPlayTtsAudio(result.tts_audio_url)) {
            ESP_LOGW(TAG, "Failed to download/play TTS audio; falling back to text-only feedback");
        }
    }
    EmitTtsMessage("stop", result.tts_text.empty() ? message.c_str() : result.tts_text.c_str());
    EmitEmotion(result.score >= 80 ? "happy" : "thinking");

    {
        std::lock_guard<std::mutex> lock(mutex_);
        prompt_id_ = result.next_prompt_id;
        prompt_text_ = result.next_prompt_text;
    }
}

void KidsEnglishProtocol::EmitTtsMessage(const char* state, const char* text) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "tts");
    cJSON_AddStringToObject(root, "state", state);
    if (text != nullptr) {
        cJSON_AddStringToObject(root, "text", text);
    }
    DispatchJson(root);
}

void KidsEnglishProtocol::EmitSttMessage(const std::string& text) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "stt");
    cJSON_AddStringToObject(root, "text", text.c_str());
    DispatchJson(root);
}

void KidsEnglishProtocol::EmitAssistantMessage(const std::string& text) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "tts");
    cJSON_AddStringToObject(root, "state", "sentence_start");
    cJSON_AddStringToObject(root, "text", text.c_str());
    DispatchJson(root);
}

void KidsEnglishProtocol::EmitEmotion(const char* emotion) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "llm");
    cJSON_AddStringToObject(root, "emotion", emotion);
    DispatchJson(root);
}

void KidsEnglishProtocol::DispatchJson(cJSON* root) {
    if (on_incoming_json_ != nullptr) {
        on_incoming_json_(root);
    }
    cJSON_Delete(root);
    last_incoming_time_ = std::chrono::steady_clock::now();
}

std::string KidsEnglishProtocol::GetErrorMessage(const cJSON* root) const {
    auto error = cJSON_GetObjectItem(root, "error");
    auto code = cJSON_GetObjectItem(error, "code");
    auto message = cJSON_GetObjectItem(error, "message");
    std::string result = JsonString(code);
    if (!result.empty() && cJSON_IsString(message)) {
        result += ": ";
    }
    result += JsonString(message);
    return result.empty() ? "unknown server error" : result;
}

std::string KidsEnglishProtocol::TrimTrailingSlash(std::string url) {
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

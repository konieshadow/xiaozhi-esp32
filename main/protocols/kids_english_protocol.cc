#include "kids_english_protocol.h"

#include "assets/lang_config.h"
#include "audio_service.h"
#include "board.h"

#include <cJSON.h>
#include <esp_log.h>
#include <http.h>

#define TAG "KidsEnglish"

#ifndef CONFIG_KIDS_ENGLISH_SERVER_URL
#define CONFIG_KIDS_ENGLISH_SERVER_URL ""
#endif

namespace {
constexpr int kHealthTimeoutMs = 3000;
constexpr int kStartSessionTimeoutMs = 5000;
constexpr int kUploadTimeoutMs = 20000;
constexpr char kMultipartBoundary[] = "----xiaozhi-kids-english-boundary";

std::string JsonString(const cJSON* item) {
    return cJSON_IsString(item) ? item->valuestring : "";
}
}  // namespace

KidsEnglishProtocol::KidsEnglishProtocol()
    : base_url_(TrimTrailingSlash(CONFIG_KIDS_ENGLISH_SERVER_URL)) {
    server_sample_rate_ = 16000;
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

void KidsEnglishProtocol::SendStartListening(ListeningMode mode) {
    (void)mode;
    std::lock_guard<std::mutex> lock(mutex_);
    pending_audio_.clear();
}

void KidsEnglishProtocol::SendStopListening() {
    std::vector<std::unique_ptr<AudioStreamPacket>> audio_to_upload;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!channel_opened_ || upload_in_progress_) {
            return;
        }
        audio_to_upload = std::move(pending_audio_);
        pending_audio_.clear();
        upload_in_progress_ = true;
    }

    if (audio_to_upload.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        upload_in_progress_ = false;
        ESP_LOGW(TAG, "No audio captured for current prompt");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_audio_ = std::move(audio_to_upload);
    }

    bool ok = UploadPendingAudio();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_audio_.clear();
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
    std::vector<std::unique_ptr<AudioStreamPacket>> packets;
    std::string session_id;
    std::string prompt_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_id = session_id_;
        prompt_id = prompt_id_;
        packets = std::move(pending_audio_);
    }

    if (session_id.empty() || prompt_id.empty()) {
        ESP_LOGE(TAG, "Missing session or prompt id");
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

    auto url = BuildUrl("/api/practice/audio");
    ESP_LOGI(TAG, "Uploading %u Opus packets to %s", (unsigned)packets.size(), url.c_str());
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "HTTP upload open failed, error=%d", http->GetLastError());
        return false;
    }

    bool write_ok = WriteMultipartField(http.get(), kMultipartBoundary, "sessionId", session_id) &&
                    WriteMultipartField(http.get(), kMultipartBoundary, "promptId", prompt_id) &&
                    WriteMultipartAudio(http.get(), kMultipartBoundary, packets) &&
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
    return true;
}

bool KidsEnglishProtocol::WriteMultipartField(Http* http, const std::string& boundary,
                                              const std::string& name, const std::string& value) {
    std::string data = "--" + boundary + "\r\n";
    data += "Content-Disposition: form-data; name=\"" + name + "\"\r\n\r\n";
    data += value + "\r\n";
    return WriteString(http, data);
}

bool KidsEnglishProtocol::WriteMultipartAudio(
    Http* http, const std::string& boundary,
    const std::vector<std::unique_ptr<AudioStreamPacket>>& packets) {
    std::string header = "--" + boundary + "\r\n";
    header += "Content-Disposition: form-data; name=\"audio\"; filename=\"practice.opus\"\r\n";
    header += "Content-Type: audio/opus\r\n\r\n";
    if (!WriteString(http, header)) {
        return false;
    }

    for (const auto& packet : packets) {
        if (packet == nullptr || packet->payload.empty()) {
            continue;
        }
        if (http->Write(reinterpret_cast<const char*>(packet->payload.data()), packet->payload.size()) <= 0) {
            return false;
        }
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

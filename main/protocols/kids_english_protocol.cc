#include "kids_english_protocol.h"

#include "application.h"
#include "assets/lang_config.h"
#include "audio_service.h"
#include "board.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <http.h>
#include <mbedtls/md.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <sys/time.h>

#define TAG "KidsEnglish"

#ifndef CONFIG_KIDS_ENGLISH_SERVER_URL
#define CONFIG_KIDS_ENGLISH_SERVER_URL ""
#endif

#ifndef CONFIG_KIDS_ENGLISH_DEVICE_ID
#define CONFIG_KIDS_ENGLISH_DEVICE_ID "esp32-devkit-001"
#endif

#ifndef CONFIG_KIDS_ENGLISH_DEVICE_SECRET
#define CONFIG_KIDS_ENGLISH_DEVICE_SECRET "dev-secret"
#endif

namespace {
constexpr int kHealthTimeoutMs = 3000;
constexpr int kDeviceHelloTimeoutMs = 5000;
constexpr int kStartConversationTimeoutMs = 5000;
constexpr int kUploadTimeoutMs = 20000;
constexpr int kStandaloneSpeechTimeoutMs = 20000;
constexpr int kTtsDownloadTimeoutMs = 10000;
constexpr char kMultipartBoundary[] = "----xiaozhi-kids-english-boundary";
constexpr char kSelfTestSpeechText[] = "I like apples.";
constexpr int kPracticeAudioSampleRate = 16000;
constexpr int kPracticeAudioChannels = 1;
constexpr int kPracticeAudioBitsPerSample = 16;
constexpr int kMaxPracticeAudioDurationSeconds = 10;
constexpr size_t kMaxPracticeAudioBytes = 5 * 1024 * 1024;
constexpr size_t kWavHeaderBytes = 44;
constexpr int64_t kUnixTimeReasonableMs = 1600000000000LL;

std::string JsonString(const cJSON* item) {
    return cJSON_IsString(item) ? item->valuestring : "";
}

int JsonInt(const cJSON* item, int fallback = -1) {
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

bool JsonBool(const cJSON* item, bool fallback = false) {
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return fallback;
}

std::string JsonEscape(const std::string& text) {
    cJSON* value = cJSON_CreateString(text.c_str());
    char* printed = cJSON_PrintUnformatted(value);
    std::string escaped = printed == nullptr ? "\"\"" : printed;
    if (printed != nullptr) {
        cJSON_free(printed);
    }
    cJSON_Delete(value);
    return escaped;
}

std::string ToHex(const uint8_t* data, size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.resize(size * 2);
    for (size_t i = 0; i < size; ++i) {
        hex[i * 2] = kHex[(data[i] >> 4) & 0x0f];
        hex[i * 2 + 1] = kHex[data[i] & 0x0f];
    }
    return hex;
}

std::string Sha256Hex(const std::string& data) {
    uint8_t digest[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_setup(&ctx, info, 0) != 0 || mbedtls_md_starts(&ctx) != 0 ||
        mbedtls_md_update(&ctx, reinterpret_cast<const unsigned char*>(data.data()), data.size()) != 0 ||
        mbedtls_md_finish(&ctx, digest) != 0) {
        ESP_LOGE(TAG, "SHA256 calculation failed");
        std::memset(digest, 0, sizeof(digest));
    }
    mbedtls_md_free(&ctx);
    return ToHex(digest, sizeof(digest));
}

std::string HmacSha256Hex(const std::string& key, const std::string& data) {
    uint8_t digest[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_setup(&ctx, info, 1) != 0 ||
        mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const unsigned char*>(key.data()), key.size()) != 0 ||
        mbedtls_md_hmac_update(&ctx, reinterpret_cast<const unsigned char*>(data.data()), data.size()) != 0 ||
        mbedtls_md_hmac_finish(&ctx, digest) != 0) {
        ESP_LOGE(TAG, "HMAC-SHA256 calculation failed");
        std::memset(digest, 0, sizeof(digest));
    }
    mbedtls_md_free(&ctx);
    return ToHex(digest, sizeof(digest));
}

int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + static_cast<int64_t>(doe) - 719468LL;
}

bool ParseIsoUtcMillis(const std::string& iso, int64_t& unix_ms) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millis = 0;
    if (std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d.%3dZ", &year, &month, &day, &hour,
                    &minute, &second, &millis) < 6) {
        return false;
    }
    int64_t days = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    unix_ms = (((days * 24 + hour) * 60 + minute) * 60 + second) * 1000 + millis;
    return true;
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
    : base_url_(TrimTrailingSlash(CONFIG_KIDS_ENGLISH_SERVER_URL)),
      device_id_(CONFIG_KIDS_ENGLISH_DEVICE_ID),
      device_secret_(CONFIG_KIDS_ENGLISH_DEVICE_SECRET) {
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
    if (!DeviceHello()) {
        return false;
    }
    std::string trigger;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        trigger = next_conversation_trigger_;
        next_conversation_trigger_ = "manual";
    }
    if (!StartConversation(trigger.c_str())) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_audio_.clear();
        pending_pcm_.clear();
        channel_opened_ = true;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

void KidsEnglishProtocol::CloseAudioChannel(bool send_goodbye) {
    std::string conversation_to_end;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!channel_opened_) {
            pending_audio_.clear();
            pending_pcm_.clear();
            return;
        }
        channel_opened_ = false;
        pending_audio_.clear();
        pending_pcm_.clear();
        conversation_to_end = conversation_id_;
        conversation_id_.clear();
        session_id_.clear();
    }

    if (send_goodbye && !conversation_to_end.empty()) {
        EndConversation(conversation_to_end);
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

bool KidsEnglishProtocol::SendSimulatedRecording(const std::string& text) {
    ESP_LOGI(TAG, "Simulating Kids English recording: %s", text.c_str());
    error_occurred_ = false;
    if (base_url_.empty()) {
        SetError("Kids English server URL is empty");
        return false;
    }
    if (!CheckHealth() || !DeviceHello()) {
        ESP_LOGE(TAG, "Simulated recording failed during server/device check");
        return false;
    }

    StandaloneTtsResponse tts;
    if (!RequestStandaloneTts(text, tts)) {
        ESP_LOGE(TAG, "Simulated recording failed at TTS step");
        return false;
    }

    std::vector<int16_t> pcm;
    int sample_rate = 0;
    if (!DownloadWavAudio(tts.tts_audio_url, pcm, sample_rate)) {
        ESP_LOGE(TAG, "Simulated recording failed to download generated speech");
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    if (sample_rate != kPracticeAudioSampleRate) {
        ESP_LOGE(TAG, "Simulated recording sample rate mismatch: %d", sample_rate);
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    bool opened_here = false;
    std::string conversation_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        conversation_id = conversation_id_;
    }
    if (conversation_id.empty() || !IsAudioChannelOpened()) {
        if (!StartConversation("touch")) {
            ESP_LOGE(TAG, "Simulated recording failed to start conversation");
            return false;
        }
        opened_here = true;
        Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty();
        std::lock_guard<std::mutex> lock(mutex_);
        conversation_id = conversation_id_;
        channel_opened_ = true;
    }

    if (conversation_id.empty()) {
        ESP_LOGE(TAG, "Simulated recording missing conversation id");
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    UploadResult upload = UploadPcmAudio(std::move(pcm), conversation_id);
    bool ok = upload == UploadResult::kSuccess;
    if (!ok) {
        ESP_LOGE(TAG, "Simulated recording upload failed: result=%d", static_cast<int>(upload));
        SetError(Lang::Strings::SERVER_ERROR);
    }

    Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty();
    if (opened_here) {
        if (ok) {
            EndConversation(conversation_id);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        channel_opened_ = false;
        pending_audio_.clear();
        pending_pcm_.clear();
        conversation_id_.clear();
        session_id_.clear();
    }
    return ok;
}

bool KidsEnglishProtocol::RunSelfTest() {
    ESP_LOGI(TAG, "KIDS_ENGLISH_SELF_TEST_BEGIN baseUrl=%s deviceId=%s", base_url_.c_str(),
             device_id_.c_str());
    error_occurred_ = false;

    if (base_url_.empty()) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL reason=missing_server_url");
        SetError("Kids English server URL is empty");
        return false;
    }

    if (!CheckHealth()) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=health");
        return false;
    }
    ESP_LOGI(TAG, "KIDS_ENGLISH_SELF_TEST_STEP health ok");

    if (!DeviceHello()) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=device_hello");
        return false;
    }
    ESP_LOGI(TAG, "KIDS_ENGLISH_SELF_TEST_STEP device_hello ok");

    StandaloneTtsResponse standalone_tts;
    if (!RequestStandaloneTts(kSelfTestSpeechText, standalone_tts)) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=standalone_tts");
        return false;
    }
    ESP_LOGI(TAG,
             "KIDS_ENGLISH_SELF_TEST_STEP standalone_tts ok requestId=%s format=%s "
             "ttsUrl=%s providerFallback=%s providerErrorCode=%s",
             standalone_tts.request_id.c_str(), standalone_tts.audio_format.c_str(),
             standalone_tts.tts_audio_url.c_str(),
             standalone_tts.provider_fallback ? "true" : "false",
             standalone_tts.provider_error_code.c_str());

    std::vector<int16_t> self_test_pcm;
    int self_test_sample_rate = 0;
    if (!DownloadWavAudio(standalone_tts.tts_audio_url, self_test_pcm, self_test_sample_rate)) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=download_standalone_tts");
        return false;
    }
    if (self_test_sample_rate != kPracticeAudioSampleRate) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=download_standalone_tts sampleRate=%d",
                 self_test_sample_rate);
        return false;
    }
    std::string self_test_wav = CreateWavFile(self_test_pcm, self_test_sample_rate);
    ESP_LOGI(TAG, "KIDS_ENGLISH_SELF_TEST_STEP download_standalone_tts ok samples=%u wavBytes=%u",
             (unsigned)self_test_pcm.size(), (unsigned)self_test_wav.size());

    StandaloneAsrResponse standalone_asr;
    if (!UploadStandaloneAsrAudio(self_test_wav, kSelfTestSpeechText, standalone_asr)) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=standalone_asr");
        return false;
    }
    ESP_LOGI(TAG,
             "KIDS_ENGLISH_SELF_TEST_STEP standalone_asr ok requestId=%s transcript=%s "
             "asrDurationMs=%d totalDurationMs=%d",
             standalone_asr.request_id.c_str(), standalone_asr.transcript.c_str(),
             standalone_asr.asr_duration_ms, standalone_asr.total_duration_ms);

    if (!StartConversation("manual")) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=start_conversation");
        return false;
    }
    Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty();

    std::string conversation_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        conversation_id = conversation_id_;
        channel_opened_ = true;
    }
    if (conversation_id.empty()) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=start_conversation reason=missing_id");
        return false;
    }
    ESP_LOGI(TAG, "KIDS_ENGLISH_SELF_TEST_STEP start_conversation ok conversationId=%s",
             conversation_id.c_str());

    UploadResult upload = UploadPcmAudio(std::vector<int16_t>(self_test_pcm), conversation_id);
    if (upload != UploadResult::kSuccess) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=upload_audio result=%d",
                 static_cast<int>(upload));
        return false;
    }
    Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty();
    ESP_LOGI(TAG, "KIDS_ENGLISH_SELF_TEST_STEP upload_audio ok conversationId=%s",
             conversation_id.c_str());

    if (!EndConversation(conversation_id)) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=end_conversation");
        return false;
    }
    ESP_LOGI(TAG, "KIDS_ENGLISH_SELF_TEST_STEP end_conversation ok conversationId=%s",
             conversation_id.c_str());

    UploadResult ended_upload = UploadPcmAudio(std::move(self_test_pcm), conversation_id);
    if (ended_upload != UploadResult::kConversationEnded) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=ended_upload expected=409 result=%d",
                 static_cast<int>(ended_upload));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        channel_opened_ = false;
        pending_audio_.clear();
        pending_pcm_.clear();
        conversation_id_.clear();
        session_id_.clear();
    }
    ESP_LOGI(TAG, "KIDS_ENGLISH_SELF_TEST_PASS conversationId=%s", conversation_id.c_str());
    return true;
}

void KidsEnglishProtocol::SetNextConversationTrigger(const std::string& trigger) {
    if (trigger != "wake_word" && trigger != "touch" && trigger != "manual") {
        ESP_LOGW(TAG, "Ignoring invalid conversation trigger: %s", trigger.c_str());
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    next_conversation_trigger_ = trigger;
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

void KidsEnglishProtocol::AddDeviceAuthHeaders(Http* http, const std::string& method,
                                               const std::string& path,
                                               const std::string& body_sha256) {
    if (device_id_.empty() || device_secret_.empty()) {
        ESP_LOGW(TAG, "Kids English device ID or secret is empty");
    }

    std::string timestamp = CurrentUnixMillisString();
    uint32_t random_a = esp_random();
    uint32_t random_b = esp_random();
    std::string nonce = Board::GetInstance().GetUuid() + "-" + std::to_string(esp_timer_get_time()) +
                        "-" + std::to_string(random_a) + std::to_string(random_b);
    std::string payload = method + "\n" + path + "\n" + timestamp + "\n" + nonce + "\n" + body_sha256;
    std::string signature_key = Sha256Hex(device_secret_);
    std::string signature = HmacSha256Hex(signature_key, payload);

    http->SetHeader("x-device-id", device_id_);
    http->SetHeader("x-device-timestamp", timestamp);
    http->SetHeader("x-device-nonce", nonce);
    http->SetHeader("x-device-signature", signature);
}

std::string KidsEnglishProtocol::BuildDeviceHelloBody() const {
    auto app_desc = esp_app_get_description();
    std::string body = "{";
    body += "\"capabilities\":{";
    body += "\"microphone\":true,";
    body += "\"speaker\":true,";
    body += "\"touchScreen\":true,";
    body += "\"wakeWord\":true";
    body += "},";
    body += "\"firmwareVersion\":" + JsonEscape(app_desc->version) + ",";
    body += "\"hardwareModel\":" + JsonEscape(BOARD_NAME);
    body += "}";
    return body;
}

std::string KidsEnglishProtocol::BuildStartConversationBody(const char* trigger) const {
    auto app_desc = esp_app_get_description();
    std::string body = "{";
    body += "\"firmwareVersion\":" + JsonEscape(app_desc->version) + ",";
    body += "\"trigger\":" + JsonEscape(trigger == nullptr ? "manual" : trigger);
    body += "}";
    return body;
}

std::string KidsEnglishProtocol::BuildEndConversationBody(const char* reason) const {
    std::string body = "{";
    body += "\"reason\":" + JsonEscape(reason == nullptr ? "device_end" : reason);
    body += "}";
    return body;
}

std::string KidsEnglishProtocol::BuildStandaloneTtsBody(const std::string& text) const {
    std::string body = "{";
    body += "\"text\":" + JsonEscape(text);
    body += "}";
    return body;
}

std::string KidsEnglishProtocol::BuildStandaloneAsrMultipartBody(const std::string& boundary,
                                                                 const std::string& wav,
                                                                 const std::string& prompt_text) const {
    std::string body;
    body.reserve(boundary.size() * 4 + wav.size() + prompt_text.size() + 192);
    if (!prompt_text.empty()) {
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"promptText\"\r\n\r\n";
        body += prompt_text + "\r\n";
    }
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"audio\"; filename=\"sample.wav\"\r\n";
    body += "Content-Type: audio/wav\r\n\r\n";
    body += wav;
    body += "\r\n--" + boundary + "--\r\n";
    return body;
}

std::string KidsEnglishProtocol::BuildMultipartAudioBody(const std::string& boundary,
                                                        const std::string& wav,
                                                        const std::string& client_turn_id,
                                                        const std::string& recorded_at,
                                                        int duration_ms) const {
    auto append_field = [&](std::string& out, const std::string& name, const std::string& value) {
        out += "--" + boundary + "\r\n";
        out += "Content-Disposition: form-data; name=\"" + name + "\"\r\n\r\n";
        out += value + "\r\n";
    };

    std::string body;
    body.reserve(boundary.size() * 6 + wav.size() + client_turn_id.size() + recorded_at.size() + 256);
    append_field(body, "clientTurnId", client_turn_id);
    append_field(body, "recordedAt", recorded_at);
    append_field(body, "durationMs", std::to_string(duration_ms));
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"audio\"; filename=\"sample.wav\"\r\n";
    body += "Content-Type: audio/wav\r\n\r\n";
    body += wav;
    body += "\r\n--" + boundary + "--\r\n";
    return body;
}

std::string KidsEnglishProtocol::GenerateClientTurnId() const {
    uint32_t random_a = esp_random();
    uint32_t random_b = esp_random();
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "turn-%08lx%08lx", static_cast<unsigned long>(random_a),
             static_cast<unsigned long>(random_b));
    return std::string(buffer);
}

std::string KidsEnglishProtocol::CurrentUnixMillisString() const {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    int64_t unix_ms = static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
    unix_ms += server_time_offset_ms_;
    if (unix_ms < kUnixTimeReasonableMs) {
        ESP_LOGW(TAG, "System time is not synced; signed request timestamp may be rejected");
    }
    return std::to_string(unix_ms);
}

std::string KidsEnglishProtocol::CurrentIsoTimestamp() const {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    int64_t unix_ms = static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
    unix_ms += server_time_offset_ms_;
    time_t seconds = static_cast<time_t>(unix_ms / 1000);
    int millis = static_cast<int>(unix_ms % 1000);
    if (millis < 0) {
        millis += 1000;
    }
    struct tm utc;
    gmtime_r(&seconds, &utc);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
             utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, millis);
    return std::string(buffer);
}

void KidsEnglishProtocol::UpdateServerTimeOffset(const cJSON* server_time) {
    std::string timestamp = JsonString(server_time);
    int64_t server_ms = 0;
    if (!timestamp.empty() && ParseIsoUtcMillis(timestamp, server_ms)) {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        int64_t local_ms = static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
        server_time_offset_ms_ = server_ms - local_ms;
        ESP_LOGI(TAG, "Kids English server time offset: %d ms",
                 static_cast<int>(server_time_offset_ms_));
    }
}

void KidsEnglishProtocol::LogDeviceHelloResponse(const cJSON* root) const {
    auto data = cJSON_GetObjectItem(root, "data");
    auto device_state = cJSON_GetObjectItem(data, "deviceState");
    auto settings = cJSON_GetObjectItem(data, "settings");
    auto service_capabilities = cJSON_GetObjectItem(data, "serviceCapabilities");
    auto audio_upload = cJSON_GetObjectItem(service_capabilities, "audioUpload");
    auto tts_url = cJSON_GetObjectItem(service_capabilities, "ttsUrl");
    auto streaming = cJSON_GetObjectItem(service_capabilities, "streaming");

    ESP_LOGI(TAG,
             "Device hello: deviceId=%s state=%s languageLevel=%s voice=%s "
             "dailyPracticeMinutes=%d audioUpload=%s ttsUrl=%s streaming=%s",
             device_id_.c_str(), JsonString(device_state).c_str(),
             JsonString(cJSON_GetObjectItem(settings, "languageLevel")).c_str(),
             JsonString(cJSON_GetObjectItem(settings, "voice")).c_str(),
             JsonInt(cJSON_GetObjectItem(settings, "dailyPracticeMinutes"), 0),
             cJSON_IsTrue(audio_upload) ? "true" : "false", cJSON_IsTrue(tts_url) ? "true" : "false",
             cJSON_IsTrue(streaming) ? "true" : "false");
}

bool KidsEnglishProtocol::RequestJson(const std::string& method, const std::string& path,
                                      const std::string& body, cJSON** response_root,
                                      int timeout_ms, bool authenticated) {
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
    std::string body_sha256 = Sha256Hex(body);
    if (method == "POST" || method == "PUT") {
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::string(body));
    }
    if (authenticated) {
        AddDeviceAuthHeaders(http.get(), method, path, body_sha256);
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
    bool ok = RequestJson("GET", "/health", "", &root, kHealthTimeoutMs, false);
    if (root != nullptr) {
        auto data = cJSON_GetObjectItem(root, "data");
        UpdateServerTimeOffset(cJSON_GetObjectItem(data, "timestamp"));
        cJSON_Delete(root);
    }
    if (!ok) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
    }
    return ok;
}

bool KidsEnglishProtocol::DeviceHello() {
    cJSON* root = nullptr;
    std::string body = BuildDeviceHelloBody();
    if (!RequestJson("POST", "/api/device/hello", body, &root, kDeviceHelloTimeoutMs)) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    auto data = cJSON_GetObjectItem(root, "data");
    UpdateServerTimeOffset(cJSON_GetObjectItem(data, "serverTime"));
    LogDeviceHelloResponse(root);
    cJSON_Delete(root);
    return true;
}

bool KidsEnglishProtocol::StartConversation(const char* trigger) {
    cJSON* root = nullptr;
    std::string body = BuildStartConversationBody(trigger);
    if (!RequestJson("POST", "/api/conversations/start", body, &root, kStartConversationTimeoutMs)) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    ConversationResponse response;
    bool ok = ParseConversationResponse(root, response);
    cJSON_Delete(root);
    if (!ok) {
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        conversation_id_ = response.conversation_id;
        session_id_ = conversation_id_;
    }
    ESP_LOGI(TAG, "Started conversation %s requestId=%s", response.conversation_id.c_str(),
             response.request_id.c_str());
    return HandleConversationResponse(response, "Let's speak English.");
}

bool KidsEnglishProtocol::UploadPendingAudio() {
    std::vector<int16_t> pcm;
    std::string conversation_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        conversation_id = conversation_id_;
        pending_audio_.clear();
        pcm = std::move(pending_pcm_);
    }

    if (conversation_id.empty()) {
        ESP_LOGE(TAG, "Missing conversation id");
        return false;
    }
    if (pcm.empty()) {
        ESP_LOGE(TAG, "No PCM audio captured; WAV upload requires 16 kHz mono PCM");
        return false;
    }

    return UploadPcmAudio(std::move(pcm), conversation_id) == UploadResult::kSuccess;
}

KidsEnglishProtocol::UploadResult KidsEnglishProtocol::UploadPcmAudio(
    std::vector<int16_t>&& pcm, const std::string& conversation_id) {
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
        return UploadResult::kFailed;
    }

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return UploadResult::kFailed;
    }

    http->SetTimeout(kUploadTimeoutMs);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Content-Type", std::string("multipart/form-data; boundary=") + kMultipartBoundary);

    std::string wav = CreateWavFile(pcm, kPracticeAudioSampleRate);
    std::string client_turn_id = GenerateClientTurnId();
    std::string recorded_at = CurrentIsoTimestamp();
    std::string multipart_body =
        BuildMultipartAudioBody(kMultipartBoundary, wav, client_turn_id, recorded_at, duration_ms);
    std::string path = "/api/conversations/" + conversation_id + "/turns/audio";
    AddDeviceAuthHeaders(http.get(), "POST", path, Sha256Hex(""));
    http->SetContent(std::move(multipart_body));
    auto url = BuildUrl(path.c_str());
    ESP_LOGI(TAG,
             "Uploading Kids English turn: deviceId=%s conversationId=%s clientTurnId=%s pcmBytes=%u "
             "wavBytes=%u durationMs=%d format=wav/%dHz/%dch/%dbit url=%s",
             device_id_.c_str(), conversation_id.c_str(), client_turn_id.c_str(), (unsigned)pcm_bytes,
             (unsigned)wav.size(), duration_ms, kPracticeAudioSampleRate, kPracticeAudioChannels,
             kPracticeAudioBitsPerSample, url.c_str());
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "HTTP upload open failed, error=%d", http->GetLastError());
        return UploadResult::kFailed;
    }

    int status = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "Kids English audio upload HTTP status=%d responseBytes=%u", status,
             (unsigned)response.size());
    if (status < 200 || status >= 300) {
        cJSON* error_root = cJSON_Parse(response.c_str());
        if (status == 409 && IsConversationEndedError(error_root)) {
            std::lock_guard<std::mutex> lock(mutex_);
            conversation_id_.clear();
            session_id_.clear();
            ESP_LOGW(TAG,
                     "Conversation already ended after upload attempt; local conversation state cleared");
            if (error_root != nullptr) {
                cJSON_Delete(error_root);
            }
            return UploadResult::kConversationEnded;
        }
        if (error_root != nullptr) {
            cJSON_Delete(error_root);
        }
        ESP_LOGE(TAG, "Upload failed, status=%d, body=%s", status, response.c_str());
        return UploadResult::kFailed;
    }

    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse upload response: %s", response.c_str());
        return UploadResult::kFailed;
    }

    auto ok = cJSON_GetObjectItem(root, "ok");
    if (!cJSON_IsTrue(ok)) {
        ESP_LOGE(TAG, "Upload returned error: %s", GetErrorMessage(root).c_str());
        if (IsConversationEndedError(root)) {
            std::lock_guard<std::mutex> lock(mutex_);
            conversation_id_.clear();
            session_id_.clear();
            cJSON_Delete(root);
            return UploadResult::kConversationEnded;
        }
        cJSON_Delete(root);
        return UploadResult::kFailed;
    }

    ConversationResponse result;
    bool parsed = ParseConversationResponse(root, result);
    cJSON_Delete(root);
    if (!parsed) {
        return UploadResult::kFailed;
    }

    ESP_LOGI(TAG,
             "Kids English turn result: deviceId=%s conversationId=%s turnId=%s requestId=%s "
             "recordingBytes=%u httpStatus=%d asrDurationMs=%d llmDurationMs=%d "
             "ttsDurationMs=%d totalDurationMs=%d",
             device_id_.c_str(), conversation_id.c_str(), result.turn_id.c_str(),
             result.request_id.c_str(), (unsigned)pcm_bytes, status, result.asr_duration_ms,
             result.llm_duration_ms, result.tts_duration_ms, result.total_duration_ms);
    EmitSttMessage("Audio submitted.");
    return HandleConversationResponse(result, "I heard you.") ? UploadResult::kSuccess
                                                              : UploadResult::kFailed;
}

bool KidsEnglishProtocol::RequestStandaloneTts(const std::string& text,
                                               StandaloneTtsResponse& response) {
    cJSON* root = nullptr;
    std::string body = BuildStandaloneTtsBody(text);
    if (!RequestJson("POST", "/api/device/tts", body, &root, kStandaloneSpeechTimeoutMs)) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    bool parsed = ParseStandaloneTtsResponse(root, response);
    cJSON_Delete(root);
    if (!parsed) {
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    ESP_LOGI(TAG,
             "Standalone TTS result: requestId=%s format=%s ttsUrl=%s ttsDurationMs=%d "
             "totalDurationMs=%d providerFallback=%s providerErrorCode=%s",
             response.request_id.c_str(), response.audio_format.c_str(),
             response.tts_audio_url.c_str(), response.tts_duration_ms, response.total_duration_ms,
             response.provider_fallback ? "true" : "false", response.provider_error_code.c_str());
    return true;
}

bool KidsEnglishProtocol::UploadStandaloneAsrAudio(const std::string& wav,
                                                   const std::string& prompt_text,
                                                   StandaloneAsrResponse& response) {
    if (wav.empty() || wav.size() > kMaxPracticeAudioBytes) {
        ESP_LOGE(TAG, "Standalone ASR WAV size invalid: %u bytes", (unsigned)wav.size());
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return false;
    }

    const std::string path = "/api/device/asr";
    std::string multipart_body = BuildStandaloneAsrMultipartBody(kMultipartBoundary, wav, prompt_text);

    http->SetTimeout(kStandaloneSpeechTimeoutMs);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Content-Type", std::string("multipart/form-data; boundary=") + kMultipartBoundary);
    AddDeviceAuthHeaders(http.get(), "POST", path, Sha256Hex(""));
    http->SetContent(std::move(multipart_body));

    auto url = BuildUrl(path.c_str());
    ESP_LOGI(TAG, "Uploading standalone ASR audio: deviceId=%s wavBytes=%u prompt=%s url=%s",
             device_id_.c_str(), (unsigned)wav.size(), prompt_text.c_str(), url.c_str());
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Standalone ASR open failed, error=%d", http->GetLastError());
        return false;
    }

    int status = http->GetStatusCode();
    std::string body = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "Standalone ASR HTTP status=%d responseBytes=%u", status, (unsigned)body.size());
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "Standalone ASR failed, status=%d, body=%s", status, body.c_str());
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse standalone ASR response: %s", body.c_str());
        return false;
    }

    auto ok = cJSON_GetObjectItem(root, "ok");
    if (!cJSON_IsTrue(ok)) {
        ESP_LOGE(TAG, "Standalone ASR returned error: %s", GetErrorMessage(root).c_str());
        cJSON_Delete(root);
        return false;
    }

    bool parsed = ParseStandaloneAsrResponse(root, response);
    cJSON_Delete(root);
    if (!parsed) {
        return false;
    }

    ESP_LOGI(TAG,
             "Standalone ASR result: requestId=%s transcript=%s format=%s asrDurationMs=%d "
             "totalDurationMs=%d",
             response.request_id.c_str(), response.transcript.c_str(), response.audio_format.c_str(),
             response.asr_duration_ms, response.total_duration_ms);
    return true;
}

bool KidsEnglishProtocol::EndConversation(const std::string& conversation_id) {
    std::string path = "/api/conversations/" + conversation_id + "/end";
    cJSON* root = nullptr;
    std::string body = BuildEndConversationBody("device_end");
    bool ok = RequestJson("POST", path, body, &root, kStartConversationTimeoutMs);
    if (root != nullptr) {
        auto data = cJSON_GetObjectItem(root, "data");
        ESP_LOGI(TAG, "Ended conversation %s state=%s endedAt=%s", conversation_id.c_str(),
                 JsonString(cJSON_GetObjectItem(data, "deviceState")).c_str(),
                 JsonString(cJSON_GetObjectItem(data, "endedAt")).c_str());
        cJSON_Delete(root);
    }
    return ok;
}

bool KidsEnglishProtocol::ParseConversationResponse(const cJSON* root, ConversationResponse& result) {
    auto data = cJSON_GetObjectItem(root, "data");
    auto conversation_id = cJSON_GetObjectItem(data, "conversationId");
    auto turn_id = cJSON_GetObjectItem(data, "turnId");
    auto device_state = cJSON_GetObjectItem(data, "deviceState");
    auto screen_cue = cJSON_GetObjectItem(data, "screenCue");
    auto tts_audio_url = cJSON_GetObjectItem(data, "ttsAudioUrl");
    auto audio_format = cJSON_GetObjectItem(data, "audioFormat");
    auto should_continue_listening = cJSON_GetObjectItem(data, "shouldContinueListening");
    auto diagnostics = cJSON_GetObjectItem(data, "diagnostics");
    auto request_id = cJSON_GetObjectItem(diagnostics, "requestId");
    auto asr_duration_ms = cJSON_GetObjectItem(diagnostics, "asrDurationMs");
    auto llm_duration_ms = cJSON_GetObjectItem(diagnostics, "llmDurationMs");
    auto tts_duration_ms = cJSON_GetObjectItem(diagnostics, "ttsDurationMs");
    auto total_duration_ms = cJSON_GetObjectItem(diagnostics, "totalDurationMs");
    auto provider_fallback = cJSON_GetObjectItem(diagnostics, "providerFallback");
    auto provider_error_code = cJSON_GetObjectItem(diagnostics, "providerErrorCode");

    if (!cJSON_IsString(conversation_id) || !cJSON_IsString(device_state) ||
        !cJSON_IsString(tts_audio_url)) {
        ESP_LOGE(TAG, "Invalid conversation response");
        return false;
    }

    result.conversation_id = conversation_id->valuestring;
    result.turn_id = JsonString(turn_id);
    result.device_state = device_state->valuestring;
    result.screen_cue = JsonString(screen_cue);
    result.tts_audio_url = JsonString(tts_audio_url);
    result.audio_format = JsonString(audio_format);
    result.should_continue_listening = JsonBool(should_continue_listening, true);
    result.request_id = JsonString(request_id);
    result.asr_duration_ms = JsonInt(asr_duration_ms);
    result.llm_duration_ms = JsonInt(llm_duration_ms);
    result.tts_duration_ms = JsonInt(tts_duration_ms);
    result.total_duration_ms = JsonInt(total_duration_ms);
    result.provider_fallback = JsonBool(provider_fallback);
    result.provider_error_code = JsonString(provider_error_code);
    return true;
}

bool KidsEnglishProtocol::ParseStandaloneTtsResponse(const cJSON* root,
                                                     StandaloneTtsResponse& result) {
    auto data = cJSON_GetObjectItem(root, "data");
    auto tts_audio_url = cJSON_GetObjectItem(data, "ttsAudioUrl");
    auto audio_format = cJSON_GetObjectItem(data, "audioFormat");
    auto diagnostics = cJSON_GetObjectItem(data, "diagnostics");
    auto request_id = cJSON_GetObjectItem(diagnostics, "requestId");
    auto tts_duration_ms = cJSON_GetObjectItem(diagnostics, "ttsDurationMs");
    auto total_duration_ms = cJSON_GetObjectItem(diagnostics, "totalDurationMs");
    auto provider_fallback = cJSON_GetObjectItem(diagnostics, "providerFallback");
    auto provider_error_code = cJSON_GetObjectItem(diagnostics, "providerErrorCode");

    if (!cJSON_IsString(tts_audio_url)) {
        ESP_LOGE(TAG, "Invalid standalone TTS response");
        return false;
    }

    result.tts_audio_url = tts_audio_url->valuestring;
    result.audio_format = JsonString(audio_format);
    result.request_id = JsonString(request_id);
    result.tts_duration_ms = JsonInt(tts_duration_ms);
    result.total_duration_ms = JsonInt(total_duration_ms);
    result.provider_fallback = JsonBool(provider_fallback);
    result.provider_error_code = JsonString(provider_error_code);
    return true;
}

bool KidsEnglishProtocol::ParseStandaloneAsrResponse(const cJSON* root,
                                                     StandaloneAsrResponse& result) {
    auto data = cJSON_GetObjectItem(root, "data");
    auto transcript = cJSON_GetObjectItem(data, "transcript");
    auto audio_format = cJSON_GetObjectItem(data, "audioFormat");
    auto diagnostics = cJSON_GetObjectItem(data, "diagnostics");
    auto request_id = cJSON_GetObjectItem(diagnostics, "requestId");
    auto asr_duration_ms = cJSON_GetObjectItem(diagnostics, "asrDurationMs");
    auto total_duration_ms = cJSON_GetObjectItem(diagnostics, "totalDurationMs");

    if (!cJSON_IsString(transcript)) {
        ESP_LOGE(TAG, "Invalid standalone ASR response");
        return false;
    }

    result.transcript = transcript->valuestring;
    result.audio_format = JsonString(audio_format);
    result.request_id = JsonString(request_id);
    result.asr_duration_ms = JsonInt(asr_duration_ms);
    result.total_duration_ms = JsonInt(total_duration_ms);
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
            if (std::memcmp(chunk, "data", 4) == 0) {
                ESP_LOGW(TAG, "WAV data chunk size %u exceeds remaining %u; using remaining bytes",
                         (unsigned)chunk_size, (unsigned)(wav.size() - offset));
                chunk_size = wav.size() - offset;
            } else {
                ESP_LOGE(TAG, "Truncated WAV chunk");
                return false;
            }
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

        size_t next_offset = offset + chunk_size + (chunk_size & 1);
        if (next_offset <= offset) {
            ESP_LOGE(TAG, "Invalid WAV chunk offset");
            return false;
        }
        offset = next_offset;
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

bool KidsEnglishProtocol::DownloadWavAudio(const std::string& url, std::vector<int16_t>& pcm,
                                           int& sample_rate, std::string* content_type) {
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
    std::string response_content_type = http->GetResponseHeader("Content-Type");
    std::string body;
    bool read_ok = ReadHttpBody(http.get(), body);
    http->Close();
    if (content_type != nullptr) {
        *content_type = response_content_type;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "TTS download failed, status=%d, bytes=%u", status, (unsigned)body.size());
        return false;
    }
    if (!read_ok) {
        ESP_LOGE(TAG, "Failed to read TTS audio body");
        return false;
    }

    if (!ParseWavPcm16Mono(body, pcm, sample_rate)) {
        ESP_LOGE(TAG, "TTS response is not playable WAV, content_type=%s",
                 response_content_type.c_str());
        return false;
    }

    ESP_LOGI(TAG, "Downloaded TTS WAV: content_type=%s sample_rate=%d samples=%u",
             response_content_type.c_str(), sample_rate, (unsigned)pcm.size());
    return true;
}

bool KidsEnglishProtocol::DownloadAndPlayTtsAudio(const std::string& url) {
    std::vector<int16_t> pcm;
    int sample_rate = 0;
    if (!DownloadWavAudio(url, pcm, sample_rate)) {
        return false;
    }

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

bool KidsEnglishProtocol::HandleConversationResponse(const ConversationResponse& response,
                                                     const char* fallback_text) {
    ESP_LOGI(TAG,
             "Conversation response: conversationId=%s turnId=%s state=%s cue=%s tts=%s "
             "format=%s continue=%s requestId=%s asr=%d llm=%d ttsMs=%d total=%d "
             "providerFallback=%s providerErrorCode=%s",
             response.conversation_id.c_str(), response.turn_id.c_str(),
             response.device_state.c_str(), response.screen_cue.c_str(),
             response.tts_audio_url.c_str(), response.audio_format.c_str(),
             response.should_continue_listening ? "true" : "false", response.request_id.c_str(),
             response.asr_duration_ms, response.llm_duration_ms, response.tts_duration_ms,
             response.total_duration_ms, response.provider_fallback ? "true" : "false",
             response.provider_error_code.c_str());

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!response.conversation_id.empty()) {
            conversation_id_ = response.conversation_id;
            session_id_ = conversation_id_;
        }
        if (!response.turn_id.empty()) {
            last_turn_id_ = response.turn_id;
        }
    }

    EmitTtsMessage("start");
    EmitAssistantMessage(fallback_text == nullptr ? "Let's keep talking." : fallback_text);
    bool played = true;
    if (!response.tts_audio_url.empty()) {
        ESP_LOGI(TAG, "Conversation ttsAudioUrl: %s", response.tts_audio_url.c_str());
        if (!DownloadAndPlayTtsAudio(response.tts_audio_url)) {
            ESP_LOGW(TAG, "Failed to download/play TTS audio; falling back to text-only feedback");
            played = false;
        }
    } else {
        played = false;
    }

    if (!response.should_continue_listening) {
        std::lock_guard<std::mutex> lock(mutex_);
        conversation_id_.clear();
        session_id_.clear();
    }
    EmitTtsMessage("stop", fallback_text, response.should_continue_listening);
    EmitEmotion(response.should_continue_listening ? "happy" : "neutral");
    return played;
}

bool KidsEnglishProtocol::IsConversationEndedError(const cJSON* root) const {
    auto error = cJSON_GetObjectItem(root, "error");
    auto code = cJSON_GetObjectItem(error, "code");
    return JsonString(code) == "CONVERSATION_ENDED";
}

void KidsEnglishProtocol::EmitTtsMessage(const char* state, const char* text,
                                         bool continue_listening) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "tts");
    cJSON_AddStringToObject(root, "state", state);
    if (text != nullptr) {
        cJSON_AddStringToObject(root, "text", text);
    }
    cJSON_AddBoolToObject(root, "continue_listening", continue_listening);
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

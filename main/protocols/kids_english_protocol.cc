#include "kids_english_protocol.h"

#include "application.h"
#include "assets/lang_config.h"
#include "audio_service.h"
#include "board.h"
#include "settings.h"

#include <esp_ae_rate_cvt.h>
#include <esp_app_desc.h>
#include <esp_audio_types.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/task.h>
#include <http.h>
#include <mbedtls/md.h>
#include <web_socket.h>

#include <sys/time.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>

#define TAG "KidsEnglish"

#ifndef CONFIG_KIDS_ENGLISH_SERVER_URL
#define CONFIG_KIDS_ENGLISH_SERVER_URL ""
#endif

#ifndef CONFIG_KIDS_ENGLISH_DEVELOPMENT_SERVER_URL
#define CONFIG_KIDS_ENGLISH_DEVELOPMENT_SERVER_URL "http://192.168.2.152:3000"
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
constexpr int kUploadTimeoutMs = 90000;
constexpr int kStandaloneSpeechTimeoutMs = 20000;
constexpr int kTtsDownloadTimeoutMs = 10000;
constexpr int kWsSessionReadyTimeoutMs = 10000;
constexpr int kWsConversationStartTimeoutMs = 30000;
constexpr int kWsSendTimeoutMs = 2000;
constexpr int kWsSlowSendLogMs = 200;
constexpr int kWsHeartbeatMissesBeforeFallback = 3;
constexpr int kSelfTestPlaybackDrainTimeoutMs = 30000;
constexpr size_t kMaxWelcomeAudioCandidates = 3;
constexpr size_t kMaxWelcomeAudioCacheEntries = 3;
constexpr char kMultipartBoundary[] = "----xiaozhi-kids-english-boundary";
constexpr char kSelfTestSpeechText[] = "I like apples.";
constexpr const char* kSelfTestConversationTexts[] = {
    "I like apples.",        "My apple is red.",       "I have a small dog.",
    "The dog can run fast.", "I want to read a book.",
};
constexpr char kWsProtocol[] = "xiaozhi.conversation.ws.v1";
constexpr char kWsAudioMagic[] = "XZWS";
constexpr int kPracticeAudioSampleRate = 16000;
constexpr int kPracticeAudioChannels = 1;
constexpr int kPracticeAudioBitsPerSample = 16;
constexpr int kWsInputFrameDurationMs = 20;
constexpr int kWsInputFrameSamples = kPracticeAudioSampleRate * kWsInputFrameDurationMs / 1000;
constexpr int kWsInputFramePaceMs = 5;
constexpr size_t kWsOutputJitterMinChunks = 4;
constexpr int kWsOutputJitterMinMs = 1200;
constexpr size_t kWsOutputRebufferMinChunks = 4;
constexpr int kWsOutputRebufferMinMs = 1200;
constexpr size_t kWsAudioHeaderBytes = 16;
constexpr int kMaxPracticeAudioDurationSeconds = 10;
constexpr size_t kMaxPracticeAudioBytes = 5 * 1024 * 1024;
constexpr size_t kWavHeaderBytes = 44;
constexpr int64_t kUnixTimeReasonableMs = 1600000000000LL;
constexpr char kKidsEnglishSettingsNamespace[] = "kids_english";
constexpr char kKidsEnglishEnvironmentKey[] = "environment";
constexpr char kKidsEnglishEnvironmentProduction[] = "production";
constexpr char kKidsEnglishEnvironmentDevelopment[] = "development";
constexpr EventBits_t kWsSessionReadyEvent = BIT0;
constexpr EventBits_t kWsConversationStartedEvent = BIT1;
constexpr EventBits_t kWsTurnCompleteEvent = BIT2;
constexpr EventBits_t kWsReconnectRequiredEvent = BIT3;
constexpr EventBits_t kWsDisconnectedEvent = BIT4;
constexpr EventBits_t kWsAssistantAudioEndEvent = BIT5;
constexpr EventBits_t kWsConversationEndedEvent = BIT6;
constexpr EventBits_t kWsUrlAudioFinishedEvent = BIT7;
constexpr EventBits_t kWsPlaybackFinishedEvent = BIT8;

struct WsUrlAudioTaskArgs {
    KidsEnglishProtocol* self = nullptr;
    std::string url;
};

std::string JsonString(const cJSON* item) { return cJSON_IsString(item) ? item->valuestring : ""; }

int JsonInt(const cJSON* item, int fallback = -1) {
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

bool JsonBool(const cJSON* item, bool fallback = false) {
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return fallback;
}

std::string JsonCompactString(const cJSON* item) {
    if (item == nullptr) {
        return "{}";
    }
    char* printed = cJSON_PrintUnformatted(item);
    std::string text = printed == nullptr ? "{}" : printed;
    if (printed != nullptr) {
        cJSON_free(printed);
    }
    return text;
}

uint32_t LogMs(int64_t ms) { return ms > 0 ? static_cast<uint32_t>(ms) : 0; }

int LogDeltaMs(int64_t from, int64_t to) {
    return from > 0 && to > 0 ? static_cast<int>(to - from) : -1;
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
        mbedtls_md_update(&ctx, reinterpret_cast<const unsigned char*>(data.data()), data.size()) !=
            0 ||
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
        mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const unsigned char*>(key.data()),
                               key.size()) != 0 ||
        mbedtls_md_hmac_update(&ctx, reinterpret_cast<const unsigned char*>(data.data()),
                               data.size()) != 0 ||
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

void AppendBe16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

void AppendBe32(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
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

uint16_t ReadBe16(const char* data) {
    return (static_cast<uint16_t>(static_cast<uint8_t>(data[0])) << 8) |
           static_cast<uint16_t>(static_cast<uint8_t>(data[1]));
}

uint32_t ReadBe32(const char* data) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(data[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(data[3]));
}

bool ResamplePcm16Mono(std::vector<int16_t>& pcm, int source_sample_rate, int target_sample_rate) {
    if (source_sample_rate == target_sample_rate) {
        return true;
    }
    if (pcm.empty() || source_sample_rate <= 0 || target_sample_rate <= 0) {
        ESP_LOGE(TAG, "Invalid PCM resample request: samples=%u sourceRate=%d targetRate=%d",
                 (unsigned)pcm.size(), source_sample_rate, target_sample_rate);
        return false;
    }

    esp_ae_rate_cvt_handle_t resampler = nullptr;
    esp_ae_rate_cvt_cfg_t cfg = {
        .src_rate = static_cast<uint32_t>(source_sample_rate),
        .dest_rate = static_cast<uint32_t>(target_sample_rate),
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .complexity = 2,
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
    };
    auto ret = esp_ae_rate_cvt_open(&cfg, &resampler);
    if (resampler == nullptr) {
        ESP_LOGE(TAG, "Failed to create simulated recording resampler, error code: %d", ret);
        return false;
    }

    uint32_t target_samples = 0;
    esp_ae_rate_cvt_get_max_out_sample_num(resampler, pcm.size(), &target_samples);
    std::vector<int16_t> resampled(target_samples);
    uint32_t actual_output = target_samples;
    esp_ae_rate_cvt_process(resampler, reinterpret_cast<esp_ae_sample_t>(pcm.data()), pcm.size(),
                            reinterpret_cast<esp_ae_sample_t>(resampled.data()), &actual_output);
    esp_ae_rate_cvt_close(resampler);
    resampled.resize(actual_output);
    pcm = std::move(resampled);
    ESP_LOGI(TAG, "Resampled simulated recording PCM: sourceRate=%d targetRate=%d samples=%u",
             source_sample_rate, target_sample_rate, (unsigned)pcm.size());
    return true;
}

}  // namespace

KidsEnglishProtocol::KidsEnglishProtocol()
    : base_url_(GetConfiguredBaseUrl()),
      device_id_(CONFIG_KIDS_ENGLISH_DEVICE_ID),
      device_secret_(CONFIG_KIDS_ENGLISH_DEVICE_SECRET) {
    server_sample_rate_ = kPracticeAudioSampleRate;
    server_frame_duration_ = OPUS_FRAME_DURATION_MS;
    ws_event_group_ = xEventGroupCreate();
    ESP_LOGI(TAG, "Kids English environment: %s baseUrl=%s", GetConfiguredEnvironmentName().c_str(),
             base_url_.c_str());
}

KidsEnglishProtocol::~KidsEnglishProtocol() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        welcome_audio_destroying_ = true;
        welcome_audio_prefetch_pending_ = false;
    }
    while (true) {
        bool prefetch_in_progress = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            prefetch_in_progress = welcome_audio_prefetch_in_progress_;
        }
        if (!prefetch_in_progress) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    CloseAudioChannel(false);
    if (ws_event_group_ != nullptr) {
        vEventGroupDelete(ws_event_group_);
        ws_event_group_ = nullptr;
    }
}

KidsEnglishProtocol::Environment KidsEnglishProtocol::GetConfiguredEnvironment() {
    Settings settings(kKidsEnglishSettingsNamespace, false);
    auto environment =
        settings.GetString(kKidsEnglishEnvironmentKey, kKidsEnglishEnvironmentProduction);
    if (environment == kKidsEnglishEnvironmentDevelopment) {
        return Environment::kDevelopment;
    }
    return Environment::kProduction;
}

std::string KidsEnglishProtocol::GetConfiguredEnvironmentName() {
    return GetEnvironmentName(GetConfiguredEnvironment());
}

std::string KidsEnglishProtocol::GetConfiguredBaseUrl() {
    return GetBaseUrl(GetConfiguredEnvironment());
}

std::string KidsEnglishProtocol::GetEnvironmentName(Environment environment) {
    switch (environment) {
        case Environment::kDevelopment:
            return kKidsEnglishEnvironmentDevelopment;
        case Environment::kProduction:
        default:
            return kKidsEnglishEnvironmentProduction;
    }
}

std::string KidsEnglishProtocol::GetBaseUrl(Environment environment) {
    switch (environment) {
        case Environment::kDevelopment:
            return TrimTrailingSlash(CONFIG_KIDS_ENGLISH_DEVELOPMENT_SERVER_URL);
        case Environment::kProduction:
        default:
            return TrimTrailingSlash(CONFIG_KIDS_ENGLISH_SERVER_URL);
    }
}

void KidsEnglishProtocol::SetConfiguredEnvironment(Environment environment) {
    Settings settings(kKidsEnglishSettingsNamespace, true);
    settings.SetString(kKidsEnglishEnvironmentKey, GetEnvironmentName(environment));
}

bool KidsEnglishProtocol::Start() {
    if (on_connected_ != nullptr) {
        on_connected_();
    }
    PrefetchWelcomeAudioAsync(true);
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

    bool opened = false;
    ConversationTransportMode selected_transport;
    WebSocketTransportConfig ws_config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selected_transport = selected_transport_;
        ws_config = ws_config_;
    }
    if (selected_transport == ConversationTransportMode::kWebSocket) {
        opened = OpenWebSocketConversation(trigger.c_str());
        if (!opened) {
            ESP_LOGW(TAG, "Kids English WebSocket conversation unavailable");
            CloseWebSocket();
        }
    } else {
        opened = StartConversation(trigger.c_str());
    }
    if (!opened) {
        return false;
    }
    EmitConversationStarted();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_audio_.clear();
        pending_pcm_.clear();
        channel_opened_ = true;
    }
    FlushPendingTtsStop();

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
        pending_tts_stop_ = false;
        pending_tts_stop_text_.clear();
        pending_tts_stop_end_reason_.clear();
        pending_tts_stop_continue_listening_ = true;
    }

    ConversationTransportMode transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = selected_transport_;
    }
    if (send_goodbye && !conversation_to_end.empty()) {
        if (transport == ConversationTransportMode::kWebSocket) {
            SendWsConversationEnd(conversation_to_end, "device_end");
        } else {
            EndConversation(conversation_to_end);
        }
    }
    CloseWebSocket();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selected_transport_ = ConversationTransportMode::kHttp;
    }

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool KidsEnglishProtocol::IsAudioChannelOpened() const {
    ConversationTransportMode transport;
    bool opened = false;
    bool ws_connected = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        opened = channel_opened_ && !error_occurred_;
        transport = selected_transport_;
        ws_connected = IsWebSocketConnectedLocked();
    }
    if (!opened) {
        return false;
    }
    if (transport == ConversationTransportMode::kWebSocket) {
        return ws_connected && !IsWebSocketHeartbeatTimedOut();
    }
    return true;
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

bool KidsEnglishProtocol::SubmitPcmAudio(std::vector<int16_t>&& pcm) {
    if (pcm.empty()) {
        ESP_LOGW(TAG, "No PCM audio captured for current prompt");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!channel_opened_ || upload_in_progress_) {
            ESP_LOGW(TAG, "Cannot submit Kids English PCM: channelOpen=%s uploadInProgress=%s",
                     channel_opened_ ? "true" : "false", upload_in_progress_ ? "true" : "false");
            return false;
        }
        upload_in_progress_ = true;
    }

    bool ok = UploadPcmAudioForCurrentConversation(std::move(pcm));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_audio_.clear();
        pending_pcm_.clear();
        upload_in_progress_ = false;
    }

    if (!ok) {
        SetError(Lang::Strings::SERVER_ERROR);
    }
    return ok;
}

bool KidsEnglishProtocol::GenerateSimulatedRecordingPcm(const std::string& text,
                                                        std::vector<int16_t>& pcm) {
    ESP_LOGI(TAG, "Generating simulated Kids English recording PCM: %s", text.c_str());
    pcm.clear();
    error_occurred_ = false;
    if (base_url_.empty()) {
        SetError("Kids English server URL is empty");
        return false;
    }

    StandaloneTtsResponse tts;
    if (!RequestStandaloneTts(text, tts)) {
        ESP_LOGE(TAG, "Simulated recording PCM generation failed at TTS step");
        return false;
    }

    int sample_rate = 0;
    if (!DownloadWavAudio(tts.tts_audio_url, pcm, sample_rate)) {
        ESP_LOGE(TAG, "Simulated recording PCM generation failed to download generated speech");
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    if (sample_rate != kPracticeAudioSampleRate) {
        ESP_LOGW(TAG, "Simulated recording sample rate mismatch: %d; resampling to %d", sample_rate,
                 kPracticeAudioSampleRate);
        if (!ResamplePcm16Mono(pcm, sample_rate, kPracticeAudioSampleRate)) {
            SetError(Lang::Strings::SERVER_ERROR);
            return false;
        }
    }

    ESP_LOGI(TAG, "Generated simulated recording PCM: samples=%u durationMs=%u",
             (unsigned)pcm.size(), (unsigned)(pcm.size() * 1000 / kPracticeAudioSampleRate));
    return true;
}

bool KidsEnglishProtocol::RunSelfTest() {
    ESP_LOGI(TAG, "KIDS_ENGLISH_SELF_TEST_BEGIN baseUrl=%s deviceId=%s", base_url_.c_str(),
             device_id_.c_str());
    error_occurred_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        self_test_in_progress_ = true;
    }
    auto finish_self_test = [this](bool ok) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            self_test_in_progress_ = false;
        }
        return ok;
    };

    if (base_url_.empty()) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL reason=missing_server_url");
        SetError("Kids English server URL is empty");
        return finish_self_test(false);
    }

    if (!CheckHealth()) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=health");
        return finish_self_test(false);
    }
    ESP_LOGI(TAG, "KIDS_ENGLISH_SELF_TEST_STEP health ok");

    if (!DeviceHello()) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=device_hello");
        return finish_self_test(false);
    }
    ESP_LOGI(TAG, "KIDS_ENGLISH_SELF_TEST_STEP device_hello ok");

    StandaloneTtsResponse standalone_tts;
    if (!RequestStandaloneTts(kSelfTestSpeechText, standalone_tts)) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=standalone_tts");
        return finish_self_test(false);
    }
    ESP_LOGI(TAG,
             "KIDS_ENGLISH_SELF_TEST_STEP standalone_tts ok requestId=%s format=%s "
             "ttsTextPresent=%s ttsText=%s ttsUrl=%s providerFallback=%s providerErrorCode=%s",
             standalone_tts.request_id.c_str(), standalone_tts.audio_format.c_str(),
             standalone_tts.has_tts_text ? "true" : "false", standalone_tts.tts_text.c_str(),
             RedactUrlForLog(standalone_tts.tts_audio_url).c_str(),
             standalone_tts.provider_fallback ? "true" : "false",
             standalone_tts.provider_error_code.c_str());

    std::vector<int16_t> self_test_pcm;
    int self_test_sample_rate = 0;
    if (!DownloadWavAudio(standalone_tts.tts_audio_url, self_test_pcm, self_test_sample_rate)) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=download_standalone_tts");
        return finish_self_test(false);
    }
    if (self_test_sample_rate != kPracticeAudioSampleRate) {
        ESP_LOGW(TAG,
                 "KIDS_ENGLISH_SELF_TEST_STEP download_standalone_tts sampleRate=%d; "
                 "resampling to %d",
                 self_test_sample_rate, kPracticeAudioSampleRate);
        if (!ResamplePcm16Mono(self_test_pcm, self_test_sample_rate, kPracticeAudioSampleRate)) {
            ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=resample_standalone_tts sampleRate=%d",
                     self_test_sample_rate);
            return finish_self_test(false);
        }
        self_test_sample_rate = kPracticeAudioSampleRate;
    }
    std::string self_test_wav = CreateWavFile(self_test_pcm, self_test_sample_rate);
    ESP_LOGI(TAG, "KIDS_ENGLISH_SELF_TEST_STEP download_standalone_tts ok samples=%u wavBytes=%u",
             (unsigned)self_test_pcm.size(), (unsigned)self_test_wav.size());

    StandaloneAsrResponse standalone_asr;
    if (!UploadStandaloneAsrAudio(self_test_wav, kSelfTestSpeechText, standalone_asr)) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=standalone_asr");
        return finish_self_test(false);
    }
    ESP_LOGI(TAG,
             "KIDS_ENGLISH_SELF_TEST_STEP standalone_asr ok requestId=%s transcript=%s "
             "asrDurationMs=%d totalDurationMs=%d",
             standalone_asr.request_id.c_str(), standalone_asr.transcript.c_str(),
             standalone_asr.asr_duration_ms, standalone_asr.total_duration_ms);

    ConversationTransportMode self_test_transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        self_test_transport = selected_transport_;
    }

    bool started = self_test_transport == ConversationTransportMode::kWebSocket
                       ? OpenWebSocketConversation("manual")
                       : StartConversation("manual");
    if (!started) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=start_conversation");
        return finish_self_test(false);
    }

    std::string conversation_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        conversation_id = conversation_id_;
        channel_opened_ = true;
    }
    FlushPendingTtsStop();
    if (conversation_id.empty()) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=start_conversation reason=missing_id");
        return finish_self_test(false);
    }
    ESP_LOGI(TAG,
             "KIDS_ENGLISH_SELF_TEST_STEP start_conversation ok conversationId=%s transport=%s",
             conversation_id.c_str(),
             self_test_transport == ConversationTransportMode::kWebSocket ? "websocket" : "http");

    auto wait_for_reply_playback = [this, self_test_transport](const char* step,
                                                               unsigned turn) -> bool {
        if (self_test_transport == ConversationTransportMode::kWebSocket) {
            bool wait_url_audio = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                wait_url_audio = ws_url_audio_in_progress_;
            }
            if (wait_url_audio) {
                EventBits_t bits = xEventGroupWaitBits(
                    ws_event_group_,
                    kWsUrlAudioFinishedEvent | kWsReconnectRequiredEvent | kWsDisconnectedEvent,
                    pdTRUE, pdFALSE, pdMS_TO_TICKS(kWsConversationStartTimeoutMs));
                if (!(bits & kWsUrlAudioFinishedEvent)) {
                    ESP_LOGE(TAG,
                             "KIDS_ENGLISH_SELF_TEST_FAIL step=%s turn=%u "
                             "reason=wait_url_audio",
                             step, turn);
                    return false;
                }
            }
        }
        bool drained = self_test_transport == ConversationTransportMode::kWebSocket
                           ? WaitForWsPlaybackFinished(step, turn, kSelfTestPlaybackDrainTimeoutMs)
                           : Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty(
                                 kSelfTestPlaybackDrainTimeoutMs);
        if (!drained) {
            ESP_LOGE(TAG,
                     "KIDS_ENGLISH_SELF_TEST_FAIL step=%s turn=%u reason=playback_drain_timeout "
                     "timeoutMs=%d",
                     step, turn, kSelfTestPlaybackDrainTimeoutMs);
        }
        return drained;
    };

    if (!wait_for_reply_playback("initial_audio_playback", 0)) {
        return finish_self_test(false);
    }

    constexpr size_t self_test_turn_count = std::size(kSelfTestConversationTexts);
    ESP_LOGI(TAG,
             "KIDS_ENGLISH_SELF_TEST_STEP conversation_turns begin conversationId=%s turns=%u "
             "transport=%s",
             conversation_id.c_str(), (unsigned)self_test_turn_count,
             self_test_transport == ConversationTransportMode::kWebSocket ? "websocket" : "http");
    std::vector<int16_t> ended_upload_pcm;
    for (size_t turn_index = 0; turn_index < self_test_turn_count; ++turn_index) {
        const char* turn_text = kSelfTestConversationTexts[turn_index];
        std::vector<int16_t> turn_pcm;
        if (!GenerateSimulatedRecordingPcm(turn_text, turn_pcm)) {
            ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=generate_turn_pcm turn=%u text=%s",
                     (unsigned)(turn_index + 1), turn_text);
            return finish_self_test(false);
        }
        if (self_test_transport == ConversationTransportMode::kHttp && turn_index == 0) {
            ended_upload_pcm = turn_pcm;
        }

        ESP_LOGI(TAG,
                 "KIDS_ENGLISH_SELF_TEST_STEP conversation_turn begin turn=%u/%u "
                 "conversationId=%s text=%s",
                 (unsigned)(turn_index + 1), (unsigned)self_test_turn_count,
                 conversation_id.c_str(), turn_text);
        if (self_test_transport == ConversationTransportMode::kWebSocket) {
            if (!UploadPcmAudioWebSocket(turn_pcm, conversation_id)) {
                ESP_LOGE(TAG,
                         "KIDS_ENGLISH_SELF_TEST_FAIL step=upload_audio turn=%u "
                         "transport=websocket",
                         (unsigned)(turn_index + 1));
                return finish_self_test(false);
            }
        } else {
            UploadResult upload = UploadPcmAudio(std::move(turn_pcm), conversation_id);
            if (upload != UploadResult::kSuccess) {
                ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=upload_audio turn=%u result=%d",
                         (unsigned)(turn_index + 1), static_cast<int>(upload));
                return finish_self_test(false);
            }
        }
        if (!wait_for_reply_playback("conversation_turn_playback", (unsigned)(turn_index + 1))) {
            return finish_self_test(false);
        }
        ESP_LOGI(TAG,
                 "KIDS_ENGLISH_SELF_TEST_STEP conversation_turn ok turn=%u/%u "
                 "conversationId=%s",
                 (unsigned)(turn_index + 1), (unsigned)self_test_turn_count,
                 conversation_id.c_str());
    }
    ESP_LOGI(TAG,
             "KIDS_ENGLISH_SELF_TEST_STEP upload_audio ok conversationId=%s turns=%u transport=%s",
             conversation_id.c_str(), (unsigned)self_test_turn_count,
             self_test_transport == ConversationTransportMode::kWebSocket ? "websocket" : "http");

    bool ended = self_test_transport == ConversationTransportMode::kWebSocket
                     ? SendWsConversationEnd(conversation_id, "device_end")
                     : EndConversation(conversation_id);
    if (!ended) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=end_conversation");
        return finish_self_test(false);
    }
    ESP_LOGI(TAG, "KIDS_ENGLISH_SELF_TEST_STEP end_conversation ok conversationId=%s transport=%s",
             conversation_id.c_str(),
             self_test_transport == ConversationTransportMode::kWebSocket ? "websocket" : "http");

    if (self_test_transport == ConversationTransportMode::kHttp) {
        UploadResult ended_upload = UploadPcmAudio(std::move(ended_upload_pcm), conversation_id);
        if (ended_upload != UploadResult::kConversationEnded) {
            ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=ended_upload expected=409 result=%d",
                     static_cast<int>(ended_upload));
            return finish_self_test(false);
        }
    } else {
        ESP_LOGI(TAG,
                 "KIDS_ENGLISH_SELF_TEST_STEP ended_upload skip transport=websocket "
                 "conversationId=%s reason=avoid_post_end_http_upload",
                 conversation_id.c_str());
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        channel_opened_ = false;
        pending_audio_.clear();
        pending_pcm_.clear();
        conversation_id_.clear();
        session_id_.clear();
        selected_transport_ = ConversationTransportMode::kHttp;
    }
    CloseWebSocket();
    ESP_LOGI(TAG, "KIDS_ENGLISH_SELF_TEST_PASS conversationId=%s", conversation_id.c_str());
    return finish_self_test(true);
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
    if (text.empty()) {
        return true;
    }
    if (text[0] == '{') {
        ESP_LOGI(TAG, "Ignoring JSON command in kids English HTTP protocol: %s", text.c_str());
        return true;
    }

    StandaloneTtsResponse response;
    if (!RequestStandaloneTts(text, response)) {
        return false;
    }
    return HandleStandaloneTtsResponse(response, true);
}

std::string KidsEnglishProtocol::BuildUrl(const char* path) const { return base_url_ + path; }

void KidsEnglishProtocol::AddDeviceAuthHeaders(Http* http, const std::string& method,
                                               const std::string& path,
                                               const std::string& body_sha256) {
    if (device_id_.empty() || device_secret_.empty()) {
        ESP_LOGW(TAG, "Kids English device ID or secret is empty");
    }

    std::string timestamp = CurrentUnixMillisString();
    uint32_t random_a = esp_random();
    uint32_t random_b = esp_random();
    std::string nonce = Board::GetInstance().GetUuid() + "-" +
                        std::to_string(esp_timer_get_time()) + "-" + std::to_string(random_a) +
                        std::to_string(random_b);
    std::string payload =
        method + "\n" + path + "\n" + timestamp + "\n" + nonce + "\n" + body_sha256;
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
    body += "\"streamingAudio\":{";
    body += "\"bargeIn\":false,";
    body += "\"binaryFrames\":true,";
    body += "\"frameDurationMs\":[20,40],";
    body += "\"inputFormats\":[\"pcm_s16le_16k_mono\",\"wav_16k_mono\"],";
    body += "\"outputFormats\":[\"pcm_s16le_16k_mono\",\"wav_16k_mono\"],";
    body += "\"resume\":true";
    body += "},";
    body += "\"touchScreen\":true,";
    body += "\"transports\":{";
    body += "\"http\":true,";
    body += "\"websocket\":true";
    body += "},";
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

std::string KidsEnglishProtocol::BuildWsJsonEnvelope(const std::string& type, cJSON* payload,
                                                     const std::string& conversation_id,
                                                     const std::string& turn_id,
                                                     bool include_client_ts) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "type", type.c_str());

    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_id = session_id_;
    }
    if (!session_id.empty()) {
        cJSON_AddStringToObject(root, "sessionId", session_id.c_str());
    }
    if (!conversation_id.empty()) {
        cJSON_AddStringToObject(root, "conversationId", conversation_id.c_str());
    }
    if (!turn_id.empty()) {
        cJSON_AddStringToObject(root, "turnId", turn_id.c_str());
    }
    cJSON_AddNumberToObject(root, "seq", NextWsSeq());
    if (include_client_ts) {
        cJSON_AddStringToObject(root, "clientTs", CurrentIsoTimestamp().c_str());
    }
    if (payload == nullptr) {
        payload = cJSON_CreateObject();
    }
    cJSON_AddItemToObject(root, "payload", payload);

    char* printed = cJSON_PrintUnformatted(root);
    std::string message = printed == nullptr ? "{}" : printed;
    if (printed != nullptr) {
        cJSON_free(printed);
    }
    cJSON_Delete(root);
    return message;
}

std::string KidsEnglishProtocol::BuildStandaloneAsrMultipartBody(
    const std::string& boundary, const std::string& wav, const std::string& prompt_text) const {
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
    body.reserve(boundary.size() * 6 + wav.size() + client_turn_id.size() + recorded_at.size() +
                 256);
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

int64_t KidsEnglishProtocol::NowMs() const { return esp_timer_get_time() / 1000; }

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
    auto transport = cJSON_GetObjectItem(data, "conversationTransport");
    auto transport_selected = cJSON_GetObjectItem(transport, "selected");
    auto transport_reason = cJSON_GetObjectItem(transport, "reason");
    auto transport_protocol = cJSON_GetObjectItem(transport, "protocol");

    ESP_LOGI(TAG,
             "Device hello: deviceId=%s state=%s languageLevel=%s voice=%s "
             "dailyPracticeMinutes=%d audioUpload=%s ttsUrl=%s streaming=%s "
             "conversationTransport.selected=%s reason=%s protocol=%s",
             device_id_.c_str(), JsonString(device_state).c_str(),
             JsonString(cJSON_GetObjectItem(settings, "languageLevel")).c_str(),
             JsonString(cJSON_GetObjectItem(settings, "voice")).c_str(),
             JsonInt(cJSON_GetObjectItem(settings, "dailyPracticeMinutes"), 0),
             cJSON_IsTrue(audio_upload) ? "true" : "false",
             cJSON_IsTrue(tts_url) ? "true" : "false", cJSON_IsTrue(streaming) ? "true" : "false",
             JsonString(transport_selected).c_str(), JsonString(transport_reason).c_str(),
             JsonString(transport_protocol).c_str());
}

std::vector<KidsEnglishProtocol::WelcomeAudioCandidate>
KidsEnglishProtocol::ParseWelcomeAudioCandidates(const cJSON* root) const {
    std::vector<WelcomeAudioCandidate> candidates;
    auto data = cJSON_GetObjectItem(root, "data");
    auto candidate_array = cJSON_GetObjectItem(data, "welcomeAudioCandidates");
    if (!cJSON_IsArray(candidate_array)) {
        ESP_LOGI(TAG, "WELCOME_AUDIO_CANDIDATES total=0 ready=0 usable=0");
        return candidates;
    }

    int total = cJSON_GetArraySize(candidate_array);
    int ready = 0;
    int usable = 0;
    cJSON* item = nullptr;
    cJSON_ArrayForEach (item, candidate_array) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        WelcomeAudioCandidate candidate;
        candidate.id = JsonString(cJSON_GetObjectItem(item, "id"));
        candidate.tts_text = JsonString(cJSON_GetObjectItem(item, "ttsText"));
        candidate.tts_audio_url =
            ResolveAudioUrl(JsonString(cJSON_GetObjectItem(item, "ttsAudioUrl")));
        candidate.audio_format = JsonString(cJSON_GetObjectItem(item, "audioFormat"));
        candidate.status = JsonString(cJSON_GetObjectItem(item, "status"));
        candidate.cache_key = JsonString(cJSON_GetObjectItem(item, "cacheKey"));
        candidate.voice = JsonString(cJSON_GetObjectItem(item, "voice"));
        bool is_ready = candidate.status == "ready";
        bool is_usable = is_ready && !candidate.tts_audio_url.empty() &&
                         candidate.audio_format == "wav" && !candidate.cache_key.empty();
        if (is_ready) {
            ++ready;
        }
        if (is_usable) {
            ++usable;
            if (candidates.size() < kMaxWelcomeAudioCandidates) {
                candidates.push_back(std::move(candidate));
            }
        }
    }

    ESP_LOGI(TAG, "WELCOME_AUDIO_CANDIDATES total=%d ready=%d usable=%d stored=%u", total, ready,
             usable, (unsigned)candidates.size());
    return candidates;
}

void KidsEnglishProtocol::StoreWelcomeAudioCandidates(
    std::vector<WelcomeAudioCandidate>&& candidates, const std::string& voice,
    bool schedule_prefetch) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (voice != welcome_audio_voice_) {
            ESP_LOGI(TAG, "WELCOME_AUDIO_CACHE_CLEAR oldVoice=%s newVoice=%s",
                     welcome_audio_voice_.c_str(), voice.c_str());
            welcome_audio_cache_.clear();
            welcome_audio_voice_ = voice;
        }
        for (auto& candidate : candidates) {
            if (candidate.voice.empty()) {
                candidate.voice = voice;
            }
        }
        welcome_audio_candidates_ = std::move(candidates);
    }
    if (schedule_prefetch) {
        PrefetchWelcomeAudioAsync(false);
    }
}

void KidsEnglishProtocol::PrefetchWelcomeAudioAsync(bool refresh_device_hello) {
    bool should_start = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (welcome_audio_destroying_) {
            return;
        }
        welcome_audio_refresh_requested_ = welcome_audio_refresh_requested_ || refresh_device_hello;
        if (welcome_audio_prefetch_in_progress_) {
            welcome_audio_prefetch_pending_ = true;
            ESP_LOGD(TAG, "WELCOME_AUDIO_PREFETCH_PENDING");
            return;
        }
        welcome_audio_prefetch_in_progress_ = true;
        should_start = true;
    }
    if (!should_start) {
        return;
    }

    BaseType_t created = xTaskCreate(
        [](void* arg) {
            auto* self = static_cast<KidsEnglishProtocol*>(arg);
            if (self != nullptr) {
                self->PrefetchWelcomeAudioTask();
            }
            vTaskDelete(NULL);
        },
        "kids_welcome_audio", 4096 * 2, this, 2, nullptr);
    if (created != pdPASS) {
        std::lock_guard<std::mutex> lock(mutex_);
        welcome_audio_prefetch_in_progress_ = false;
        welcome_audio_prefetch_pending_ = false;
        ESP_LOGW(TAG, "WELCOME_AUDIO_PREFETCH_TASK_FAILED");
    }
}

void KidsEnglishProtocol::PrefetchWelcomeAudioTask() {
    while (true) {
        bool refresh_device_hello = false;
        bool should_stop = false;
        std::vector<WelcomeAudioCandidate> candidates;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            should_stop = welcome_audio_destroying_;
            if (should_stop) {
                welcome_audio_prefetch_in_progress_ = false;
                welcome_audio_prefetch_pending_ = false;
                welcome_audio_refresh_requested_ = false;
            }
            refresh_device_hello = welcome_audio_refresh_requested_;
            welcome_audio_refresh_requested_ = false;
            candidates = welcome_audio_candidates_;
            welcome_audio_prefetch_pending_ = false;
        }
        if (should_stop) {
            break;
        }

        if (refresh_device_hello) {
            if (!CheckHealth(false) || !DeviceHello(false, false, false)) {
                ESP_LOGW(TAG, "WELCOME_AUDIO_PREFETCH_HELLO_FAILED");
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                candidates = welcome_audio_candidates_;
            }
        }

        for (const auto& candidate : candidates) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (welcome_audio_destroying_) {
                    break;
                }
                if (IsWelcomeAudioCachedLocked(candidate.cache_key, candidate.voice)) {
                    ESP_LOGI(TAG, "WELCOME_AUDIO_PREFETCH_SKIP cacheKey=%s voice=%s reason=cached",
                             candidate.cache_key.c_str(), candidate.voice.c_str());
                    continue;
                }
            }

            ESP_LOGI(TAG, "WELCOME_AUDIO_PREFETCH_BEGIN cacheKey=%s voice=%s url=%s",
                     candidate.cache_key.c_str(), candidate.voice.c_str(),
                     RedactUrlForLog(candidate.tts_audio_url).c_str());
            std::vector<int16_t> pcm;
            int sample_rate = 0;
            if (!DownloadWavAudio(candidate.tts_audio_url, pcm, sample_rate)) {
                ESP_LOGW(TAG, "WELCOME_AUDIO_PREFETCH_FAILED cacheKey=%s voice=%s url=%s",
                         candidate.cache_key.c_str(), candidate.voice.c_str(),
                         RedactUrlForLog(candidate.tts_audio_url).c_str());
                continue;
            }
            size_t samples = pcm.size();
            if (StoreWelcomeAudioCache(candidate, std::move(pcm), sample_rate)) {
                ESP_LOGI(TAG,
                         "WELCOME_AUDIO_PREFETCH_OK cacheKey=%s voice=%s sampleRate=%d "
                         "samples=%u",
                         candidate.cache_key.c_str(), candidate.voice.c_str(), sample_rate,
                         (unsigned)samples);
            } else {
                ESP_LOGW(TAG, "WELCOME_AUDIO_PREFETCH_FAILED cacheKey=%s voice=%s reason=store",
                         candidate.cache_key.c_str(), candidate.voice.c_str());
            }
        }

        bool rerun = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rerun = (welcome_audio_prefetch_pending_ || welcome_audio_refresh_requested_) &&
                    !welcome_audio_destroying_;
            if (!rerun) {
                welcome_audio_prefetch_in_progress_ = false;
                welcome_audio_prefetch_pending_ = false;
                welcome_audio_refresh_requested_ = false;
            }
        }
        if (!rerun) {
            break;
        }
    }
}

bool KidsEnglishProtocol::IsWelcomeAudioCachedLocked(const std::string& cache_key,
                                                     const std::string& voice) const {
    if (cache_key.empty()) {
        return false;
    }
    return std::any_of(welcome_audio_cache_.begin(), welcome_audio_cache_.end(),
                       [&](const WelcomeAudioCacheEntry& entry) {
                           return entry.cache_key == cache_key && entry.voice == voice &&
                                  !entry.pcm.empty() && entry.sample_rate > 0;
                       });
}

bool KidsEnglishProtocol::StoreWelcomeAudioCache(const WelcomeAudioCandidate& candidate,
                                                 std::vector<int16_t>&& pcm, int sample_rate) {
    if (candidate.cache_key.empty() || pcm.empty() || sample_rate <= 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (welcome_audio_destroying_) {
        return false;
    }
    auto existing = std::find_if(welcome_audio_cache_.begin(), welcome_audio_cache_.end(),
                                 [&](const WelcomeAudioCacheEntry& entry) {
                                     return entry.cache_key == candidate.cache_key &&
                                            entry.voice == candidate.voice;
                                 });
    if (existing != welcome_audio_cache_.end()) {
        existing->pcm = std::move(pcm);
        existing->sample_rate = sample_rate;
        return true;
    }
    if (welcome_audio_cache_.size() >= kMaxWelcomeAudioCacheEntries) {
        welcome_audio_cache_.erase(welcome_audio_cache_.begin());
    }
    WelcomeAudioCacheEntry entry;
    entry.cache_key = candidate.cache_key;
    entry.voice = candidate.voice;
    entry.pcm = std::move(pcm);
    entry.sample_rate = sample_rate;
    welcome_audio_cache_.push_back(std::move(entry));
    return true;
}

void KidsEnglishProtocol::LogWsOutputAudioConfig(const char* source,
                                                 const WebSocketTransportConfig& config) const {
    ESP_LOGI(TAG,
             "WS_NATIVE_OUTPUT_AUDIO_CONFIG source=%s sampleRateHz=%d channels=%d "
             "maxChunkBytes=%d pacingMs=%d backpressureHighWaterBytes=%d "
             "backpressureLowWaterBytes=%d",
             source == nullptr ? "unknown" : source, config.output_sample_rate_hz,
             config.output_channels, config.output_max_chunk_bytes, config.output_pacing_ms,
             config.output_backpressure_high_water_bytes,
             config.output_backpressure_low_water_bytes);
}

void KidsEnglishProtocol::ParseWsOutputAudioConfig(const cJSON* output_audio,
                                                   WebSocketTransportConfig& config) const {
    config.output_sample_rate_hz =
        JsonInt(cJSON_GetObjectItem(output_audio, "sampleRateHz"), config.output_sample_rate_hz);
    config.output_channels =
        JsonInt(cJSON_GetObjectItem(output_audio, "channels"), config.output_channels);
    config.output_max_chunk_bytes =
        JsonInt(cJSON_GetObjectItem(output_audio, "maxChunkBytes"), config.output_max_chunk_bytes);
    config.output_pacing_ms =
        JsonInt(cJSON_GetObjectItem(output_audio, "pacingMs"), config.output_pacing_ms);
    config.output_backpressure_high_water_bytes =
        JsonInt(cJSON_GetObjectItem(output_audio, "backpressureHighWaterBytes"),
                config.output_backpressure_high_water_bytes);
    config.output_backpressure_low_water_bytes =
        JsonInt(cJSON_GetObjectItem(output_audio, "backpressureLowWaterBytes"),
                config.output_backpressure_low_water_bytes);
}

void KidsEnglishProtocol::ParseConversationTransport(const cJSON* root) {
    auto data = cJSON_GetObjectItem(root, "data");
    auto transport = cJSON_GetObjectItem(data, "conversationTransport");
    WebSocketTransportConfig config;
    ConversationTransportMode mode = ConversationTransportMode::kHttp;

    if (cJSON_IsObject(transport)) {
        auto selected = cJSON_GetObjectItem(transport, "selected");
        if (JsonString(selected) == "websocket") {
            config.available = true;
            config.fallback_http = JsonString(cJSON_GetObjectItem(transport, "fallback")) != "none";
            config.protocol = JsonString(cJSON_GetObjectItem(transport, "protocol"));
            config.websocket_url = JsonString(cJSON_GetObjectItem(transport, "websocketUrl"));
            config.connect_token = JsonString(cJSON_GetObjectItem(transport, "connectToken"));
            config.ping_interval_ms =
                JsonInt(cJSON_GetObjectItem(transport, "pingIntervalMs"), config.ping_interval_ms);
            config.idle_timeout_ms =
                JsonInt(cJSON_GetObjectItem(transport, "idleTimeoutMs"), config.idle_timeout_ms);
            config.max_frame_bytes =
                JsonInt(cJSON_GetObjectItem(transport, "maxFrameBytes"), config.max_frame_bytes);

            auto input_audio = cJSON_GetObjectItem(transport, "inputAudio");
            config.input_sample_rate_hz = JsonInt(cJSON_GetObjectItem(input_audio, "sampleRateHz"),
                                                  config.input_sample_rate_hz);
            config.input_channels =
                JsonInt(cJSON_GetObjectItem(input_audio, "channels"), config.input_channels);
            config.input_frame_duration_ms =
                JsonInt(cJSON_GetObjectItem(input_audio, "frameDurationMs"),
                        config.input_frame_duration_ms);

            auto output_audio = cJSON_GetObjectItem(transport, "outputAudio");
            ParseWsOutputAudioConfig(output_audio, config);
            LogWsOutputAudioConfig("/api/device/hello", config);

            auto features = cJSON_GetObjectItem(transport, "features");
            config.feature_asr_partial =
                JsonBool(cJSON_GetObjectItem(features, "asrPartial"), config.feature_asr_partial);
            config.feature_asr_speech_endpoint =
                JsonBool(cJSON_GetObjectItem(features, "asrSpeechEndpoint"),
                         config.feature_asr_speech_endpoint);
            config.feature_server_vad_auto_end =
                JsonBool(cJSON_GetObjectItem(features, "serverVadAutoEnd"),
                         config.feature_server_vad_auto_end);

            if (config.protocol == kWsProtocol && !config.websocket_url.empty() &&
                !config.connect_token.empty() &&
                config.input_sample_rate_hz == kPracticeAudioSampleRate &&
                config.input_channels == 1 &&
                config.input_frame_duration_ms == kWsInputFrameDurationMs) {
                mode = ConversationTransportMode::kWebSocket;
            } else {
                ESP_LOGW(TAG,
                         "Ignoring unusable WebSocket transport: protocol=%s urlPresent=%s "
                         "tokenPresent=%s input=%dHz/%dch/%dms",
                         config.protocol.c_str(), config.websocket_url.empty() ? "false" : "true",
                         config.connect_token.empty() ? "false" : "true",
                         config.input_sample_rate_hz, config.input_channels,
                         config.input_frame_duration_ms);
            }
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    selected_transport_ = mode;
    ws_config_ = config;
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

bool KidsEnglishProtocol::CheckHealth(bool set_error) {
    cJSON* root = nullptr;
    bool ok = RequestJson("GET", "/health", "", &root, kHealthTimeoutMs, false);
    if (root != nullptr) {
        auto data = cJSON_GetObjectItem(root, "data");
        UpdateServerTimeOffset(cJSON_GetObjectItem(data, "timestamp"));
        cJSON_Delete(root);
    }
    if (!ok) {
        if (set_error) {
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        }
    }
    return ok;
}

bool KidsEnglishProtocol::DeviceHello(bool set_error, bool schedule_welcome_prefetch,
                                      bool update_transport) {
    cJSON* root = nullptr;
    std::string body = BuildDeviceHelloBody();
    if (!RequestJson("POST", "/api/device/hello", body, &root, kDeviceHelloTimeoutMs)) {
        if (set_error) {
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        }
        return false;
    }

    auto data = cJSON_GetObjectItem(root, "data");
    UpdateServerTimeOffset(cJSON_GetObjectItem(data, "serverTime"));
    LogDeviceHelloResponse(root);
    auto settings = cJSON_GetObjectItem(data, "settings");
    std::string voice = JsonString(cJSON_GetObjectItem(settings, "voice"));
    StoreWelcomeAudioCandidates(ParseWelcomeAudioCandidates(root), voice,
                                schedule_welcome_prefetch);
    if (update_transport) {
        ParseConversationTransport(root);
    }
    cJSON_Delete(root);
    return true;
}

bool KidsEnglishProtocol::StartHttpConversationWithFreshHello(const char* trigger) {
    if (!DeviceHello()) {
        return false;
    }
    ConversationTransportMode selected_transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selected_transport = selected_transport_;
    }
    if (selected_transport == ConversationTransportMode::kWebSocket) {
        ESP_LOGW(TAG, "Server still selected WebSocket after re-hello; trying WebSocket again");
        if (OpenWebSocketConversation(trigger)) {
            return true;
        }
        ESP_LOGW(TAG, "WebSocket retry failed; using HTTP v1 fallback for this attempt");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selected_transport_ = ConversationTransportMode::kHttp;
    }
    return StartConversation(trigger);
}

bool KidsEnglishProtocol::StartConversation(const char* trigger) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selected_transport_ = ConversationTransportMode::kHttp;
        ws_waiting_turn_complete_ = false;
        ws_waiting_initial_audio_ = false;
    }
    cJSON* root = nullptr;
    std::string body = BuildStartConversationBody(trigger);
    if (!RequestJson("POST", "/api/conversations/start", body, &root,
                     kStartConversationTimeoutMs)) {
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
    return HandleConversationResponse(response, "Let's speak English.", true, true, true);
}

bool KidsEnglishProtocol::OpenWebSocketConversation(const char* trigger) {
    if (!ConnectWebSocket()) {
        return false;
    }
    if (!SendWsConversationStart(trigger)) {
        ESP_LOGW(TAG, "Failed to start Kids English WebSocket conversation");
        return false;
    }
    return true;
}

bool KidsEnglishProtocol::ConnectWebSocket() {
    WebSocketTransportConfig config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config = ws_config_;
    }
    if (!config.available || config.websocket_url.empty() || config.connect_token.empty()) {
        ESP_LOGW(TAG, "WebSocket transport is not available after device hello");
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    auto ws = network->CreateWebSocket(1);
    if (ws == nullptr) {
        ESP_LOGE(TAG, "Failed to create WebSocket client");
        return false;
    }

    ws->SetReceiveBufferSize(config.max_frame_bytes + kWsAudioHeaderBytes + 256);
    ws->SetSendTimeoutMs(kWsSendTimeoutMs);
    std::string authorization = "Bearer " + config.connect_token;
    ws->SetHeader("Authorization", authorization.c_str());
    ws->OnData([this](const char* data, size_t len, bool binary) {
        last_incoming_time_ = std::chrono::steady_clock::now();
        if (binary) {
            HandleWsBinaryFrame(data, len);
        } else {
            HandleWsTextFrame(data, len);
        }
    });
    ws->OnDisconnected([this]() {
        ESP_LOGW(TAG, "Kids English WebSocket disconnected");
        bool closing = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closing = ws_closing_;
            if (!closing && selected_transport_ == ConversationTransportMode::kWebSocket) {
                channel_opened_ = false;
            }
        }
        if (!closing && ws_event_group_ != nullptr) {
            xEventGroupSetBits(ws_event_group_, kWsDisconnectedEvent | kWsReconnectRequiredEvent);
        }
    });
    ws->OnError([this](int error) {
        ESP_LOGW(TAG, "Kids English WebSocket error=%d", error);
        if (ws_event_group_ != nullptr) {
            xEventGroupSetBits(ws_event_group_, kWsReconnectRequiredEvent);
        }
    });

    ESP_LOGI(TAG, "Connecting Kids English WebSocket: %s",
             RedactUrlForLog(config.websocket_url).c_str());
    if (ws_event_group_ != nullptr) {
        xEventGroupClearBits(
            ws_event_group_,
            kWsSessionReadyEvent | kWsConversationStartedEvent | kWsTurnCompleteEvent |
                kWsReconnectRequiredEvent | kWsDisconnectedEvent | kWsAssistantAudioEndEvent |
                kWsConversationEndedEvent | kWsUrlAudioFinishedEvent | kWsPlaybackFinishedEvent);
    }
    std::shared_ptr<WebSocket> websocket_to_connect(std::move(ws));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        websocket_ = websocket_to_connect;
        ws_next_seq_ = 0;
        ws_closing_ = false;
        ws_waiting_turn_complete_ = false;
        ws_should_continue_listening_ = true;
        ws_waiting_initial_audio_ = false;
        ws_output_audio_active_ = false;
        ws_output_audio_finalized_ = false;
        ws_output_audio_end_received_ = false;
        ws_output_playback_pending_ = false;
        ws_output_playback_started_sent_ = false;
        ws_output_playback_finished_sent_ = false;
        ws_output_playback_queue_drained_ = false;
        ws_tts_playback_started_ = false;
        ws_url_audio_in_progress_ = false;
        pending_tts_stop_ = false;
        pending_tts_stop_text_.clear();
        pending_tts_stop_end_reason_.clear();
        pending_tts_stop_continue_listening_ = true;
        ws_output_audio_transport_.clear();
        ws_output_audio_format_.clear();
        ws_output_audio_url_.clear();
        ws_output_audio_conversation_id_.clear();
        ws_output_audio_turn_id_.clear();
        ws_output_audio_buffer_.clear();
        ws_output_audio_sample_rate_hz_ = kPracticeAudioSampleRate;
        ws_output_audio_channels_ = kPracticeAudioChannels;
        ResetWsJitterBufferLocked();
    }

    if (!websocket_to_connect->Connect(config.websocket_url.c_str())) {
        ESP_LOGW(TAG, "Kids English WebSocket connect failed, error=%d",
                 websocket_to_connect->GetLastError());
        CloseWebSocket();
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        ws_event_group_, kWsSessionReadyEvent | kWsReconnectRequiredEvent | kWsDisconnectedEvent,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(kWsSessionReadyTimeoutMs));
    if (!(bits & kWsSessionReadyEvent)) {
        ESP_LOGW(TAG, "Timed out waiting for Kids English WebSocket session.ready");
        CloseWebSocket();
        return false;
    }
    if (!SendWsSessionOpen()) {
        CloseWebSocket();
        return false;
    }
    ESP_LOGI(TAG, "Kids English WebSocket session ready");
    return true;
}

bool KidsEnglishProtocol::SendWsSessionOpen() {
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "lastSeq", 0);
    cJSON_AddStringToObject(payload, "protocol", kWsProtocol);
    return SendWsTextFrame(BuildWsJsonEnvelope("session.open", payload));
}

bool KidsEnglishProtocol::SendWsConversationStart(const char* trigger) {
    auto app_desc = esp_app_get_description();
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "firmwareVersion", app_desc->version);
    cJSON_AddStringToObject(payload, "trigger", trigger == nullptr ? "manual" : trigger);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ws_waiting_initial_audio_ = true;
    }
    if (!SendWsTextFrame(BuildWsJsonEnvelope("conversation.start", payload))) {
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        ws_event_group_,
        kWsConversationStartedEvent | kWsReconnectRequiredEvent | kWsDisconnectedEvent, pdTRUE,
        pdFALSE, pdMS_TO_TICKS(kWsConversationStartTimeoutMs));
    if (!(bits & kWsConversationStartedEvent)) {
        ESP_LOGW(TAG, "Timed out waiting for WebSocket conversation.started");
        return false;
    }
    bits = xEventGroupWaitBits(
        ws_event_group_,
        kWsAssistantAudioEndEvent | kWsReconnectRequiredEvent | kWsDisconnectedEvent, pdTRUE,
        pdFALSE, pdMS_TO_TICKS(kWsConversationStartTimeoutMs));
    if (!(bits & kWsAssistantAudioEndEvent)) {
        ESP_LOGW(TAG, "Timed out waiting for WebSocket initial assistant audio");
        return false;
    }
    return true;
}

bool KidsEnglishProtocol::SendWsConversationEnd(const std::string& conversation_id,
                                                const char* reason) {
    if (conversation_id.empty()) {
        return true;
    }
    if (ws_event_group_ != nullptr) {
        xEventGroupClearBits(ws_event_group_, kWsConversationEndedEvent |
                                                  kWsReconnectRequiredEvent | kWsDisconnectedEvent);
    }
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "reason", reason == nullptr ? "device_end" : reason);
    if (!SendWsTextFrame(BuildWsJsonEnvelope("conversation.end", payload, conversation_id))) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(
        ws_event_group_,
        kWsConversationEndedEvent | kWsReconnectRequiredEvent | kWsDisconnectedEvent, pdTRUE,
        pdFALSE, pdMS_TO_TICKS(kStartConversationTimeoutMs));
    return (bits & kWsConversationEndedEvent) != 0;
}

bool KidsEnglishProtocol::SendWsPlaybackEvent(const char* type, const std::string& conversation_id,
                                              const std::string& turn_id) {
    if (type == nullptr || conversation_id.empty()) {
        return false;
    }
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "clientTs", CurrentIsoTimestamp().c_str());
    bool sent = SendWsTextFrame(BuildWsJsonEnvelope(type, payload, conversation_id, turn_id, true));
    ESP_LOGI(TAG, "WebSocket %s sent=%s conversationId=%s turnId=%s", type, sent ? "true" : "false",
             conversation_id.c_str(), turn_id.c_str());
    return sent;
}

bool KidsEnglishProtocol::SendWsTextFrame(const std::string& text) {
    std::shared_ptr<WebSocket> ws;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsWebSocketConnectedLocked()) {
            return false;
        }
        ws = websocket_;
    }
    int64_t send_begin_ms = NowMs();
    std::lock_guard<std::mutex> send_lock(ws_send_mutex_);
    bool sent = ws->Send(text);
    int64_t send_ms = NowMs() - send_begin_ms;
    if (!sent || send_ms >= kWsSlowSendLogMs) {
        ESP_LOGW(TAG, "WS_NATIVE_TEXT_SEND_%s timeMs=%u bytes=%u sendMs=%d",
                 sent ? "SLOW" : "FAILED", LogMs(NowMs()), (unsigned)text.size(), (int)send_ms);
    }
    return sent;
}

bool KidsEnglishProtocol::SendWsAudioFrame(const uint8_t* payload, size_t len, bool end_of_segment,
                                           uint32_t* sent_seq) {
    if (sent_seq != nullptr) {
        *sent_seq = 0;
    }
    WebSocketTransportConfig config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config = ws_config_;
    }
    if (len > static_cast<size_t>(config.max_frame_bytes)) {
        ESP_LOGE(TAG, "WebSocket audio frame too large: %u > %d", (unsigned)len,
                 config.max_frame_bytes);
        return false;
    }

    std::shared_ptr<WebSocket> ws;
    uint32_t seq = 0;
    std::string conversation_id;
    std::string client_turn_id;
    std::string frame;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsWebSocketConnectedLocked()) {
            return false;
        }
        if (ws_turn_metrics_.active && !ws_turn_metrics_.input_audio_active) {
            if (ws_turn_metrics_.server_auto_end_received) {
                ++ws_turn_metrics_.input_late_frames_suppressed;
                ws_turn_metrics_.input_late_bytes_suppressed += len;
                ESP_LOGI(TAG,
                         "WS_NATIVE_INPUT_LATE_PCM_SUPPRESSED timeMs=%u conversationId=%s "
                         "clientTurnId=%s bytes=%u lateFrames=%u lateBytes=%u afterAutoEnd=true",
                         LogMs(NowMs()), ws_turn_metrics_.conversation_id.c_str(),
                         ws_turn_metrics_.client_turn_id.c_str(), (unsigned)len,
                         (unsigned)ws_turn_metrics_.input_late_frames_suppressed,
                         (unsigned)ws_turn_metrics_.input_late_bytes_suppressed);
            }
            return true;
        }

        ws = websocket_;
        seq = ++ws_next_seq_;
        conversation_id = ws_turn_metrics_.conversation_id;
        client_turn_id = ws_turn_metrics_.client_turn_id;
        frame.reserve(kWsAudioHeaderBytes + len);
        frame.append(kWsAudioMagic, 4);
        frame.push_back(static_cast<char>(1));
        frame.push_back(static_cast<char>(1));
        AppendBe16(frame, end_of_segment ? 1 : 0);
        AppendBe32(frame, seq);
        AppendBe32(frame, static_cast<uint32_t>(len));
        frame.append(reinterpret_cast<const char*>(payload), len);
        if (sent_seq != nullptr) {
            *sent_seq = seq;
        }
    }

    bool log_frame = seq <= 3 || end_of_segment;
    if (log_frame) {
        ESP_LOGI(TAG,
                 "WS_NATIVE_TURN_AUDIO_FRAME_SEND_BEGIN timeMs=%u conversationId=%s "
                 "clientTurnId=%s seq=%u bytes=%u final=%s",
                 LogMs(NowMs()), conversation_id.c_str(), client_turn_id.c_str(), seq,
                 (unsigned)len, end_of_segment ? "true" : "false");
    }
    int64_t send_begin_ms = NowMs();
    std::lock_guard<std::mutex> send_lock(ws_send_mutex_);
    bool sent = ws->Send(frame.data(), frame.size(), true);
    int64_t send_ms = NowMs() - send_begin_ms;

    bool in_flight_after_auto_end = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sent && ws_turn_metrics_.active &&
            ws_turn_metrics_.conversation_id == conversation_id &&
            ws_turn_metrics_.client_turn_id == client_turn_id) {
            ws_turn_metrics_.input_audio_bytes_sent += len;
            ++ws_turn_metrics_.input_audio_frames_sent;
            ws_turn_metrics_.input_last_seq = seq;
            in_flight_after_auto_end =
                ws_turn_metrics_.server_auto_end_received && !ws_turn_metrics_.input_audio_active;
        }
    }

    if (log_frame || !sent || send_ms >= kWsSlowSendLogMs || in_flight_after_auto_end) {
        ESP_LOG_LEVEL(sent ? ESP_LOG_INFO : ESP_LOG_WARN, TAG,
                      "WS_NATIVE_TURN_AUDIO_FRAME_SEND_%s timeMs=%u conversationId=%s "
                      "clientTurnId=%s seq=%u bytes=%u final=%s sendMs=%d afterAutoEnd=%s",
                      sent ? (send_ms >= kWsSlowSendLogMs ? "SLOW" : "END") : "FAILED",
                      LogMs(NowMs()), conversation_id.c_str(), client_turn_id.c_str(), seq,
                      (unsigned)len, end_of_segment ? "true" : "false", (int)send_ms,
                      in_flight_after_auto_end ? "true" : "false");
    }
    return sent;
}

bool KidsEnglishProtocol::SendWsTurnAudioEndIfNeeded(const std::string& conversation_id,
                                                     const std::string& client_turn_id,
                                                     const char* reason, const char* source) {
    std::string conversation_to_send = conversation_id;
    std::string turn_to_send = client_turn_id;
    int duration_ms = 0;
    bool should_send = false;
    bool after_auto_end = false;
    bool already_sent = false;
    bool active_turn = false;
    int begin_to_input_stopped_ms = -1;
    int64_t now_ms = NowMs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_turn =
            ws_turn_metrics_.active && IsCurrentWsTurnEventLocked(conversation_id, client_turn_id);
        if (conversation_to_send.empty()) {
            conversation_to_send = ws_turn_metrics_.conversation_id;
        }
        if (turn_to_send.empty()) {
            turn_to_send = ws_turn_metrics_.client_turn_id;
        }
        after_auto_end = ws_turn_metrics_.server_auto_end_received;
        already_sent = ws_turn_metrics_.turn_audio_end_sent;
        if (active_turn && ws_turn_metrics_.input_audio_active) {
            ws_turn_metrics_.input_audio_active = false;
            ws_turn_metrics_.input_stopped_ms = now_ms;
            ws_turn_metrics_.input_stop_reason = source == nullptr ? "client_turn_end" : source;
        }
        duration_ms = WsInputDurationMsLocked();
        begin_to_input_stopped_ms =
            LogDeltaMs(ws_turn_metrics_.turn_begin_ms, ws_turn_metrics_.input_stopped_ms);
        if (active_turn && after_auto_end) {
            if (!ws_turn_metrics_.late_turn_end_suppressed) {
                ws_turn_metrics_.late_turn_end_suppressed = true;
                ESP_LOGI(TAG,
                         "WS_NATIVE_TURN_AUDIO_END_SUPPRESSED timeMs=%u conversationId=%s "
                         "clientTurnId=%s reason=%s source=%s afterAutoEnd=true "
                         "alreadyEndSent=%s beginToInputStoppedMs=%d",
                         LogMs(now_ms), conversation_to_send.c_str(), turn_to_send.c_str(),
                         reason == nullptr ? "unknown" : reason,
                         source == nullptr ? "unknown" : source, already_sent ? "true" : "false",
                         begin_to_input_stopped_ms);
            }
        } else if (active_turn && !already_sent) {
            ws_turn_metrics_.turn_audio_end_sent = true;
            ws_turn_metrics_.audio_submitted_ms = now_ms;
            if (ws_turn_metrics_.input_stopped_ms == 0) {
                ws_turn_metrics_.input_stopped_ms = now_ms;
                ws_turn_metrics_.input_stop_reason = source == nullptr ? "client_turn_end" : source;
            }
            should_send = true;
        }
    }

    if (!active_turn) {
        ESP_LOGI(TAG,
                 "WS_NATIVE_TURN_AUDIO_END_IGNORED timeMs=%u conversationId=%s clientTurnId=%s "
                 "reason=%s source=%s currentTurn=false",
                 LogMs(now_ms), conversation_id.c_str(), client_turn_id.c_str(),
                 reason == nullptr ? "unknown" : reason, source == nullptr ? "unknown" : source);
        return true;
    }
    if (after_auto_end || already_sent) {
        return true;
    }
    if (!should_send) {
        return true;
    }

    cJSON* end_payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(end_payload, "durationMs", duration_ms);
    cJSON_AddStringToObject(end_payload, "reason", reason == nullptr ? "manual_stop" : reason);
    bool sent = SendWsTextFrame(
        BuildWsJsonEnvelope("turn.audio.end", end_payload, conversation_to_send, turn_to_send));
    ESP_LOGI(TAG,
             "WS_NATIVE_TURN_AUDIO_END_SENT timeMs=%u conversationId=%s clientTurnId=%s "
             "reason=%s source=%s durationMs=%d beginToInputStoppedMs=%d sent=%s",
             LogMs(now_ms), conversation_to_send.c_str(), turn_to_send.c_str(),
             reason == nullptr ? "manual_stop" : reason, source == nullptr ? "unknown" : source,
             duration_ms, begin_to_input_stopped_ms, sent ? "true" : "false");
    return sent;
}

uint32_t KidsEnglishProtocol::NextWsSeq() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ++ws_next_seq_;
}

void KidsEnglishProtocol::CloseWebSocket() {
    std::shared_ptr<WebSocket> ws;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ws_closing_ = true;
        ws = std::move(websocket_);
        ws_output_audio_active_ = false;
        ws_output_audio_buffer_.clear();
        ws_output_audio_url_.clear();
        ws_output_audio_conversation_id_.clear();
        ws_output_audio_turn_id_.clear();
        ws_output_playback_pending_ = false;
        ws_output_playback_queue_drained_ = false;
        ws_url_audio_in_progress_ = false;
    }
    if (ws != nullptr) {
        ws->Close();
        ws.reset();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ws_closing_ = false;
    }
}

bool KidsEnglishProtocol::IsWebSocketConnectedLocked() const {
    return websocket_ != nullptr && websocket_->IsConnected();
}

bool KidsEnglishProtocol::IsWebSocketHeartbeatTimedOut() const {
    WebSocketTransportConfig config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config = ws_config_;
    }
    auto timeout_ms = config.ping_interval_ms * kWsHeartbeatMissesBeforeFallback;
    if (timeout_ms <= 0) {
        timeout_ms = 45000;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - last_incoming_time_);
    return elapsed.count() > timeout_ms;
}

bool KidsEnglishProtocol::UploadPendingAudio() {
    std::vector<int16_t> pcm;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_audio_.clear();
        pcm = std::move(pending_pcm_);
    }

    return UploadPcmAudioForCurrentConversation(std::move(pcm));
}

bool KidsEnglishProtocol::UploadPcmAudioForCurrentConversation(std::vector<int16_t>&& pcm) {
    std::string conversation_id;
    ConversationTransportMode transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        conversation_id = conversation_id_;
        transport = selected_transport_;
    }

    if (conversation_id.empty()) {
        ESP_LOGE(TAG, "Missing conversation id");
        return false;
    }
    if (pcm.empty()) {
        ESP_LOGE(TAG, "No PCM audio captured; WAV upload requires 16 kHz mono PCM");
        return false;
    }

    if (transport == ConversationTransportMode::kWebSocket) {
        if (UploadPcmAudioWebSocket(pcm, conversation_id)) {
            return true;
        }
        ESP_LOGW(TAG,
                 "WebSocket native turn upload failed; closing current WebSocket without HTTP "
                 "fallback");
        CloseWebSocket();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            conversation_id_.clear();
            session_id_.clear();
            channel_opened_ = false;
        }
        return false;
    }

    UploadResult result = UploadPcmAudio(std::move(pcm), conversation_id);
    return result == UploadResult::kSuccess || result == UploadResult::kConversationEnded;
}

bool KidsEnglishProtocol::UploadPcmAudioWebSocket(const std::vector<int16_t>& pcm,
                                                  const std::string& conversation_id) {
    if (pcm.empty() || conversation_id.empty()) {
        return false;
    }
    if (IsWebSocketHeartbeatTimedOut()) {
        ESP_LOGW(TAG, "Kids English WebSocket heartbeat timed out before upload");
        return false;
    }

    size_t samples_to_send = pcm.size();
    int duration_ms = static_cast<int>((samples_to_send * 1000) / kPracticeAudioSampleRate);
    if (duration_ms > kMaxPracticeAudioDurationSeconds * 1000) {
        ESP_LOGW(TAG, "Trimming WebSocket recording from %d ms to %d seconds", duration_ms,
                 kMaxPracticeAudioDurationSeconds);
        samples_to_send = kPracticeAudioSampleRate * kMaxPracticeAudioDurationSeconds;
        duration_ms = static_cast<int>((samples_to_send * 1000) / kPracticeAudioSampleRate);
    }

    std::string client_turn_id = GenerateClientTurnId();
    std::string recorded_at = CurrentIsoTimestamp();
    int64_t turn_begin_ms = NowMs();
    ESP_LOGI(TAG,
             "WS_NATIVE_TURN_BEGIN timeMs=%u deviceId=%s conversationId=%s clientTurnId=%s "
             "pcmBytes=%u durationMs=%d format=pcm_s16le/%dHz/%dch/%dms",
             LogMs(turn_begin_ms), device_id_.c_str(), conversation_id.c_str(),
             client_turn_id.c_str(), (unsigned)(samples_to_send * sizeof(int16_t)), duration_ms,
             kPracticeAudioSampleRate, kPracticeAudioChannels, kWsInputFrameDurationMs);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ResetWsTurnMetricsLocked();
        ws_turn_metrics_.active = true;
        ws_turn_metrics_.input_audio_active = true;
        ws_turn_metrics_.conversation_id = conversation_id;
        ws_turn_metrics_.client_turn_id = client_turn_id;
        ws_turn_metrics_.turn_begin_ms = turn_begin_ms;
        ws_waiting_turn_complete_ = true;
        ws_should_continue_listening_ = true;
        ws_output_audio_active_ = false;
        ws_output_audio_finalized_ = false;
        ws_output_audio_end_received_ = false;
        ws_output_playback_pending_ = false;
        ws_output_playback_started_sent_ = false;
        ws_output_playback_finished_sent_ = false;
        ws_output_playback_queue_drained_ = false;
        ws_tts_playback_started_ = false;
        ws_url_audio_in_progress_ = false;
        pending_tts_stop_ = false;
        pending_tts_stop_text_.clear();
        pending_tts_stop_end_reason_.clear();
        pending_tts_stop_continue_listening_ = true;
        ws_output_audio_transport_.clear();
        ws_output_audio_format_.clear();
        ws_output_audio_url_.clear();
        ws_output_audio_conversation_id_.clear();
        ws_output_audio_turn_id_.clear();
        ws_output_audio_buffer_.clear();
        ws_output_audio_sample_rate_hz_ = kPracticeAudioSampleRate;
        ws_output_audio_channels_ = kPracticeAudioChannels;
        ResetWsJitterBufferLocked();
    }
    if (ws_event_group_ != nullptr) {
        xEventGroupClearBits(ws_event_group_, kWsTurnCompleteEvent | kWsReconnectRequiredEvent |
                                                  kWsDisconnectedEvent | kWsConversationEndedEvent |
                                                  kWsPlaybackFinishedEvent);
    }

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "clientTurnId", client_turn_id.c_str());
    cJSON_AddStringToObject(payload, "recordedAt", recorded_at.c_str());
    cJSON* format = cJSON_CreateObject();
    cJSON_AddNumberToObject(format, "channels", kPracticeAudioChannels);
    cJSON_AddStringToObject(format, "codec", "pcm_s16le");
    cJSON_AddNumberToObject(format, "frameDurationMs", kWsInputFrameDurationMs);
    cJSON_AddNumberToObject(format, "sampleRateHz", kPracticeAudioSampleRate);
    cJSON_AddItemToObject(payload, "format", format);
    if (!SendWsTextFrame(BuildWsJsonEnvelope("turn.audio.start", payload, conversation_id,
                                             client_turn_id, true))) {
        ESP_LOGW(TAG,
                 "WS_NATIVE_TURN_UPLOAD_FAILED stage=turn_audio_start conversationId=%s "
                 "clientTurnId=%s",
                 conversation_id.c_str(), client_turn_id.c_str());
        return false;
    }
    ESP_LOGI(TAG,
             "WS_NATIVE_TURN_AUDIO_START_SENT timeMs=%u conversationId=%s clientTurnId=%s "
             "totalBytes=%u frameBytes=%u paceMs=%d",
             LogMs(NowMs()), conversation_id.c_str(), client_turn_id.c_str(),
             (unsigned)(samples_to_send * sizeof(int16_t)),
             (unsigned)(kWsInputFrameSamples * sizeof(int16_t)), kWsInputFramePaceMs);

    const uint8_t* pcm_bytes = reinterpret_cast<const uint8_t*>(pcm.data());
    size_t total_bytes = samples_to_send * sizeof(int16_t);
    size_t offset = 0;
    size_t frame_bytes = kWsInputFrameSamples * sizeof(int16_t);
    size_t frame_count = 0;
    int64_t last_progress_ms = NowMs();
    std::string turn_end_reason = "manual_stop";
    std::string turn_end_source = "client_upload_complete";
    while (offset < total_bytes) {
        bool input_active = true;
        bool auto_end_received = false;
        bool turn_end_requested = false;
        std::string requested_reason;
        std::string requested_source;
        size_t late_frames = 0;
        int begin_to_request_ms = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            input_active = ws_turn_metrics_.input_audio_active;
            auto_end_received = ws_turn_metrics_.server_auto_end_received;
            turn_end_requested = ws_turn_metrics_.turn_audio_end_requested &&
                                 !ws_turn_metrics_.turn_audio_end_sent &&
                                 !ws_turn_metrics_.server_auto_end_received;
            requested_reason = ws_turn_metrics_.turn_audio_end_request_reason;
            requested_source = ws_turn_metrics_.turn_audio_end_request_source;
            begin_to_request_ms = LogDeltaMs(ws_turn_metrics_.turn_begin_ms,
                                             ws_turn_metrics_.turn_audio_end_requested_ms);
            late_frames = ws_turn_metrics_.input_late_frames_suppressed;
            if (!input_active && auto_end_received) {
                ++ws_turn_metrics_.input_late_frames_suppressed;
                ws_turn_metrics_.input_late_bytes_suppressed +=
                    std::min(frame_bytes, total_bytes - offset);
                late_frames = ws_turn_metrics_.input_late_frames_suppressed;
            }
        }
        if (turn_end_requested) {
            if (!requested_reason.empty()) {
                turn_end_reason = requested_reason;
            }
            if (!requested_source.empty()) {
                turn_end_source = requested_source;
            }
            ESP_LOGI(TAG,
                     "WS_NATIVE_TURN_AUDIO_SEND_STOPPED timeMs=%u conversationId=%s "
                     "clientTurnId=%s offset=%u totalBytes=%u frames=%u requestedEnd=true "
                     "reason=%s source=%s beginToRequestMs=%d",
                     LogMs(NowMs()), conversation_id.c_str(), client_turn_id.c_str(),
                     (unsigned)offset, (unsigned)total_bytes, (unsigned)frame_count,
                     turn_end_reason.c_str(), turn_end_source.c_str(), begin_to_request_ms);
            break;
        }
        if (!input_active) {
            ESP_LOGI(TAG,
                     "WS_NATIVE_TURN_AUDIO_SEND_STOPPED timeMs=%u conversationId=%s "
                     "clientTurnId=%s offset=%u totalBytes=%u frames=%u autoEnd=%s "
                     "lateFramesSuppressed=%u",
                     LogMs(NowMs()), conversation_id.c_str(), client_turn_id.c_str(),
                     (unsigned)offset, (unsigned)total_bytes, (unsigned)frame_count,
                     auto_end_received ? "true" : "false", (unsigned)late_frames);
            break;
        }
        size_t chunk_bytes = std::min(frame_bytes, total_bytes - offset);
        bool final_frame = offset + chunk_bytes >= total_bytes;
        uint32_t sent_seq = 0;
        if (chunk_bytes == frame_bytes) {
            if (!SendWsAudioFrame(pcm_bytes + offset, chunk_bytes, final_frame, &sent_seq)) {
                ESP_LOGW(TAG,
                         "WS_NATIVE_TURN_UPLOAD_FAILED stage=binary_audio_frame "
                         "conversationId=%s clientTurnId=%s offset=%u totalBytes=%u frames=%u",
                         conversation_id.c_str(), client_turn_id.c_str(), (unsigned)offset,
                         (unsigned)total_bytes, (unsigned)frame_count);
                return false;
            }
        } else {
            uint8_t padded_frame[kWsInputFrameSamples * sizeof(int16_t)] = {};
            std::memcpy(padded_frame, pcm_bytes + offset, chunk_bytes);
            if (!SendWsAudioFrame(padded_frame, frame_bytes, final_frame, &sent_seq)) {
                ESP_LOGW(TAG,
                         "WS_NATIVE_TURN_UPLOAD_FAILED stage=binary_audio_frame_padded "
                         "conversationId=%s clientTurnId=%s offset=%u totalBytes=%u frames=%u",
                         conversation_id.c_str(), client_turn_id.c_str(), (unsigned)offset,
                         (unsigned)total_bytes, (unsigned)frame_count);
                return false;
            }
        }
        if (sent_seq == 0) {
            ESP_LOGI(TAG,
                     "WS_NATIVE_TURN_AUDIO_SEND_STOPPED timeMs=%u conversationId=%s "
                     "clientTurnId=%s offset=%u totalBytes=%u frames=%u sentSeq=0",
                     LogMs(NowMs()), conversation_id.c_str(), client_turn_id.c_str(),
                     (unsigned)offset, (unsigned)total_bytes, (unsigned)frame_count);
            break;
        }
        ++frame_count;
        offset += chunk_bytes;
        int64_t now_ms = NowMs();
        if (now_ms - last_progress_ms >= 1000 || offset >= total_bytes) {
            ESP_LOGI(TAG,
                     "WS_NATIVE_TURN_AUDIO_SEND_PROGRESS timeMs=%u conversationId=%s "
                     "clientTurnId=%s sentBytes=%u/%u frames=%u final=%s",
                     LogMs(now_ms), conversation_id.c_str(), client_turn_id.c_str(),
                     (unsigned)offset, (unsigned)total_bytes, (unsigned)frame_count,
                     offset >= total_bytes ? "true" : "false");
            last_progress_ms = now_ms;
        }
        vTaskDelay(pdMS_TO_TICKS(kWsInputFramePaceMs));
    }

    if (!SendWsTurnAudioEndIfNeeded(conversation_id, client_turn_id, turn_end_reason.c_str(),
                                    turn_end_source.c_str())) {
        ESP_LOGW(TAG,
                 "WS_NATIVE_TURN_UPLOAD_FAILED stage=turn_audio_end conversationId=%s "
                 "clientTurnId=%s frames=%u totalBytes=%u",
                 conversation_id.c_str(), client_turn_id.c_str(), (unsigned)frame_count,
                 (unsigned)total_bytes);
        return false;
    }
    ESP_LOGI(TAG,
             "WS_NATIVE_TURN_AUDIO_UPLOAD_STOPPED timeMs=%u conversationId=%s clientTurnId=%s "
             "frames=%u sentBytes=%u/%u nominalDurationMs=%d",
             LogMs(NowMs()), conversation_id.c_str(), client_turn_id.c_str(), (unsigned)frame_count,
             (unsigned)offset, (unsigned)total_bytes, duration_ms);
    ESP_LOGI(TAG,
             "WS_NATIVE_TURN_AUDIO_SUBMITTED_UI_SUPPRESSED conversationId=%s clientTurnId=%s "
             "reason=preserve_assistant_subtitle",
             conversation_id.c_str(), client_turn_id.c_str());

    EventBits_t bits = xEventGroupWaitBits(ws_event_group_,
                                           kWsTurnCompleteEvent | kWsReconnectRequiredEvent |
                                               kWsDisconnectedEvent | kWsConversationEndedEvent,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(kUploadTimeoutMs));
    if (bits & kWsTurnCompleteEvent) {
        // Handled in HandleWsTurnComplete, after assistant audio playback drains.
    } else if (bits & kWsConversationEndedEvent) {
        ESP_LOGI(TAG,
                 "WS_NATIVE_CONVERSATION_ENDED_WAIT_RESULT conversationId=%s clientTurnId=%s "
                 "reason=defer_until_playback_finished",
                 conversation_id.c_str(), client_turn_id.c_str());
        MaybeEmitWsTtsStopAfterPlayback("ws_conversation_ended_wait");
    } else {
        std::lock_guard<std::mutex> lock(mutex_);
        const char* reason = (bits & kWsReconnectRequiredEvent) ? "reconnect_required"
                             : (bits & kWsDisconnectedEvent)    ? "disconnected"
                                                                : "timeout";
        ws_turn_metrics_.fallback_requested = true;
        ws_turn_metrics_.fallback_reason = reason;
        LogWsTurnMetricsLocked("ws_turn_failed");
        ESP_LOGW(TAG,
                 "Timed out waiting for WebSocket turn.complete; native turn failed reason=%s "
                 "conversationId=%s clientTurnId=%s",
                 reason, conversation_id.c_str(), client_turn_id.c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_pcm_.clear();
        pending_audio_.clear();
    }
    return true;
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
    http->SetHeader("Content-Type",
                    std::string("multipart/form-data; boundary=") + kMultipartBoundary);

    std::string wav = CreateWavFile(pcm, kPracticeAudioSampleRate);
    std::string client_turn_id = GenerateClientTurnId();
    std::string recorded_at = CurrentIsoTimestamp();
    std::string multipart_body =
        BuildMultipartAudioBody(kMultipartBoundary, wav, client_turn_id, recorded_at, duration_ms);
    std::string path = "/api/conversations/" + conversation_id + "/turns/audio";
    AddDeviceAuthHeaders(http.get(), "POST", path, Sha256Hex(""));
    http->SetContent(std::move(multipart_body));
    auto url = BuildUrl(path.c_str());
    ESP_LOGI(
        TAG,
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
            bool was_open = ClearServerEndedConversation("http_409_conversation_ended");
            EmitTtsMessage("stop", nullptr, false);
            EmitEmotion("neutral");
            if (was_open && on_audio_channel_closed_ != nullptr) {
                on_audio_channel_closed_();
            }
            ESP_LOGW(TAG,
                     "Conversation already ended after upload attempt; local conversation state "
                     "cleared");
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
            bool was_open = ClearServerEndedConversation("http_ok_false_conversation_ended");
            EmitTtsMessage("stop", nullptr, false);
            EmitEmotion("neutral");
            if (was_open && on_audio_channel_closed_ != nullptr) {
                on_audio_channel_closed_();
            }
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
             "recordingBytes=%u httpStatus=%d ttsTextPresent=%s ttsAudioUrl=%s "
             "providerFallback=%s providerErrorCode=%s asrDurationMs=%d llmDurationMs=%d "
             "ttsDurationMs=%d totalDurationMs=%d",
             device_id_.c_str(), conversation_id.c_str(), result.turn_id.c_str(),
             result.request_id.c_str(), (unsigned)pcm_bytes, status,
             result.has_tts_text ? "true" : "false", RedactUrlForLog(result.tts_audio_url).c_str(),
             result.provider_fallback ? "true" : "false", result.provider_error_code.c_str(),
             result.asr_duration_ms, result.llm_duration_ms, result.tts_duration_ms,
             result.total_duration_ms);
    EmitSttMessage("Audio submitted.");
    return HandleConversationResponse(result, "I heard you.", true) ? UploadResult::kSuccess
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
             "Standalone TTS result: requestId=%s format=%s ttsTextPresent=%s ttsText=%s "
             "ttsUrl=%s ttsDurationMs=%d totalDurationMs=%d providerFallback=%s "
             "providerErrorCode=%s",
             response.request_id.c_str(), response.audio_format.c_str(),
             response.has_tts_text ? "true" : "false", response.tts_text.c_str(),
             RedactUrlForLog(response.tts_audio_url).c_str(), response.tts_duration_ms,
             response.total_duration_ms, response.provider_fallback ? "true" : "false",
             response.provider_error_code.c_str());
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
    std::string multipart_body =
        BuildStandaloneAsrMultipartBody(kMultipartBoundary, wav, prompt_text);

    http->SetTimeout(kStandaloneSpeechTimeoutMs);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Content-Type",
                    std::string("multipart/form-data; boundary=") + kMultipartBoundary);
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
             response.request_id.c_str(), response.transcript.c_str(),
             response.audio_format.c_str(), response.asr_duration_ms, response.total_duration_ms);
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

bool KidsEnglishProtocol::ParseConversationResponse(const cJSON* root,
                                                    ConversationResponse& result) {
    auto data = cJSON_GetObjectItem(root, "data");
    auto conversation_id = cJSON_GetObjectItem(data, "conversationId");
    auto turn_id = cJSON_GetObjectItem(data, "turnId");
    auto device_state = cJSON_GetObjectItem(data, "deviceState");
    auto screen_cue = cJSON_GetObjectItem(data, "screenCue");
    auto tts_audio_url = cJSON_GetObjectItem(data, "ttsAudioUrl");
    auto tts_text = cJSON_GetObjectItem(data, "ttsText");
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
    auto welcome_audio_cached = cJSON_GetObjectItem(diagnostics, "welcomeAudioCached");
    auto welcome_audio_cache_key = cJSON_GetObjectItem(diagnostics, "welcomeAudioCacheKey");

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
    result.has_tts_text = cJSON_IsString(tts_text);
    result.tts_text = JsonString(tts_text);
    result.audio_format = JsonString(audio_format);
    result.should_continue_listening = JsonBool(should_continue_listening, true);
    result.request_id = JsonString(request_id);
    result.asr_duration_ms = JsonInt(asr_duration_ms);
    result.llm_duration_ms = JsonInt(llm_duration_ms);
    result.tts_duration_ms = JsonInt(tts_duration_ms);
    result.total_duration_ms = JsonInt(total_duration_ms);
    result.provider_fallback = JsonBool(provider_fallback);
    result.provider_error_code = JsonString(provider_error_code);
    result.welcome_audio_cached = JsonBool(welcome_audio_cached);
    result.welcome_audio_cache_key = JsonString(welcome_audio_cache_key);
    if (!result.tts_audio_url.empty() && !result.has_tts_text) {
        ESP_LOGW(TAG,
                 "Conversation response missing ttsText: requestId=%s ttsAudioUrl=%s "
                 "providerFallback=%s providerErrorCode=%s",
                 result.request_id.c_str(), RedactUrlForLog(result.tts_audio_url).c_str(),
                 result.provider_fallback ? "true" : "false", result.provider_error_code.c_str());
    }
    return true;
}

bool KidsEnglishProtocol::ParseStandaloneTtsResponse(const cJSON* root,
                                                     StandaloneTtsResponse& result) {
    auto data = cJSON_GetObjectItem(root, "data");
    auto tts_audio_url = cJSON_GetObjectItem(data, "ttsAudioUrl");
    auto tts_text = cJSON_GetObjectItem(data, "ttsText");
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
    result.has_tts_text = cJSON_IsString(tts_text);
    result.tts_text = JsonString(tts_text);
    result.audio_format = JsonString(audio_format);
    result.request_id = JsonString(request_id);
    result.tts_duration_ms = JsonInt(tts_duration_ms);
    result.total_duration_ms = JsonInt(total_duration_ms);
    result.provider_fallback = JsonBool(provider_fallback);
    result.provider_error_code = JsonString(provider_error_code);
    if (!result.has_tts_text) {
        ESP_LOGW(TAG,
                 "Standalone TTS response missing ttsText: requestId=%s ttsAudioUrl=%s "
                 "providerFallback=%s providerErrorCode=%s",
                 result.request_id.c_str(), RedactUrlForLog(result.tts_audio_url).c_str(),
                 result.provider_fallback ? "true" : "false", result.provider_error_code.c_str());
    }
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

bool KidsEnglishProtocol::ParsePcm16MonoBytes(const std::string& bytes,
                                              std::vector<int16_t>& pcm) const {
    if (bytes.empty() || (bytes.size() % sizeof(int16_t)) != 0) {
        ESP_LOGE(TAG, "Invalid PCM payload size: %u", (unsigned)bytes.size());
        return false;
    }
    pcm.resize(bytes.size() / sizeof(int16_t));
    std::memcpy(pcm.data(), bytes.data(), bytes.size());
    return true;
}

std::string KidsEnglishProtocol::CreateWavFile(const std::vector<int16_t>& pcm,
                                               int sample_rate) const {
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
    ESP_LOGI(TAG, "Downloading TTS audio: %s", RedactUrlForLog(url).c_str());
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

    return PlayPcmAudio(std::move(pcm), sample_rate);
}

bool KidsEnglishProtocol::PlayPcmAudio(std::vector<int16_t>&& pcm, int sample_rate) {
    if (pcm.empty() || sample_rate <= 0) {
        return false;
    }
    AudioService& audio_service = Application::GetInstance().GetAudioService();
    audio_service.PushPcmToPlaybackQueue(std::move(pcm), sample_rate);
    return true;
}

bool KidsEnglishProtocol::TryPlayWelcomeAudioCache(const ConversationResponse& response) {
    if (!response.welcome_audio_cached || response.welcome_audio_cache_key.empty()) {
        if (response.welcome_audio_cached || !response.welcome_audio_cache_key.empty()) {
            ESP_LOGI(TAG,
                     "WELCOME_AUDIO_CACHE_MISS requestId=%s cached=%s cacheKey=%s "
                     "reason=disabled",
                     response.request_id.c_str(), response.welcome_audio_cached ? "true" : "false",
                     response.welcome_audio_cache_key.c_str());
        }
        return false;
    }

    std::vector<int16_t> pcm;
    int sample_rate = 0;
    std::string voice;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        voice = welcome_audio_voice_;
        auto entry = std::find_if(welcome_audio_cache_.begin(), welcome_audio_cache_.end(),
                                  [&](const WelcomeAudioCacheEntry& item) {
                                      return item.cache_key == response.welcome_audio_cache_key &&
                                             item.voice == voice && !item.pcm.empty() &&
                                             item.sample_rate > 0;
                                  });
        if (entry == welcome_audio_cache_.end()) {
            ESP_LOGI(TAG,
                     "WELCOME_AUDIO_CACHE_MISS requestId=%s cached=true cacheKey=%s voice=%s "
                     "reason=not_found",
                     response.request_id.c_str(), response.welcome_audio_cache_key.c_str(),
                     voice.c_str());
            return false;
        }
        pcm = entry->pcm;
        sample_rate = entry->sample_rate;
    }

    ESP_LOGI(TAG,
             "WELCOME_AUDIO_CACHE_HIT requestId=%s cacheKey=%s voice=%s sampleRate=%d samples=%u",
             response.request_id.c_str(), response.welcome_audio_cache_key.c_str(), voice.c_str(),
             sample_rate, (unsigned)pcm.size());
    return PlayPcmAudio(std::move(pcm), sample_rate);
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

bool KidsEnglishProtocol::ShouldContinueConversation(const ConversationResponse& response) const {
    return response.should_continue_listening && response.device_state != "finished";
}

bool KidsEnglishProtocol::ClearServerEndedConversation(const char* reason) {
    bool was_open = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        was_open = channel_opened_;
        channel_opened_ = false;
        upload_in_progress_ = false;
        pending_audio_.clear();
        pending_pcm_.clear();
        conversation_id_.clear();
        session_id_.clear();
        ws_waiting_turn_complete_ = false;
        ws_waiting_initial_audio_ = false;
        ws_should_continue_listening_ = false;
    }
    ESP_LOGI(TAG, "Server ended Kids English conversation; local state cleared reason=%s",
             reason == nullptr ? "unknown" : reason);
    return was_open;
}

bool KidsEnglishProtocol::WaitForWsUrlAudioIfNeeded(int timeout_ms) {
    bool wait_url_audio = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wait_url_audio = ws_url_audio_in_progress_;
    }
    if (!wait_url_audio) {
        return true;
    }
    if (ws_event_group_ == nullptr) {
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        ws_event_group_,
        kWsUrlAudioFinishedEvent | kWsReconnectRequiredEvent | kWsDisconnectedEvent, pdTRUE,
        pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & kWsUrlAudioFinishedEvent) != 0;
}

void KidsEnglishProtocol::QueuePendingTtsStop(const std::string& text, bool continue_listening,
                                              const char* end_reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_tts_stop_ = true;
    pending_tts_stop_text_ = text;
    pending_tts_stop_end_reason_ = end_reason == nullptr ? "" : end_reason;
    pending_tts_stop_continue_listening_ = continue_listening;
}

void KidsEnglishProtocol::FlushPendingTtsStop() {
    bool has_pending = false;
    std::string text;
    std::string end_reason;
    bool continue_listening = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        has_pending = pending_tts_stop_;
        if (has_pending) {
            text = pending_tts_stop_text_;
            end_reason = pending_tts_stop_end_reason_;
            continue_listening = pending_tts_stop_continue_listening_;
            pending_tts_stop_ = false;
            pending_tts_stop_text_.clear();
            pending_tts_stop_end_reason_.clear();
            pending_tts_stop_continue_listening_ = true;
        }
    }
    if (has_pending) {
        EmitConversationStop(text, continue_listening,
                             end_reason.empty() ? nullptr : end_reason.c_str());
    }
}

void KidsEnglishProtocol::EmitConversationStop(const std::string& text, bool continue_listening,
                                               const char* end_reason) {
    bool was_open = false;
    bool conversation_ended = !continue_listening && end_reason != nullptr;
    if (conversation_ended) {
        was_open = ClearServerEndedConversation(end_reason);
    }
    EmitTtsMessage("stop", text.empty() ? nullptr : text.c_str(), continue_listening);
    EmitEmotion(continue_listening ? "happy" : "neutral");
    if (conversation_ended && was_open && on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool KidsEnglishProtocol::HandleConversationResponse(const ConversationResponse& response,
                                                     const char* fallback_text,
                                                     bool wait_for_playback, bool defer_tts_stop,
                                                     bool allow_welcome_audio_cache) {
    (void)fallback_text;
    bool should_continue = ShouldContinueConversation(response);
    ESP_LOGI(TAG,
             "Conversation response: conversationId=%s turnId=%s state=%s cue=%s "
             "ttsTextPresent=%s ttsText=%s ttsAudioUrl=%s format=%s continue=%s "
             "requestId=%s asr=%d llm=%d ttsMs=%d total=%d providerFallback=%s "
             "providerErrorCode=%s welcomeAudioCached=%s welcomeAudioCacheKey=%s",
             response.conversation_id.c_str(), response.turn_id.c_str(),
             response.device_state.c_str(), response.screen_cue.c_str(),
             response.has_tts_text ? "true" : "false", response.tts_text.c_str(),
             RedactUrlForLog(response.tts_audio_url).c_str(), response.audio_format.c_str(),
             should_continue ? "true" : "false", response.request_id.c_str(),
             response.asr_duration_ms, response.llm_duration_ms, response.tts_duration_ms,
             response.total_duration_ms, response.provider_fallback ? "true" : "false",
             response.provider_error_code.c_str(), response.welcome_audio_cached ? "true" : "false",
             response.welcome_audio_cache_key.c_str());

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
    const std::string display_text = response.has_tts_text ? response.tts_text : "";
    if (!display_text.empty()) {
        EmitAssistantMessage(display_text);
    }
    bool played = true;
    if (!response.tts_audio_url.empty()) {
        ESP_LOGI(TAG,
                 "Conversation TTS playback: requestId=%s ttsTextPresent=%s ttsAudioUrl=%s "
                 "providerFallback=%s providerErrorCode=%s welcomeAudioCached=%s "
                 "welcomeAudioCacheKey=%s",
                 response.request_id.c_str(), response.has_tts_text ? "true" : "false",
                 RedactUrlForLog(response.tts_audio_url).c_str(),
                 response.provider_fallback ? "true" : "false",
                 response.provider_error_code.c_str(),
                 response.welcome_audio_cached ? "true" : "false",
                 response.welcome_audio_cache_key.c_str());
        bool cache_played = allow_welcome_audio_cache && TryPlayWelcomeAudioCache(response);
        if (!cache_played && !DownloadAndPlayTtsAudio(response.tts_audio_url)) {
            ESP_LOGW(TAG, "Failed to download/play TTS audio; falling back to text-only feedback");
            played = false;
        } else if (wait_for_playback) {
            bool self_test = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                self_test = self_test_in_progress_;
            }
            if (self_test) {
                bool drained =
                    Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty(
                        kSelfTestPlaybackDrainTimeoutMs);
                if (!drained) {
                    ESP_LOGE(TAG,
                             "KIDS_ENGLISH_SELF_TEST_FAIL step=http_tts_playback_drain "
                             "reason=timeout timeoutMs=%d",
                             kSelfTestPlaybackDrainTimeoutMs);
                    played = false;
                }
            } else {
                Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty();
            }
        }
    } else {
        played = false;
    }

    if (defer_tts_stop) {
        QueuePendingTtsStop(display_text, should_continue,
                            should_continue ? nullptr : "http_turn_complete");
    } else {
        EmitConversationStop(display_text, should_continue, "http_turn_complete");
    }
    return played;
}

bool KidsEnglishProtocol::HandleWsAudioPlayback() {
    std::string transport;
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = ws_output_audio_transport_;
        url = ws_output_audio_url_;
    }
    if (transport == "url") {
        return HandleWsAudioUrl(url);
    }
    if (transport == "binary_chunk") {
        return HandleWsAudioBuffer();
    }
    return true;
}

bool KidsEnglishProtocol::HandleWsAudioUrl(const std::string& url) {
    if (url.empty()) {
        return false;
    }
    return DownloadAndPlayTtsAudio(ResolveAudioUrl(url));
}

void KidsEnglishProtocol::HandleWsAudioUrlAsync(const std::string& url) {
    if (url.empty()) {
        MarkWebSocketFallback();
        return;
    }

    auto args = new WsUrlAudioTaskArgs{this, ResolveAudioUrl(url)};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ws_url_audio_in_progress_ = true;
    }
    if (ws_event_group_ != nullptr) {
        xEventGroupClearBits(ws_event_group_, kWsUrlAudioFinishedEvent);
    }
    BaseType_t created = xTaskCreate(
        [](void* arg) {
            std::unique_ptr<WsUrlAudioTaskArgs> owned_args(static_cast<WsUrlAudioTaskArgs*>(arg));
            if (owned_args->self == nullptr ||
                !owned_args->self->DownloadAndPlayTtsAudio(owned_args->url)) {
                ESP_LOGW(TAG, "Failed to download/play WebSocket URL audio asynchronously");
            }
            {
                std::lock_guard<std::mutex> lock(owned_args->self->mutex_);
                owned_args->self->ws_url_audio_in_progress_ = false;
            }
            owned_args->self->TrySendWsPlaybackFinished("url_audio_finished");
            owned_args->self->MaybeEmitWsTtsStopAfterPlayback("url_audio_finished");
            if (owned_args->self->ws_event_group_ != nullptr) {
                xEventGroupSetBits(owned_args->self->ws_event_group_, kWsUrlAudioFinishedEvent);
            }
            vTaskDelete(NULL);
        },
        "kids_ws_url_audio", 4096 * 2, args, 3, nullptr);
    if (created != pdPASS) {
        delete args;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ws_url_audio_in_progress_ = false;
        }
        ESP_LOGW(TAG, "Failed to create WebSocket URL audio playback task");
        MarkWebSocketFallback();
    }
}

bool KidsEnglishProtocol::HandleWsAudioBuffer() {
    std::string audio_format;
    std::string audio_bytes;
    int sample_rate = kPracticeAudioSampleRate;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_format = ws_output_audio_format_;
        sample_rate = ws_output_audio_sample_rate_hz_;
        audio_bytes = std::move(ws_output_audio_buffer_);
        ws_output_audio_buffer_.clear();
    }
    if (audio_bytes.empty()) {
        ESP_LOGW(TAG, "WebSocket assistant audio buffer is empty");
        return false;
    }

    std::vector<int16_t> pcm;
    bool parsed = false;
    if (audio_format == "pcm_s16le" || audio_format == "pcm_s16le_16k_mono") {
        parsed = ParsePcm16MonoBytes(audio_bytes, pcm);
    } else {
        parsed = ParseWavPcm16Mono(audio_bytes, pcm, sample_rate);
    }
    if (!parsed) {
        ESP_LOGE(TAG, "Failed to parse WebSocket assistant audio: format=%s bytes=%u",
                 audio_format.c_str(), (unsigned)audio_bytes.size());
        return false;
    }

    Application::GetInstance().GetAudioService().PushPcmToPlaybackQueue(std::move(pcm),
                                                                        sample_rate);
    return true;
}

void KidsEnglishProtocol::ResetWsTurnMetricsLocked() { ws_turn_metrics_ = WsTurnMetrics{}; }

void KidsEnglishProtocol::LogWsTurnMetricsLocked(const char* reason) const {
    const auto& m = ws_turn_metrics_;
    if (!m.active && m.turn_begin_ms == 0) {
        return;
    }

    ESP_LOGI(
        TAG,
        "WS_NATIVE_TURN_METRICS reason=%s conversationId=%s clientTurnId=%s serverTurnId=%s "
        "beginMs=%u inputStoppedMs=%u audioSubmittedMs=%u speechStartedMs=%u "
        "speechStoppedMs=%u serverAutoEndMs=%u assistantAudioStartMs=%u firstBinaryFrameMs=%u "
        "firstPcmQueueMs=%u playbackStartedMs=%u assistantAudioEndMs=%u binaryEndMs=%u "
        "turnCompleteMs=%u playbackFinishedMs=%u beginToInputStoppedMs=%d "
        "beginToSubmittedMs=%d beginToSpeechStoppedMs=%d beginToServerAutoEndMs=%d "
        "submittedToAudioStartMs=%d submittedToFirstBinaryMs=%d firstBinaryToQueueMs=%d "
        "firstBinaryToPlaybackStartMs=%d playbackStartedToFinishedMs=%d "
        "submittedToTurnCompleteMs=%d inputBytes=%u inputFrames=%u inputLastSeq=%u "
        "lateInputFramesSuppressed=%u lateInputBytesSuppressed=%u audioBytes=%u "
        "audioChunks=%u playbackChunks=%u "
        "playbackSamples=%u queuePeak=%u queueMin=%u underruns=%u "
        "jitterPeakChunks=%u jitterPeakMs=%d jitterRebuffers=%u jitterReleases=%u "
        "speechStarted=%s speechStopped=%s serverAutoEnd=%s turnEndRequested=%s "
        "turnEndSent=%s lateTurnEndSuppressed=%s inputStopReason=%s turnEndRequestReason=%s "
        "turnEndRequestSource=%s serverAutoEndReason=%s "
        "speechStartedSeq=%d speechStoppedSeq=%d serverAutoEndSeq=%d fallback=%s "
        "fallbackReason=%s",
        reason == nullptr ? "unknown" : reason, m.conversation_id.c_str(), m.client_turn_id.c_str(),
        m.server_turn_id.c_str(), LogMs(m.turn_begin_ms), LogMs(m.input_stopped_ms),
        LogMs(m.audio_submitted_ms), LogMs(m.speech_started_ms), LogMs(m.speech_stopped_ms),
        LogMs(m.server_auto_end_ms), LogMs(m.assistant_audio_start_ms),
        LogMs(m.first_binary_frame_ms), LogMs(m.first_pcm_queue_ms), LogMs(m.playback_started_ms),
        LogMs(m.assistant_audio_end_ms), LogMs(m.binary_end_ms), LogMs(m.turn_complete_ms),
        LogMs(m.playback_finished_ms), LogDeltaMs(m.turn_begin_ms, m.input_stopped_ms),
        LogDeltaMs(m.turn_begin_ms, m.audio_submitted_ms),
        LogDeltaMs(m.turn_begin_ms, m.speech_stopped_ms),
        LogDeltaMs(m.turn_begin_ms, m.server_auto_end_ms),
        LogDeltaMs(m.audio_submitted_ms, m.assistant_audio_start_ms),
        LogDeltaMs(m.audio_submitted_ms, m.first_binary_frame_ms),
        LogDeltaMs(m.first_binary_frame_ms, m.first_pcm_queue_ms),
        LogDeltaMs(m.first_binary_frame_ms, m.playback_started_ms),
        LogDeltaMs(m.playback_started_ms, m.playback_finished_ms),
        LogDeltaMs(m.audio_submitted_ms, m.turn_complete_ms), (unsigned)m.input_audio_bytes_sent,
        (unsigned)m.input_audio_frames_sent, (unsigned)m.input_last_seq,
        (unsigned)m.input_late_frames_suppressed, (unsigned)m.input_late_bytes_suppressed,
        (unsigned)m.audio_bytes, (unsigned)m.audio_chunks, (unsigned)m.playback_chunks,
        (unsigned)m.playback_samples, (unsigned)m.queue_peak_depth, (unsigned)m.queue_min_depth,
        (unsigned)m.underruns, (unsigned)m.jitter_buffer_peak_chunks, m.jitter_buffer_peak_ms,
        (unsigned)m.jitter_rebuffers, (unsigned)m.jitter_releases,
        m.speech_started_received ? "true" : "false", m.speech_stopped_received ? "true" : "false",
        m.server_auto_end_received ? "true" : "false",
        m.turn_audio_end_requested ? "true" : "false",
        m.turn_audio_end_sent ? "true" : "false",
        m.late_turn_end_suppressed ? "true" : "false", m.input_stop_reason.c_str(),
        m.turn_audio_end_request_reason.c_str(), m.turn_audio_end_request_source.c_str(),
        m.server_auto_end_reason.c_str(), m.speech_started_seq, m.speech_stopped_seq,
        m.server_auto_end_seq, m.fallback_requested ? "true" : "false", m.fallback_reason.c_str());
}

bool KidsEnglishProtocol::IsCurrentWsTurnEventLocked(const std::string& conversation_id,
                                                     const std::string& client_turn_id) const {
    if (!ws_turn_metrics_.active || ws_turn_metrics_.conversation_id.empty()) {
        return false;
    }
    if (!conversation_id.empty() && conversation_id != ws_turn_metrics_.conversation_id) {
        return false;
    }
    if (!client_turn_id.empty() && client_turn_id != ws_turn_metrics_.client_turn_id &&
        client_turn_id != ws_turn_metrics_.server_turn_id) {
        return false;
    }
    return true;
}

int KidsEnglishProtocol::WsInputDurationMsLocked() const {
    if (ws_turn_metrics_.input_audio_bytes_sent == 0) {
        return 0;
    }
    return static_cast<int>(ws_turn_metrics_.input_audio_bytes_sent * 1000 /
                            (kPracticeAudioSampleRate * kPracticeAudioChannels * sizeof(int16_t)));
}

bool KidsEnglishProtocol::PushWsPcmAudioChunk(const char* payload, size_t len, int sample_rate) {
    std::vector<int16_t> pcm;
    if (!ParsePcm16MonoBytes(std::string(payload, len), pcm)) {
        return false;
    }

    bool should_flush = false;
    bool direct_push = false;
    bool buffer_chunk = false;
    std::string conversation_id;
    std::string turn_id;
    size_t buffered_chunks = 0;
    int buffered_ms = 0;
    bool rebuffering = false;
    size_t min_chunks = kWsOutputJitterMinChunks;
    int min_ms = kWsOutputJitterMinMs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ws_output_playback_pending_) {
            direct_push = true;
        } else if (!ws_output_jitter_buffering_ && ws_output_jitter_chunks_.empty()) {
            direct_push = true;
            conversation_id = ws_output_audio_conversation_id_;
            turn_id = ws_output_audio_turn_id_;
        } else {
            buffer_chunk = true;
            if (ws_output_jitter_chunks_.empty()) {
                ws_output_jitter_sample_rate_hz_ = sample_rate;
            } else if (sample_rate != ws_output_jitter_sample_rate_hz_) {
                ESP_LOGW(TAG, "WebSocket PCM sample rate changed during segment: old=%d new=%d",
                         ws_output_jitter_sample_rate_hz_, sample_rate);
                ws_output_jitter_sample_rate_hz_ = sample_rate;
            }
            ws_output_jitter_samples_ += pcm.size();
            ws_output_jitter_chunks_.push_back(std::move(pcm));
            ws_output_jitter_buffering_ = true;
            buffered_chunks = ws_output_jitter_chunks_.size();
            buffered_ms = WsJitterBufferDurationMsLocked();
            conversation_id = ws_output_audio_conversation_id_;
            turn_id = ws_output_audio_turn_id_;
            if (ws_turn_metrics_.active && conversation_id == ws_turn_metrics_.conversation_id) {
                ws_turn_metrics_.jitter_buffer_peak_chunks =
                    std::max(ws_turn_metrics_.jitter_buffer_peak_chunks, buffered_chunks);
                ws_turn_metrics_.jitter_buffer_peak_ms =
                    std::max(ws_turn_metrics_.jitter_buffer_peak_ms, buffered_ms);
            }
            should_flush = IsWsJitterBufferReadyLocked();
            rebuffering = ws_output_jitter_rebuffering_;
            min_chunks = rebuffering ? kWsOutputRebufferMinChunks : kWsOutputJitterMinChunks;
            min_ms = rebuffering ? kWsOutputRebufferMinMs : kWsOutputJitterMinMs;
        }
    }
    if (direct_push) {
        ESP_LOGD(TAG, "WS_NATIVE_JITTER_DIRECT timeMs=%u conversationId=%s turnId=%s samples=%u",
                 LogMs(NowMs()), conversation_id.c_str(), turn_id.c_str(), (unsigned)pcm.size());
        Application::GetInstance().GetAudioService().PushPcmToPlaybackQueue(std::move(pcm),
                                                                            sample_rate);
        return true;
    }
    if (!buffer_chunk) {
        return true;
    }
    ESP_LOGI(TAG,
             "WS_NATIVE_JITTER_BUFFER timeMs=%u conversationId=%s turnId=%s chunks=%u "
             "durationMs=%d ready=%s rebuffer=%s minChunks=%u minMs=%d",
             LogMs(NowMs()), conversation_id.c_str(), turn_id.c_str(), (unsigned)buffered_chunks,
             buffered_ms, should_flush ? "true" : "false", rebuffering ? "true" : "false",
             (unsigned)min_chunks, min_ms);
    if (should_flush) {
        return FlushWsJitterBuffer("low_watermark");
    }
    return true;
}

bool KidsEnglishProtocol::FlushWsJitterBuffer(const char* reason) {
    std::vector<std::vector<int16_t>> chunks;
    int sample_rate = kPracticeAudioSampleRate;
    std::string conversation_id;
    std::string turn_id;
    size_t samples = 0;
    int duration_ms = 0;
    bool rebuffer_release = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ws_output_jitter_chunks_.empty()) {
            return true;
        }
        chunks.swap(ws_output_jitter_chunks_);
        samples = ws_output_jitter_samples_;
        duration_ms = WsJitterBufferDurationMsLocked();
        ws_output_jitter_samples_ = 0;
        sample_rate = ws_output_jitter_sample_rate_hz_;
        conversation_id = ws_output_audio_conversation_id_;
        turn_id = ws_output_audio_turn_id_;
        rebuffer_release = ws_output_jitter_rebuffering_;
        ws_output_jitter_buffering_ = false;
        ws_output_jitter_rebuffering_ = false;
        if (ws_turn_metrics_.active && conversation_id == ws_turn_metrics_.conversation_id) {
            ++ws_turn_metrics_.jitter_releases;
        }
    }

    ESP_LOGI(TAG,
             "WS_NATIVE_JITTER_RELEASE timeMs=%u reason=%s conversationId=%s turnId=%s "
             "chunks=%u samples=%u durationMs=%d rebuffer=%s",
             LogMs(NowMs()), reason == nullptr ? "unknown" : reason, conversation_id.c_str(),
             turn_id.c_str(), (unsigned)chunks.size(), (unsigned)samples, duration_ms,
             rebuffer_release ? "true" : "false");
    std::vector<int16_t> merged;
    merged.reserve(samples);
    for (const auto& chunk : chunks) {
        merged.insert(merged.end(), chunk.begin(), chunk.end());
    }
    Application::GetInstance().GetAudioService().PushPcmToPlaybackQueue(std::move(merged),
                                                                        sample_rate);
    return true;
}

void KidsEnglishProtocol::ResetWsJitterBufferLocked() {
    ws_output_jitter_buffering_ = false;
    ws_output_jitter_rebuffering_ = false;
    ws_output_jitter_sample_rate_hz_ = kPracticeAudioSampleRate;
    ws_output_jitter_samples_ = 0;
    ws_output_jitter_chunks_.clear();
}

bool KidsEnglishProtocol::IsWsJitterBufferReadyLocked() const {
    size_t min_chunks =
        ws_output_jitter_rebuffering_ ? kWsOutputRebufferMinChunks : kWsOutputJitterMinChunks;
    int min_ms = ws_output_jitter_rebuffering_ ? kWsOutputRebufferMinMs : kWsOutputJitterMinMs;
    return ws_output_jitter_chunks_.size() >= min_chunks &&
           WsJitterBufferDurationMsLocked() >= min_ms;
}

int KidsEnglishProtocol::WsJitterBufferDurationMsLocked() const {
    if (ws_output_jitter_sample_rate_hz_ <= 0) {
        return 0;
    }
    return static_cast<int>(ws_output_jitter_samples_ * 1000 / ws_output_jitter_sample_rate_hz_);
}

std::string KidsEnglishProtocol::ResolveAudioUrl(const std::string& url) const {
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
        return url;
    }
    if (!url.empty() && url.front() == '/') {
        return base_url_ + url;
    }
    return url;
}

std::string KidsEnglishProtocol::RedactUrlForLog(const std::string& url) const {
    size_t query = url.find('?');
    if (query == std::string::npos) {
        return url;
    }
    return url.substr(0, query) + "?<redacted>";
}

void KidsEnglishProtocol::HandleWsTextFrame(const char* data, size_t len) {
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        ESP_LOGW(TAG, "Ignoring invalid WebSocket JSON frame");
        MarkWebSocketFallback();
        return;
    }

    auto type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        ESP_LOGW(TAG, "Ignoring WebSocket JSON frame without type");
        cJSON_Delete(root);
        return;
    }

    const std::string event_type = type->valuestring;
    if (event_type == "session.ready") {
        HandleWsSessionReady(root);
    } else if (event_type == "ping") {
        cJSON* payload = cJSON_CreateObject();
        SendWsTextFrame(BuildWsJsonEnvelope("pong", payload));
    } else if (event_type == "pong" || event_type == "ack" || event_type == "buffer.status" ||
               event_type == "server.state" || event_type == "asr.partial" ||
               event_type == "assistant.text.delta") {
        // Optional or diagnostic events; receipt updates last_incoming_time_ above.
    } else if (event_type == "conversation.started") {
        HandleWsConversationStarted(root);
    } else if (event_type == "assistant.text.final") {
        auto payload = cJSON_GetObjectItem(root, "payload");
        auto text = cJSON_GetObjectItem(payload, "text");
        if (cJSON_IsString(text)) {
            EmitAssistantMessage(text->valuestring);
        }
    } else if (event_type == "assistant.audio.start") {
        HandleWsAssistantAudioStart(root);
    } else if (event_type == "assistant.audio.end") {
        HandleWsAssistantAudioEnd(root);
    } else if (event_type == "asr.final") {
        auto payload = cJSON_GetObjectItem(root, "payload");
        auto transcript = cJSON_GetObjectItem(payload, "transcript");
        if (cJSON_IsString(transcript) && JsonString(transcript).size() > 0) {
            EmitSttMessage(transcript->valuestring);
        }
    } else if (event_type == "asr.speech_started") {
        HandleWsAsrSpeechEndpoint(root, false);
    } else if (event_type == "asr.speech_stopped") {
        HandleWsAsrSpeechEndpoint(root, true);
    } else if (event_type == "turn.audio.auto_end") {
        HandleWsTurnAudioAutoEnd(root);
    } else if (event_type == "turn.complete") {
        HandleWsTurnComplete(root);
    } else if (event_type == "server.fallback") {
        HandleWsServerFallback(root);
    } else if (event_type == "conversation.ended") {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ws_should_continue_listening_ = false;
            if (!ws_turn_metrics_.turn_complete) {
                ws_waiting_turn_complete_ = false;
            }
        }
        MaybeEmitWsTtsStopAfterPlayback("ws_conversation_ended_event");
        xEventGroupSetBits(ws_event_group_, kWsConversationEndedEvent);
    } else if (event_type == "error") {
        HandleWsError(root);
    } else {
        ESP_LOGD(TAG, "Ignoring unknown WebSocket event: %s", event_type.c_str());
    }

    cJSON_Delete(root);
}

void KidsEnglishProtocol::HandleWsBinaryFrame(const char* data, size_t len) {
    if (len < kWsAudioHeaderBytes || std::memcmp(data, kWsAudioMagic, 4) != 0) {
        ESP_LOGW(TAG, "Ignoring invalid WebSocket audio frame");
        MarkWebSocketFallback();
        return;
    }
    uint8_t version = static_cast<uint8_t>(data[4]);
    uint8_t direction = static_cast<uint8_t>(data[5]);
    uint16_t flags = ReadBe16(data + 6);
    uint32_t payload_len = ReadBe32(data + 12);
    if (version != 1 || direction != 2 || payload_len != len - kWsAudioHeaderBytes) {
        ESP_LOGW(TAG, "Ignoring malformed WebSocket audio frame: version=%u direction=%u bytes=%u",
                 version, direction, (unsigned)len);
        MarkWebSocketFallback();
        return;
    }

    bool should_finalize = false;
    bool stream_pcm_chunk = false;
    int sample_rate = kPracticeAudioSampleRate;
    std::string conversation_id;
    std::string turn_id;
    bool first_frame = false;
    int64_t now_ms = NowMs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ws_output_audio_active_ || ws_output_audio_transport_ != "binary_chunk") {
            ESP_LOGW(TAG, "Ignoring WebSocket audio chunk outside binary assistant audio segment");
            return;
        }
        conversation_id = ws_output_audio_conversation_id_;
        turn_id = ws_output_audio_turn_id_;
        stream_pcm_chunk = ws_output_audio_format_ == "pcm_s16le" ||
                           ws_output_audio_format_ == "pcm_s16le_16k_mono";
        sample_rate = ws_output_audio_sample_rate_hz_;
        if (ws_turn_metrics_.active && conversation_id == ws_turn_metrics_.conversation_id) {
            first_frame = ws_turn_metrics_.first_binary_frame_ms == 0;
            if (first_frame) {
                ws_turn_metrics_.first_binary_frame_ms = now_ms;
            }
            ws_turn_metrics_.audio_bytes += payload_len;
            ++ws_turn_metrics_.audio_chunks;
        }
        if (!stream_pcm_chunk) {
            ws_output_audio_buffer_.append(data + kWsAudioHeaderBytes, payload_len);
        }
        should_finalize = (flags & 1) != 0;
    }
    if (first_frame) {
        ESP_LOGI(TAG,
                 "WS_NATIVE_FIRST_BINARY_AUDIO_FRAME timeMs=%u conversationId=%s turnId=%s "
                 "bytes=%u flags=0x%04x",
                 LogMs(now_ms), conversation_id.c_str(), turn_id.c_str(), (unsigned)payload_len,
                 flags);
    }
    if (stream_pcm_chunk && payload_len > 0 &&
        !PushWsPcmAudioChunk(data + kWsAudioHeaderBytes, payload_len, sample_rate)) {
        MarkWebSocketFallback();
        return;
    }
    if (should_finalize) {
        if (stream_pcm_chunk) {
            FlushWsJitterBuffer("binary_end");
        } else {
            HandleWsAudioPlayback();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ws_output_audio_finalized_ = true;
            ws_output_audio_active_ = false;
            if (ws_turn_metrics_.active && conversation_id == ws_turn_metrics_.conversation_id) {
                ws_turn_metrics_.binary_end_ms = NowMs();
            }
        }
        ESP_LOGI(TAG,
                 "WS_NATIVE_BINARY_END_OF_SEGMENT timeMs=%u conversationId=%s turnId=%s "
                 "flags=0x%04x audioBytes=%u chunks=%u",
                 LogMs(NowMs()), conversation_id.c_str(), turn_id.c_str(), flags,
                 (unsigned)payload_len, 1u);
        TrySendWsPlaybackFinished("binary_end");
    }
}

void KidsEnglishProtocol::HandleWsSessionReady(const cJSON* root) {
    auto session_id = cJSON_GetObjectItem(root, "sessionId");
    if (!cJSON_IsString(session_id)) {
        ESP_LOGW(TAG, "WebSocket session.ready missing sessionId");
        MarkWebSocketFallback();
        return;
    }
    auto payload = cJSON_GetObjectItem(root, "payload");
    auto ping_interval = cJSON_GetObjectItem(payload, "pingIntervalMs");
    auto idle_timeout = cJSON_GetObjectItem(payload, "idleTimeoutMs");
    auto max_frame_bytes = cJSON_GetObjectItem(payload, "maxFrameBytes");
    auto input_audio = cJSON_GetObjectItem(payload, "inputAudio");
    auto output_audio = cJSON_GetObjectItem(payload, "outputAudio");
    bool feature_asr_partial = false;
    bool feature_asr_speech_endpoint = false;
    bool feature_server_vad_auto_end = false;
    WebSocketTransportConfig config_for_log;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_id_ = session_id->valuestring;
        if (cJSON_IsNumber(ping_interval)) {
            ws_config_.ping_interval_ms = ping_interval->valueint;
        }
        if (cJSON_IsNumber(idle_timeout)) {
            ws_config_.idle_timeout_ms = idle_timeout->valueint;
        }
        if (cJSON_IsNumber(max_frame_bytes)) {
            ws_config_.max_frame_bytes = max_frame_bytes->valueint;
        }
        ws_config_.input_sample_rate_hz = JsonInt(cJSON_GetObjectItem(input_audio, "sampleRateHz"),
                                                  ws_config_.input_sample_rate_hz);
        ws_config_.input_channels =
            JsonInt(cJSON_GetObjectItem(input_audio, "channels"), ws_config_.input_channels);
        ws_config_.input_frame_duration_ms =
            JsonInt(cJSON_GetObjectItem(input_audio, "frameDurationMs"),
                    ws_config_.input_frame_duration_ms);
        ParseWsOutputAudioConfig(output_audio, ws_config_);
        auto features = cJSON_GetObjectItem(payload, "features");
        ws_config_.feature_asr_partial =
            JsonBool(cJSON_GetObjectItem(features, "asrPartial"), ws_config_.feature_asr_partial);
        ws_config_.feature_asr_speech_endpoint =
            JsonBool(cJSON_GetObjectItem(features, "asrSpeechEndpoint"),
                     ws_config_.feature_asr_speech_endpoint);
        ws_config_.feature_server_vad_auto_end =
            JsonBool(cJSON_GetObjectItem(features, "serverVadAutoEnd"),
                     ws_config_.feature_server_vad_auto_end);
        feature_asr_partial = ws_config_.feature_asr_partial;
        feature_asr_speech_endpoint = ws_config_.feature_asr_speech_endpoint;
        feature_server_vad_auto_end = ws_config_.feature_server_vad_auto_end;
        config_for_log = ws_config_;
    }
    ESP_LOGI(TAG,
             "WS_NATIVE_SESSION_READY_FEATURES session.ready.features asrPartial=%s "
             "asrSpeechEndpoint=%s serverVadAutoEnd=%s",
             feature_asr_partial ? "true" : "false", feature_asr_speech_endpoint ? "true" : "false",
             feature_server_vad_auto_end ? "true" : "false");
    LogWsOutputAudioConfig("session.ready", config_for_log);
    xEventGroupSetBits(ws_event_group_, kWsSessionReadyEvent);
}

void KidsEnglishProtocol::HandleWsConversationStarted(const cJSON* root) {
    auto payload = cJSON_GetObjectItem(root, "payload");
    std::string conversation_id = JsonString(cJSON_GetObjectItem(payload, "conversationId"));
    if (conversation_id.empty()) {
        conversation_id = JsonString(cJSON_GetObjectItem(root, "conversationId"));
    }
    if (conversation_id.empty()) {
        ESP_LOGW(TAG, "WebSocket conversation.started missing conversationId");
        MarkWebSocketFallback();
        return;
    }

    auto should_continue = cJSON_GetObjectItem(payload, "shouldContinueListening");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        conversation_id_ = conversation_id;
        ws_should_continue_listening_ = JsonBool(should_continue, true);
    }
    ESP_LOGI(TAG, "Started WebSocket conversation %s", conversation_id.c_str());
    xEventGroupSetBits(ws_event_group_, kWsConversationStartedEvent);
}

void KidsEnglishProtocol::HandleWsAssistantAudioStart(const cJSON* root) {
    auto payload = cJSON_GetObjectItem(root, "payload");
    std::string transport = JsonString(cJSON_GetObjectItem(payload, "transport"));
    std::string audio_format = JsonString(cJSON_GetObjectItem(payload, "audioFormat"));
    std::string url = JsonString(cJSON_GetObjectItem(payload, "url"));
    std::string conversation_id = JsonString(cJSON_GetObjectItem(payload, "conversationId"));
    std::string turn_id = JsonString(cJSON_GetObjectItem(payload, "turnId"));
    if (conversation_id.empty()) {
        conversation_id = JsonString(cJSON_GetObjectItem(root, "conversationId"));
    }
    if (turn_id.empty()) {
        turn_id = JsonString(cJSON_GetObjectItem(root, "turnId"));
    }
    int sample_rate =
        JsonInt(cJSON_GetObjectItem(payload, "sampleRateHz"), kPracticeAudioSampleRate);
    int channels = JsonInt(cJSON_GetObjectItem(payload, "channels"), kPracticeAudioChannels);

    auto audio_format_object = cJSON_GetObjectItem(payload, "audioFormat");
    if (cJSON_IsObject(audio_format_object)) {
        transport = JsonString(cJSON_GetObjectItem(audio_format_object, "transport"));
        audio_format = JsonString(cJSON_GetObjectItem(audio_format_object, "codec"));
        url = JsonString(cJSON_GetObjectItem(audio_format_object, "url"));
        sample_rate =
            JsonInt(cJSON_GetObjectItem(audio_format_object, "sampleRateHz"), sample_rate);
        channels = JsonInt(cJSON_GetObjectItem(audio_format_object, "channels"), channels);
    }
    if (audio_format.empty()) {
        audio_format = "wav";
    }
    if (transport.empty() && !url.empty()) {
        transport = "url";
    }
    bool is_native_pcm_stream =
        transport == "binary_chunk" &&
        (audio_format == "pcm_s16le" || audio_format == "pcm_s16le_16k_mono");
    bool turn_id_present = !turn_id.empty();
    int64_t now_ms = NowMs();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (conversation_id.empty()) {
            conversation_id = conversation_id_;
        }
        if (turn_id.empty() && ws_turn_metrics_.active) {
            turn_id = ws_turn_metrics_.client_turn_id;
        }
        ws_output_audio_active_ = true;
        ws_output_audio_finalized_ = false;
        ws_output_audio_end_received_ = false;
        ws_output_playback_pending_ = true;
        ws_output_playback_started_sent_ = false;
        ws_output_playback_finished_sent_ = false;
        ws_output_playback_queue_drained_ = false;
        ws_tts_playback_started_ = true;
        ws_output_audio_transport_ = transport;
        ws_output_audio_format_ = audio_format;
        ws_output_audio_url_ = url;
        ws_output_audio_conversation_id_ = conversation_id;
        ws_output_audio_turn_id_ = turn_id;
        ws_output_audio_buffer_.clear();
        ws_output_audio_sample_rate_hz_ = sample_rate;
        ws_output_audio_channels_ = channels;
        ResetWsJitterBufferLocked();
        if (is_native_pcm_stream) {
            ws_output_jitter_buffering_ = true;
            ws_output_jitter_rebuffering_ = false;
        }
        if (ws_turn_metrics_.active && conversation_id == ws_turn_metrics_.conversation_id &&
            ws_turn_metrics_.assistant_audio_start_ms == 0) {
            ws_turn_metrics_.assistant_audio_start_ms = now_ms;
        }
    }
    std::string redacted_url = url.empty() ? "" : RedactUrlForLog(url);
    ESP_LOGI(TAG,
             "WS_NATIVE_ASSISTANT_AUDIO_START timeMs=%u conversationId=%s turnId=%s "
             "turnIdPresent=%s audioFormat=%s transport=%s url=%s sampleRateHz=%d channels=%d",
             LogMs(now_ms), conversation_id.c_str(), turn_id.c_str(),
             turn_id_present ? "true" : "false", audio_format.c_str(), transport.c_str(),
             redacted_url.c_str(), sample_rate, channels);
    if (is_native_pcm_stream) {
        ESP_LOGI(TAG,
                 "WS_NATIVE_JITTER_RESET timeMs=%u conversationId=%s turnId=%s "
                 "initialMinChunks=%u initialMinMs=%d rebufferMinChunks=%u rebufferMinMs=%d "
                 "sampleRateHz=%d",
                 LogMs(now_ms), conversation_id.c_str(), turn_id.c_str(),
                 (unsigned)kWsOutputJitterMinChunks, kWsOutputJitterMinMs,
                 (unsigned)kWsOutputRebufferMinChunks, kWsOutputRebufferMinMs, sample_rate);
    }
    EmitTtsMessage("start");
    if (transport == "url") {
        HandleWsAudioUrlAsync(url);
        std::lock_guard<std::mutex> lock(mutex_);
        ws_output_audio_finalized_ = true;
    }
}

void KidsEnglishProtocol::HandleWsAssistantAudioEnd(const cJSON* root) {
    auto payload = cJSON_GetObjectItem(root, "payload");
    std::string conversation_id = JsonString(cJSON_GetObjectItem(payload, "conversationId"));
    std::string turn_id = JsonString(cJSON_GetObjectItem(payload, "turnId"));
    if (conversation_id.empty()) {
        conversation_id = JsonString(cJSON_GetObjectItem(root, "conversationId"));
    }
    if (turn_id.empty()) {
        turn_id = JsonString(cJSON_GetObjectItem(root, "turnId"));
    }
    bool should_play = false;
    bool was_initial_audio = false;
    int64_t now_ms = NowMs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (conversation_id.empty()) {
            conversation_id = ws_output_audio_conversation_id_;
        }
        if (turn_id.empty()) {
            turn_id = ws_output_audio_turn_id_;
        }
        if (!turn_id.empty()) {
            ws_output_audio_turn_id_ = turn_id;
        }
        should_play = ws_output_audio_active_ && !ws_output_audio_finalized_ &&
                      ((ws_output_audio_transport_ == "binary_chunk" &&
                        (!ws_output_audio_buffer_.empty() || !ws_output_jitter_chunks_.empty())) ||
                       ws_output_audio_transport_ == "url");
        was_initial_audio = ws_waiting_initial_audio_;
        ws_output_audio_end_received_ = true;
        if (ws_turn_metrics_.active && conversation_id == ws_turn_metrics_.conversation_id) {
            ws_turn_metrics_.assistant_audio_end_ms = now_ms;
        }
    }
    ESP_LOGI(TAG, "WS_NATIVE_ASSISTANT_AUDIO_END timeMs=%u conversationId=%s turnId=%s",
             LogMs(now_ms), conversation_id.c_str(), turn_id.c_str());
    if (should_play) {
        if (ws_output_audio_transport_ == "binary_chunk") {
            FlushWsJitterBuffer("assistant_audio_end");
        } else {
            HandleWsAudioPlayback();
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ws_output_audio_active_ = false;
        ws_output_audio_finalized_ = true;
    }
    if (was_initial_audio) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ws_waiting_initial_audio_ = false;
        }
        xEventGroupSetBits(ws_event_group_, kWsAssistantAudioEndEvent);
    }
    TrySendWsPlaybackFinished("assistant_audio_end");
    MaybeEmitWsTtsStopAfterPlayback("assistant_audio_end");
}

void KidsEnglishProtocol::HandleWsTurnComplete(const cJSON* root) {
    auto payload = cJSON_GetObjectItem(root, "payload");
    bool should_continue = JsonBool(cJSON_GetObjectItem(payload, "shouldContinueListening"), true);
    std::string turn_id = JsonString(cJSON_GetObjectItem(payload, "turnId"));
    if (turn_id.empty()) {
        turn_id = JsonString(cJSON_GetObjectItem(root, "turnId"));
    }
    std::string conversation_id = JsonString(cJSON_GetObjectItem(payload, "conversationId"));
    if (conversation_id.empty()) {
        conversation_id = JsonString(cJSON_GetObjectItem(root, "conversationId"));
    }
    auto diagnostics = cJSON_GetObjectItem(payload, "diagnostics");
    char* diagnostics_json =
        cJSON_IsObject(diagnostics) ? cJSON_PrintUnformatted(diagnostics) : nullptr;
    std::string diagnostics_text = diagnostics_json == nullptr ? "{}" : diagnostics_json;
    if (diagnostics_json != nullptr) {
        cJSON_free(diagnostics_json);
    }
    int64_t now_ms = NowMs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (conversation_id.empty()) {
            conversation_id = ws_turn_metrics_.conversation_id;
        }
        ws_waiting_turn_complete_ = false;
        ws_should_continue_listening_ = should_continue;
        if (!turn_id.empty()) {
            last_turn_id_ = turn_id;
            ws_output_audio_turn_id_ = turn_id;
            ws_turn_metrics_.server_turn_id = turn_id;
        }
        if (ws_turn_metrics_.active && conversation_id == ws_turn_metrics_.conversation_id) {
            ws_turn_metrics_.turn_complete = true;
            ws_turn_metrics_.turn_complete_ms = now_ms;
            LogWsTurnMetricsLocked("turn_complete");
        }
    }
    ESP_LOGI(TAG,
             "WS_NATIVE_TURN_COMPLETE timeMs=%u conversationId=%s turnId=%s "
             "shouldContinueListening=%s diagnostics=%s",
             LogMs(now_ms), conversation_id.c_str(), turn_id.c_str(),
             should_continue ? "true" : "false", diagnostics_text.c_str());
    TrySendWsPlaybackFinished("turn_complete");
    MaybeEmitWsTtsStopAfterPlayback("turn_complete");
    xEventGroupSetBits(ws_event_group_, kWsTurnCompleteEvent);
}

void KidsEnglishProtocol::HandleWsAsrSpeechEndpoint(const cJSON* root, bool stopped) {
    auto payload = cJSON_GetObjectItem(root, "payload");
    std::string conversation_id = JsonString(cJSON_GetObjectItem(payload, "conversationId"));
    std::string client_turn_id = JsonString(cJSON_GetObjectItem(payload, "clientTurnId"));
    if (conversation_id.empty()) {
        conversation_id = JsonString(cJSON_GetObjectItem(root, "conversationId"));
    }
    if (client_turn_id.empty()) {
        client_turn_id = JsonString(cJSON_GetObjectItem(root, "turnId"));
    }
    int seq = JsonInt(cJSON_GetObjectItem(root, "seq"));
    int elapsed_ms = JsonInt(cJSON_GetObjectItem(payload, "elapsedMs"));
    std::string provider = JsonString(cJSON_GetObjectItem(payload, "provider"));
    std::string payload_text = JsonCompactString(payload);
    bool active_turn = false;
    bool input_active = false;
    bool asr_speech_endpoint = false;
    bool server_vad_auto_end = false;
    bool should_request_turn_end = false;
    int begin_to_endpoint_ms = -1;
    int64_t now_ms = NowMs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_turn = IsCurrentWsTurnEventLocked(conversation_id, client_turn_id);
        input_active = ws_turn_metrics_.input_audio_active;
        asr_speech_endpoint = ws_config_.feature_asr_speech_endpoint;
        server_vad_auto_end = ws_config_.feature_server_vad_auto_end;
        if (active_turn) {
            if (stopped) {
                ws_turn_metrics_.speech_stopped_received = true;
                ws_turn_metrics_.speech_stopped_ms = now_ms;
                ws_turn_metrics_.speech_stopped_seq = seq;
                if (input_active && asr_speech_endpoint && !server_vad_auto_end &&
                    !ws_turn_metrics_.turn_audio_end_sent &&
                    !ws_turn_metrics_.server_auto_end_received &&
                    !ws_turn_metrics_.turn_audio_end_requested) {
                    ws_turn_metrics_.turn_audio_end_requested = true;
                    ws_turn_metrics_.turn_audio_end_requested_ms = now_ms;
                    ws_turn_metrics_.turn_audio_end_request_reason = "vad_silence";
                    ws_turn_metrics_.turn_audio_end_request_source = "asr_speech_stopped";
                    should_request_turn_end = true;
                }
            } else {
                ws_turn_metrics_.speech_started_received = true;
                ws_turn_metrics_.speech_started_ms = now_ms;
                ws_turn_metrics_.speech_started_seq = seq;
            }
            begin_to_endpoint_ms = LogDeltaMs(ws_turn_metrics_.turn_begin_ms, now_ms);
        }
    }

    ESP_LOGI(TAG,
             "WS_NATIVE_ASR_%s timeMs=%u conversationId=%s clientTurnId=%s seq=%d "
             "provider=%s elapsedMs=%d activeTurn=%s inputActive=%s features={asrSpeechEndpoint:%s,"
             "serverVadAutoEnd:%s} beginToEndpointMs=%d payload=%s",
             stopped ? "SPEECH_STOPPED" : "SPEECH_STARTED", LogMs(now_ms), conversation_id.c_str(),
             client_turn_id.c_str(), seq, provider.c_str(), elapsed_ms,
             active_turn ? "true" : "false", input_active ? "true" : "false",
             asr_speech_endpoint ? "true" : "false", server_vad_auto_end ? "true" : "false",
             begin_to_endpoint_ms, payload_text.c_str());

    if (stopped && server_vad_auto_end) {
        ESP_LOGI(TAG,
                 "WS_NATIVE_ASR_SPEECH_STOPPED_DIAGNOSTIC_ONLY timeMs=%u conversationId=%s "
                 "clientTurnId=%s waitingForServerAutoEnd=true",
                 LogMs(now_ms), conversation_id.c_str(), client_turn_id.c_str());
    }
    if (should_request_turn_end) {
        ESP_LOGI(TAG,
                 "WS_NATIVE_TURN_AUDIO_END_REQUESTED timeMs=%u conversationId=%s clientTurnId=%s "
                 "reason=vad_silence source=asr_speech_stopped beginToEndpointMs=%d",
                 LogMs(now_ms), conversation_id.c_str(), client_turn_id.c_str(),
                 begin_to_endpoint_ms);
    }
}

void KidsEnglishProtocol::HandleWsTurnAudioAutoEnd(const cJSON* root) {
    auto payload = cJSON_GetObjectItem(root, "payload");
    std::string conversation_id = JsonString(cJSON_GetObjectItem(payload, "conversationId"));
    std::string client_turn_id = JsonString(cJSON_GetObjectItem(payload, "clientTurnId"));
    if (conversation_id.empty()) {
        conversation_id = JsonString(cJSON_GetObjectItem(root, "conversationId"));
    }
    if (client_turn_id.empty()) {
        client_turn_id = JsonString(cJSON_GetObjectItem(root, "turnId"));
    }
    int seq = JsonInt(cJSON_GetObjectItem(root, "seq"));
    std::string reason = JsonString(cJSON_GetObjectItem(payload, "reason"));
    std::string payload_text = JsonCompactString(payload);
    bool active_turn = false;
    bool was_input_active = false;
    bool turn_end_sent = false;
    int begin_to_auto_end_ms = -1;
    int begin_to_input_stopped_ms = -1;
    int duration_ms = 0;
    int64_t now_ms = NowMs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_turn = IsCurrentWsTurnEventLocked(conversation_id, client_turn_id);
        if (active_turn) {
            was_input_active = ws_turn_metrics_.input_audio_active;
            turn_end_sent = ws_turn_metrics_.turn_audio_end_sent;
            ws_turn_metrics_.server_auto_end_received = true;
            ws_turn_metrics_.server_auto_end_ms = now_ms;
            ws_turn_metrics_.server_auto_end_seq = seq;
            ws_turn_metrics_.server_auto_end_reason = reason.empty() ? "vad_silence" : reason;
            ws_turn_metrics_.input_audio_active = false;
            if (ws_turn_metrics_.input_stopped_ms == 0) {
                ws_turn_metrics_.input_stopped_ms = now_ms;
                ws_turn_metrics_.input_stop_reason = "server_vad_auto_end";
            }
            duration_ms = WsInputDurationMsLocked();
            begin_to_auto_end_ms = LogDeltaMs(ws_turn_metrics_.turn_begin_ms, now_ms);
            begin_to_input_stopped_ms =
                LogDeltaMs(ws_turn_metrics_.turn_begin_ms, ws_turn_metrics_.input_stopped_ms);
        }
    }

    ESP_LOGI(TAG,
             "WS_NATIVE_TURN_AUDIO_AUTO_END timeMs=%u conversationId=%s clientTurnId=%s "
             "seq=%d reason=%s activeTurn=%s wasInputActive=%s turnEndAlreadySent=%s "
             "durationMs=%d beginToAutoEndMs=%d beginToInputStoppedMs=%d payload=%s",
             LogMs(now_ms), conversation_id.c_str(), client_turn_id.c_str(), seq,
             reason.empty() ? "vad_silence" : reason.c_str(), active_turn ? "true" : "false",
             was_input_active ? "true" : "false", turn_end_sent ? "true" : "false", duration_ms,
             begin_to_auto_end_ms, begin_to_input_stopped_ms, payload_text.c_str());
}

void KidsEnglishProtocol::HandleWsServerFallback(const cJSON* root) {
    auto payload = cJSON_GetObjectItem(root, "payload");
    std::string reason = JsonString(cJSON_GetObjectItem(payload, "reason"));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ws_turn_metrics_.active) {
            ws_turn_metrics_.fallback_requested = true;
            ws_turn_metrics_.fallback_reason = reason;
        }
    }
    ESP_LOGW(TAG, "WebSocket server switched to adapter fallback path: reason=%s", reason.c_str());
}

void KidsEnglishProtocol::HandleWsError(const cJSON* root) {
    auto payload = cJSON_GetObjectItem(root, "payload");
    std::string code = JsonString(cJSON_GetObjectItem(payload, "code"));
    std::string fallback = JsonString(cJSON_GetObjectItem(payload, "fallback"));
    bool recoverable = JsonBool(cJSON_GetObjectItem(payload, "recoverable"), false);
    ESP_LOGW(TAG, "WebSocket error: code=%s recoverable=%s fallback=%s", code.c_str(),
             recoverable ? "true" : "false", fallback.c_str());
    if (fallback == "http" || !recoverable) {
        MarkWebSocketFallback();
    }
}

void KidsEnglishProtocol::MarkWebSocketFallback() {
    if (ws_event_group_ != nullptr) {
        xEventGroupSetBits(ws_event_group_, kWsReconnectRequiredEvent);
    }
}

bool KidsEnglishProtocol::IsWsPlaybackFinishedForWaitLocked() const {
    if (ws_url_audio_in_progress_) {
        return false;
    }
    if (!ws_output_playback_pending_) {
        return true;
    }
    return ws_output_audio_end_received_ && ws_output_playback_queue_drained_ &&
           ws_output_jitter_chunks_.empty();
}

bool KidsEnglishProtocol::WaitForWsPlaybackFinished(const char* step, unsigned turn,
                                                    int timeout_ms) {
    int64_t deadline_ms = NowMs() + timeout_ms;
    while (true) {
        bool finished = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished = IsWsPlaybackFinishedForWaitLocked();
        }
        if (finished) {
            Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty();
            ESP_LOGI(TAG, "WS_NATIVE_SELF_TEST_PLAYBACK_DRAINED step=%s turn=%u timeoutMs=%d",
                     step == nullptr ? "unknown" : step, turn, timeout_ms);
            return true;
        }
        int64_t now_ms = NowMs();
        if (now_ms >= deadline_ms) {
            break;
        }
        int wait_ms = static_cast<int>(std::min<int64_t>(1000, deadline_ms - now_ms));
        if (ws_event_group_ != nullptr) {
            xEventGroupWaitBits(ws_event_group_,
                                kWsPlaybackFinishedEvent | kWsAssistantAudioEndEvent |
                                    kWsTurnCompleteEvent | kWsUrlAudioFinishedEvent |
                                    kWsReconnectRequiredEvent | kWsDisconnectedEvent,
                                pdTRUE, pdFALSE, pdMS_TO_TICKS(wait_ms));
        } else {
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        }
    }

    bool pending = false;
    bool audio_end = false;
    bool queue_drained = false;
    bool url_in_progress = false;
    bool turn_complete = false;
    size_t jitter_chunks = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending = ws_output_playback_pending_;
        audio_end = ws_output_audio_end_received_;
        queue_drained = ws_output_playback_queue_drained_;
        url_in_progress = ws_url_audio_in_progress_;
        turn_complete = ws_turn_metrics_.turn_complete;
        jitter_chunks = ws_output_jitter_chunks_.size();
    }
    ESP_LOGE(TAG,
             "KIDS_ENGLISH_SELF_TEST_FAIL step=%s turn=%u reason=ws_playback_finish_timeout "
             "timeoutMs=%d pending=%s audioEnd=%s queueDrained=%s urlAudio=%s "
             "turnComplete=%s jitterChunks=%u",
             step == nullptr ? "unknown" : step, turn, timeout_ms, pending ? "true" : "false",
             audio_end ? "true" : "false", queue_drained ? "true" : "false",
             url_in_progress ? "true" : "false", turn_complete ? "true" : "false",
             (unsigned)jitter_chunks);
    return false;
}

void KidsEnglishProtocol::TrySendWsPlaybackFinished(const char* reason) {
    std::string conversation_id;
    std::string turn_id;
    bool should_send = false;
    int64_t now_ms = NowMs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ws_output_playback_pending_ || !ws_output_audio_end_received_ ||
            !ws_output_playback_queue_drained_ || ws_output_playback_finished_sent_) {
            return;
        }
        if (ws_turn_metrics_.active && ws_turn_metrics_.playback_started_ms == 0) {
            return;
        }
        conversation_id = ws_output_audio_conversation_id_;
        turn_id = ws_output_audio_turn_id_;
        if (conversation_id.empty() && ws_turn_metrics_.active) {
            conversation_id = ws_turn_metrics_.conversation_id;
        }
        if (turn_id.empty() && ws_turn_metrics_.active) {
            turn_id = ws_turn_metrics_.server_turn_id.empty() ? ws_turn_metrics_.client_turn_id
                                                              : ws_turn_metrics_.server_turn_id;
        }
        ws_output_playback_finished_sent_ = true;
        ws_output_playback_pending_ = false;
        if (ws_turn_metrics_.active && conversation_id == ws_turn_metrics_.conversation_id) {
            ws_turn_metrics_.playback_finished_ms = now_ms;
            LogWsTurnMetricsLocked(reason == nullptr ? "playback_finished" : reason);
        }
        should_send = !conversation_id.empty();
    }
    ESP_LOGI(TAG,
             "WS_NATIVE_PLAYBACK_FINISHED_READY timeMs=%u reason=%s conversationId=%s "
             "turnId=%s",
             LogMs(now_ms), reason == nullptr ? "unknown" : reason, conversation_id.c_str(),
             turn_id.c_str());
    if (should_send) {
        SendWsPlaybackEvent("playback.finished", conversation_id, turn_id);
    }
    if (ws_event_group_ != nullptr) {
        xEventGroupSetBits(ws_event_group_, kWsPlaybackFinishedEvent);
    }
}

void KidsEnglishProtocol::MaybeEmitWsTtsStopAfterPlayback(const char* reason) {
    bool should_emit = false;
    bool should_continue = true;
    bool self_test = false;
    bool should_clear_conversation = false;
    bool channel_opened = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ws_tts_playback_started_ && !ws_waiting_turn_complete_ &&
            IsWsPlaybackFinishedForWaitLocked()) {
            should_emit = true;
            ws_tts_playback_started_ = false;
            should_continue = ws_should_continue_listening_;
            self_test = self_test_in_progress_;
            should_clear_conversation = !self_test && !should_continue;
            channel_opened = channel_opened_;
        }
    }
    if (!should_emit) {
        return;
    }

    ESP_LOGI(TAG,
             "WS_NATIVE_TTS_STOP_AFTER_PLAYBACK timeMs=%u reason=%s continueListening=%s "
             "selfTest=%s",
             LogMs(NowMs()), reason == nullptr ? "unknown" : reason,
             should_continue ? "true" : "false", self_test ? "true" : "false");
    EmitTtsMessage("stop", nullptr, self_test ? false : should_continue);
    EmitEmotion(should_continue ? "happy" : "neutral");
    if (should_clear_conversation) {
        ClearServerEndedConversation(reason == nullptr ? "ws_playback_finished" : reason);
        if (channel_opened && on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    }
}

void KidsEnglishProtocol::OnPcmPlaybackQueued(size_t samples, size_t queue_depth) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ws_output_playback_pending_) {
        return;
    }
    if (ws_turn_metrics_.active &&
        ws_output_audio_conversation_id_ == ws_turn_metrics_.conversation_id &&
        ws_turn_metrics_.first_pcm_queue_ms == 0) {
        ws_turn_metrics_.first_pcm_queue_ms = NowMs();
        ESP_LOGI(TAG,
                 "WS_NATIVE_FIRST_PCM_QUEUE timeMs=%u conversationId=%s turnId=%s "
                 "samples=%u queueDepth=%u",
                 LogMs(ws_turn_metrics_.first_pcm_queue_ms),
                 ws_output_audio_conversation_id_.c_str(), ws_output_audio_turn_id_.c_str(),
                 (unsigned)samples, (unsigned)queue_depth);
    }
    if (ws_turn_metrics_.active &&
        ws_output_audio_conversation_id_ == ws_turn_metrics_.conversation_id) {
        ws_output_playback_queue_drained_ = false;
        ws_turn_metrics_.queue_peak_depth =
            std::max(ws_turn_metrics_.queue_peak_depth, queue_depth);
        if (ws_turn_metrics_.queue_min_depth == 0 ||
            queue_depth < ws_turn_metrics_.queue_min_depth) {
            ws_turn_metrics_.queue_min_depth = queue_depth;
        }
    }
    ESP_LOGD(TAG,
             "WS_NATIVE_PCM_QUEUE_PUSH timeMs=%u conversationId=%s turnId=%s samples=%u "
             "queueDepth=%u",
             LogMs(NowMs()), ws_output_audio_conversation_id_.c_str(),
             ws_output_audio_turn_id_.c_str(), (unsigned)samples, (unsigned)queue_depth);
}

void KidsEnglishProtocol::OnAudioPlaybackStarted(size_t samples, size_t remaining_queue_depth) {
    std::string conversation_id;
    std::string turn_id;
    bool should_send = false;
    int64_t now_ms = NowMs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ws_output_playback_pending_) {
            return;
        }
        conversation_id = ws_output_audio_conversation_id_;
        turn_id = ws_output_audio_turn_id_;
        if (conversation_id.empty() && ws_turn_metrics_.active) {
            conversation_id = ws_turn_metrics_.conversation_id;
        }
        if (turn_id.empty() && ws_turn_metrics_.active) {
            turn_id = ws_turn_metrics_.client_turn_id;
        }
        if (!ws_output_playback_started_sent_) {
            ws_output_playback_started_sent_ = true;
            should_send = !conversation_id.empty();
        }
        if (ws_turn_metrics_.active && conversation_id == ws_turn_metrics_.conversation_id) {
            if (ws_output_jitter_buffering_ && ws_output_jitter_rebuffering_ &&
                ws_output_jitter_chunks_.empty()) {
                ws_output_jitter_buffering_ = false;
                ws_output_jitter_rebuffering_ = false;
            }
            ws_output_playback_queue_drained_ = false;
            if (ws_turn_metrics_.playback_started_ms == 0) {
                ws_turn_metrics_.playback_started_ms = now_ms;
            }
            ++ws_turn_metrics_.playback_chunks;
            ws_turn_metrics_.playback_samples += samples;
            if (ws_turn_metrics_.queue_min_depth == 0 ||
                remaining_queue_depth < ws_turn_metrics_.queue_min_depth) {
                ws_turn_metrics_.queue_min_depth = remaining_queue_depth;
            }
        }
    }
    ESP_LOG_LEVEL(should_send ? ESP_LOG_INFO : ESP_LOG_DEBUG, TAG,
                  "WS_NATIVE_PLAYBACK_STARTED timeMs=%u conversationId=%s turnId=%s samples=%u "
                  "remainingQueueDepth=%u firstEvent=%s",
                  LogMs(now_ms), conversation_id.c_str(), turn_id.c_str(), (unsigned)samples,
                  (unsigned)remaining_queue_depth, should_send ? "true" : "false");
    if (should_send) {
        SendWsPlaybackEvent("playback.started", conversation_id, turn_id);
    }
}

void KidsEnglishProtocol::OnAudioPlaybackFinished(size_t samples, bool queue_drained) {
    bool pending = false;
    bool underrun = false;
    std::string conversation_id;
    std::string turn_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending = ws_output_playback_pending_;
        conversation_id = ws_output_audio_conversation_id_;
        turn_id = ws_output_audio_turn_id_;
        if (pending && ws_turn_metrics_.active &&
            conversation_id == ws_turn_metrics_.conversation_id) {
            if (queue_drained && !ws_output_audio_end_received_) {
                if (ws_output_jitter_chunks_.empty()) {
                    ++ws_turn_metrics_.underruns;
                    underrun = true;
                    if (!ws_output_jitter_rebuffering_ && !ws_output_jitter_buffering_) {
                        ++ws_turn_metrics_.jitter_rebuffers;
                    }
                    ws_output_jitter_rebuffering_ = true;
                    ws_output_jitter_buffering_ = true;
                }
            }
        }
        if (queue_drained) {
            ws_output_playback_queue_drained_ = true;
        }
    }
    if (!pending) {
        return;
    }
    ESP_LOG_LEVEL((queue_drained || underrun) ? ESP_LOG_INFO : ESP_LOG_DEBUG, TAG,
                  "WS_NATIVE_PLAYBACK_CHUNK_FINISHED timeMs=%u conversationId=%s turnId=%s "
                  "samples=%u queueDrained=%s underrun=%s",
                  LogMs(NowMs()), conversation_id.c_str(), turn_id.c_str(), (unsigned)samples,
                  queue_drained ? "true" : "false", underrun ? "true" : "false");
    if (queue_drained) {
        TrySendWsPlaybackFinished("playback_queue_drained");
        MaybeEmitWsTtsStopAfterPlayback("playback_queue_drained");
    }
}

bool KidsEnglishProtocol::HandleStandaloneTtsResponse(const StandaloneTtsResponse& response,
                                                      bool wait_for_playback) {
    ESP_LOGI(TAG,
             "Standalone TTS playback: requestId=%s ttsTextPresent=%s ttsText=%s "
             "ttsAudioUrl=%s format=%s providerFallback=%s providerErrorCode=%s",
             response.request_id.c_str(), response.has_tts_text ? "true" : "false",
             response.tts_text.c_str(), RedactUrlForLog(response.tts_audio_url).c_str(),
             response.audio_format.c_str(), response.provider_fallback ? "true" : "false",
             response.provider_error_code.c_str());

    EmitTtsMessage("start");
    if (!response.tts_text.empty()) {
        EmitAssistantMessage(response.tts_text);
    }

    bool played = true;
    if (!response.tts_audio_url.empty()) {
        if (!DownloadAndPlayTtsAudio(response.tts_audio_url)) {
            ESP_LOGW(TAG, "Failed to download/play standalone TTS audio");
            played = false;
        } else if (wait_for_playback) {
            Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty();
        }
    } else {
        played = false;
    }

    EmitTtsMessage("stop", response.tts_text.empty() ? nullptr : response.tts_text.c_str(), false);
    EmitEmotion("neutral");
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

void KidsEnglishProtocol::EmitConversationStarted() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "conversation");
    cJSON_AddStringToObject(root, "event", "started");
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

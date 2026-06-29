#include "kids_english_protocol.h"

#include "application.h"
#include "assets/lang_config.h"
#include "audio_service.h"
#include "board.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_ae_rate_cvt.h>
#include <esp_audio_types.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <http.h>
#include <mbedtls/md.h>
#include <web_socket.h>

#include <algorithm>
#include <chrono>
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
constexpr int kUploadTimeoutMs = 90000;
constexpr int kStandaloneSpeechTimeoutMs = 20000;
constexpr int kTtsDownloadTimeoutMs = 10000;
constexpr int kWsSessionReadyTimeoutMs = 10000;
constexpr int kWsConversationStartTimeoutMs = 30000;
constexpr int kWsHeartbeatMissesBeforeFallback = 3;
constexpr char kMultipartBoundary[] = "----xiaozhi-kids-english-boundary";
constexpr char kSelfTestSpeechText[] = "I like apples.";
constexpr const char* kSelfTestConversationTexts[] = {
    "I like apples.",
    "My apple is red.",
    "I have a small dog.",
    "The dog can run fast.",
    "I want to read a book.",
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
constexpr size_t kWsAudioHeaderBytes = 16;
constexpr int kMaxPracticeAudioDurationSeconds = 10;
constexpr size_t kMaxPracticeAudioBytes = 5 * 1024 * 1024;
constexpr size_t kWavHeaderBytes = 44;
constexpr int64_t kUnixTimeReasonableMs = 1600000000000LL;
constexpr EventBits_t kWsSessionReadyEvent = BIT0;
constexpr EventBits_t kWsConversationStartedEvent = BIT1;
constexpr EventBits_t kWsTurnCompleteEvent = BIT2;
constexpr EventBits_t kWsReconnectRequiredEvent = BIT3;
constexpr EventBits_t kWsDisconnectedEvent = BIT4;
constexpr EventBits_t kWsAssistantAudioEndEvent = BIT5;
constexpr EventBits_t kWsConversationEndedEvent = BIT6;
constexpr EventBits_t kWsUrlAudioFinishedEvent = BIT7;

struct WsUrlAudioTaskArgs {
    KidsEnglishProtocol* self = nullptr;
    std::string url;
};

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

uint32_t LogMs(int64_t ms) {
    return ms > 0 ? static_cast<uint32_t>(ms) : 0;
}

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
    : base_url_(TrimTrailingSlash(CONFIG_KIDS_ENGLISH_SERVER_URL)),
      device_id_(CONFIG_KIDS_ENGLISH_DEVICE_ID),
      device_secret_(CONFIG_KIDS_ENGLISH_DEVICE_SECRET) {
    server_sample_rate_ = kPracticeAudioSampleRate;
    server_frame_duration_ = OPUS_FRAME_DURATION_MS;
    ws_event_group_ = xEventGroupCreate();
}

KidsEnglishProtocol::~KidsEnglishProtocol() {
    CloseAudioChannel(false);
    if (ws_event_group_ != nullptr) {
        vEventGroupDelete(ws_event_group_);
        ws_event_group_ = nullptr;
    }
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
        if (!opened && ws_config.fallback_http) {
            ESP_LOGW(TAG, "Kids English WebSocket conversation unavailable; falling back to HTTP");
            CloseWebSocket();
            error_occurred_ = false;
            opened = StartHttpConversationWithFreshHello(trigger.c_str());
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
             standalone_tts.has_tts_text ? "true" : "false",
             standalone_tts.tts_text.c_str(),
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
    if (conversation_id.empty()) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=start_conversation reason=missing_id");
        return finish_self_test(false);
    }
    ESP_LOGI(TAG, "KIDS_ENGLISH_SELF_TEST_STEP start_conversation ok conversationId=%s transport=%s",
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
                EventBits_t bits =
                    xEventGroupWaitBits(ws_event_group_, kWsUrlAudioFinishedEvent |
                                                             kWsReconnectRequiredEvent |
                                                             kWsDisconnectedEvent,
                                        pdTRUE, pdFALSE,
                                        pdMS_TO_TICKS(kWsConversationStartTimeoutMs));
                if (!(bits & kWsUrlAudioFinishedEvent)) {
                    ESP_LOGE(TAG,
                             "KIDS_ENGLISH_SELF_TEST_FAIL step=%s turn=%u "
                             "reason=wait_url_audio",
                             step, turn);
                    return false;
                }
            }
        }
        Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty();
        return true;
    };

    if (self_test_transport == ConversationTransportMode::kWebSocket) {
        EventBits_t bits = xEventGroupWaitBits(ws_event_group_, kWsAssistantAudioEndEvent |
                                                                    kWsReconnectRequiredEvent |
                                                                    kWsDisconnectedEvent,
                                               pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(kWsConversationStartTimeoutMs));
        if (!(bits & kWsAssistantAudioEndEvent)) {
            ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=initial_ws_audio");
            return finish_self_test(false);
        }
    }
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
            ESP_LOGE(TAG,
                     "KIDS_ENGLISH_SELF_TEST_FAIL step=generate_turn_pcm turn=%u text=%s",
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
    ESP_LOGI(TAG, "KIDS_ENGLISH_SELF_TEST_STEP upload_audio ok conversationId=%s turns=%u transport=%s",
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

int64_t KidsEnglishProtocol::NowMs() const {
    return esp_timer_get_time() / 1000;
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
    auto transport = cJSON_GetObjectItem(data, "conversationTransport");
    auto transport_selected = cJSON_GetObjectItem(transport, "selected");
    auto transport_reason = cJSON_GetObjectItem(transport, "reason");
    auto transport_protocol = cJSON_GetObjectItem(transport, "protocol");

    ESP_LOGI(TAG,
             "Device hello: deviceId=%s state=%s languageLevel=%s voice=%s "
             "dailyPracticeMinutes=%d audioUpload=%s ttsUrl=%s streaming=%s "
             "transport=%s reason=%s protocol=%s",
             device_id_.c_str(), JsonString(device_state).c_str(),
             JsonString(cJSON_GetObjectItem(settings, "languageLevel")).c_str(),
             JsonString(cJSON_GetObjectItem(settings, "voice")).c_str(),
             JsonInt(cJSON_GetObjectItem(settings, "dailyPracticeMinutes"), 0),
             cJSON_IsTrue(audio_upload) ? "true" : "false", cJSON_IsTrue(tts_url) ? "true" : "false",
             cJSON_IsTrue(streaming) ? "true" : "false", JsonString(transport_selected).c_str(),
             JsonString(transport_reason).c_str(), JsonString(transport_protocol).c_str());
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
            config.ping_interval_ms = JsonInt(cJSON_GetObjectItem(transport, "pingIntervalMs"),
                                              config.ping_interval_ms);
            config.idle_timeout_ms = JsonInt(cJSON_GetObjectItem(transport, "idleTimeoutMs"),
                                             config.idle_timeout_ms);
            config.max_frame_bytes = JsonInt(cJSON_GetObjectItem(transport, "maxFrameBytes"),
                                             config.max_frame_bytes);

            auto input_audio = cJSON_GetObjectItem(transport, "inputAudio");
            config.input_sample_rate_hz =
                JsonInt(cJSON_GetObjectItem(input_audio, "sampleRateHz"), config.input_sample_rate_hz);
            config.input_channels =
                JsonInt(cJSON_GetObjectItem(input_audio, "channels"), config.input_channels);
            config.input_frame_duration_ms = JsonInt(cJSON_GetObjectItem(input_audio, "frameDurationMs"),
                                                     config.input_frame_duration_ms);

            auto output_audio = cJSON_GetObjectItem(transport, "outputAudio");
            config.output_sample_rate_hz = JsonInt(cJSON_GetObjectItem(output_audio, "sampleRateHz"),
                                                   config.output_sample_rate_hz);
            config.output_channels =
                JsonInt(cJSON_GetObjectItem(output_audio, "channels"), config.output_channels);

            if (config.protocol == kWsProtocol && !config.websocket_url.empty() &&
                !config.connect_token.empty() &&
                config.input_sample_rate_hz == kPracticeAudioSampleRate && config.input_channels == 1 &&
                config.input_frame_duration_ms == kWsInputFrameDurationMs) {
                mode = ConversationTransportMode::kWebSocket;
            } else {
                ESP_LOGW(TAG,
                         "Ignoring unusable WebSocket transport: protocol=%s urlPresent=%s "
                         "tokenPresent=%s input=%dHz/%dch/%dms",
                         config.protocol.c_str(), config.websocket_url.empty() ? "false" : "true",
                         config.connect_token.empty() ? "false" : "true", config.input_sample_rate_hz,
                         config.input_channels, config.input_frame_duration_ms);
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
    ParseConversationTransport(root);
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
        xEventGroupClearBits(ws_event_group_, kWsSessionReadyEvent | kWsConversationStartedEvent |
                                                  kWsTurnCompleteEvent | kWsReconnectRequiredEvent |
                                                  kWsDisconnectedEvent | kWsAssistantAudioEndEvent |
                                                  kWsConversationEndedEvent | kWsUrlAudioFinishedEvent);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        websocket_ = std::move(ws);
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

    if (!websocket_->Connect(config.websocket_url.c_str())) {
        ESP_LOGW(TAG, "Kids English WebSocket connect failed, error=%d", websocket_->GetLastError());
        CloseWebSocket();
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(ws_event_group_, kWsSessionReadyEvent | kWsReconnectRequiredEvent |
                                                                kWsDisconnectedEvent,
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

    EventBits_t bits = xEventGroupWaitBits(ws_event_group_, kWsConversationStartedEvent |
                                                                kWsReconnectRequiredEvent |
                                                                kWsDisconnectedEvent,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(kWsConversationStartTimeoutMs));
    if (!(bits & kWsConversationStartedEvent)) {
        ESP_LOGW(TAG, "Timed out waiting for WebSocket conversation.started");
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
        xEventGroupClearBits(ws_event_group_, kWsConversationEndedEvent | kWsReconnectRequiredEvent |
                                                  kWsDisconnectedEvent);
    }
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "reason", reason == nullptr ? "device_end" : reason);
    if (!SendWsTextFrame(BuildWsJsonEnvelope("conversation.end", payload, conversation_id))) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(ws_event_group_, kWsConversationEndedEvent |
                                                                kWsReconnectRequiredEvent |
                                                                kWsDisconnectedEvent,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(kStartConversationTimeoutMs));
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
    ESP_LOGI(TAG, "WebSocket %s sent=%s conversationId=%s turnId=%s",
             type, sent ? "true" : "false", conversation_id.c_str(), turn_id.c_str());
    return sent;
}

bool KidsEnglishProtocol::SendWsTextFrame(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsWebSocketConnectedLocked()) {
        return false;
    }
    return websocket_->Send(text);
}

bool KidsEnglishProtocol::SendWsAudioFrame(const uint8_t* payload, size_t len,
                                           bool end_of_segment) {
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

    std::string frame;
    frame.reserve(kWsAudioHeaderBytes + len);
    frame.append(kWsAudioMagic, 4);
    frame.push_back(static_cast<char>(1));
    frame.push_back(static_cast<char>(1));
    AppendBe16(frame, end_of_segment ? 1 : 0);
    AppendBe32(frame, NextWsSeq());
    AppendBe32(frame, static_cast<uint32_t>(len));
    frame.append(reinterpret_cast<const char*>(payload), len);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsWebSocketConnectedLocked()) {
        return false;
    }
    return websocket_->Send(frame.data(), frame.size(), true);
}

uint32_t KidsEnglishProtocol::NextWsSeq() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ++ws_next_seq_;
}

void KidsEnglishProtocol::CloseWebSocket() {
    std::unique_ptr<WebSocket> ws;
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
        ESP_LOGW(TAG, "WebSocket turn upload failed; falling back to HTTP v1");
        CloseWebSocket();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            selected_transport_ = ConversationTransportMode::kHttp;
            conversation_id_.clear();
            session_id_.clear();
        }
        if (!StartHttpConversationWithFreshHello("manual")) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            channel_opened_ = true;
            conversation_id = conversation_id_;
            transport = selected_transport_;
        }
        if (transport == ConversationTransportMode::kWebSocket) {
            return UploadPcmAudioWebSocket(pcm, conversation_id);
        }
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
             LogMs(turn_begin_ms), device_id_.c_str(), conversation_id.c_str(), client_turn_id.c_str(),
             (unsigned)(samples_to_send * sizeof(int16_t)), duration_ms, kPracticeAudioSampleRate,
             kPracticeAudioChannels, kWsInputFrameDurationMs);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ResetWsTurnMetricsLocked();
        ws_turn_metrics_.active = true;
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
                                                  kWsDisconnectedEvent | kWsConversationEndedEvent);
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
    while (offset < total_bytes) {
        size_t chunk_bytes = std::min(frame_bytes, total_bytes - offset);
        bool final_frame = offset + chunk_bytes >= total_bytes;
        if (chunk_bytes == frame_bytes) {
            if (!SendWsAudioFrame(pcm_bytes + offset, chunk_bytes, final_frame)) {
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
            if (!SendWsAudioFrame(padded_frame, frame_bytes, final_frame)) {
                ESP_LOGW(TAG,
                         "WS_NATIVE_TURN_UPLOAD_FAILED stage=binary_audio_frame_padded "
                         "conversationId=%s clientTurnId=%s offset=%u totalBytes=%u frames=%u",
                         conversation_id.c_str(), client_turn_id.c_str(), (unsigned)offset,
                         (unsigned)total_bytes, (unsigned)frame_count);
                return false;
            }
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

    cJSON* end_payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(end_payload, "durationMs", duration_ms);
    cJSON_AddStringToObject(end_payload, "reason", "manual_stop");
    if (!SendWsTextFrame(BuildWsJsonEnvelope("turn.audio.end", end_payload, conversation_id,
                                             client_turn_id))) {
        ESP_LOGW(TAG,
                 "WS_NATIVE_TURN_UPLOAD_FAILED stage=turn_audio_end conversationId=%s "
                 "clientTurnId=%s frames=%u totalBytes=%u",
                 conversation_id.c_str(), client_turn_id.c_str(), (unsigned)frame_count,
                 (unsigned)total_bytes);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ws_turn_metrics_.audio_submitted_ms = NowMs();
    }
    ESP_LOGI(TAG,
             "WS_NATIVE_TURN_AUDIO_SUBMITTED timeMs=%u conversationId=%s clientTurnId=%s "
             "durationMs=%d",
             LogMs(NowMs()), conversation_id.c_str(), client_turn_id.c_str(), duration_ms);
    EmitSttMessage("Audio submitted.");

    EventBits_t bits = xEventGroupWaitBits(ws_event_group_,
                                           kWsTurnCompleteEvent | kWsReconnectRequiredEvent |
                                               kWsDisconnectedEvent | kWsConversationEndedEvent,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(kUploadTimeoutMs));
    if (bits & kWsTurnCompleteEvent) {
        // Handled in HandleWsTurnComplete, after assistant audio playback drains.
    } else if (bits & kWsConversationEndedEvent) {
        bool was_open = ClearServerEndedConversation("ws_conversation_ended");
        EmitTtsMessage("stop", nullptr, false);
        EmitEmotion("neutral");
        if (was_open && on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    } else {
        std::lock_guard<std::mutex> lock(mutex_);
        const char* reason = (bits & kWsReconnectRequiredEvent) ? "reconnect_required"
                             : (bits & kWsDisconnectedEvent) ? "disconnected"
                                                             : "timeout";
        ws_turn_metrics_.fallback_requested = true;
        ws_turn_metrics_.fallback_reason = reason;
        LogWsTurnMetricsLocked("ws_turn_failed");
        ESP_LOGW(TAG,
                 "Timed out waiting for WebSocket turn.complete; HTTP fallback allowed reason=%s "
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
            bool was_open = ClearServerEndedConversation("http_409_conversation_ended");
            EmitTtsMessage("stop", nullptr, false);
            EmitEmotion("neutral");
            if (was_open && on_audio_channel_closed_ != nullptr) {
                on_audio_channel_closed_();
            }
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
    if (!result.tts_audio_url.empty() && !result.has_tts_text) {
        ESP_LOGW(TAG,
                 "Conversation response missing ttsText: requestId=%s ttsAudioUrl=%s "
                 "providerFallback=%s providerErrorCode=%s",
                 result.request_id.c_str(), RedactUrlForLog(result.tts_audio_url).c_str(),
                 result.provider_fallback ? "true" : "false",
                 result.provider_error_code.c_str());
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
                 result.provider_fallback ? "true" : "false",
                 result.provider_error_code.c_str());
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

bool KidsEnglishProtocol::HandleConversationResponse(const ConversationResponse& response,
                                                     const char* fallback_text,
                                                     bool wait_for_playback) {
    (void)fallback_text;
    bool should_continue = ShouldContinueConversation(response);
    ESP_LOGI(TAG,
             "Conversation response: conversationId=%s turnId=%s state=%s cue=%s "
             "ttsTextPresent=%s ttsText=%s ttsAudioUrl=%s format=%s continue=%s "
             "requestId=%s asr=%d llm=%d ttsMs=%d total=%d providerFallback=%s "
             "providerErrorCode=%s",
             response.conversation_id.c_str(), response.turn_id.c_str(),
             response.device_state.c_str(), response.screen_cue.c_str(),
             response.has_tts_text ? "true" : "false", response.tts_text.c_str(),
             RedactUrlForLog(response.tts_audio_url).c_str(), response.audio_format.c_str(),
             should_continue ? "true" : "false", response.request_id.c_str(),
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
    const std::string display_text = response.has_tts_text ? response.tts_text : "";
    if (!display_text.empty()) {
        EmitAssistantMessage(display_text);
    }
    bool played = true;
    if (!response.tts_audio_url.empty()) {
        ESP_LOGI(TAG,
                 "Conversation TTS playback: requestId=%s ttsTextPresent=%s ttsAudioUrl=%s "
                 "providerFallback=%s providerErrorCode=%s",
                 response.request_id.c_str(), response.has_tts_text ? "true" : "false",
                 RedactUrlForLog(response.tts_audio_url).c_str(),
                 response.provider_fallback ? "true" : "false",
                 response.provider_error_code.c_str());
        if (!DownloadAndPlayTtsAudio(response.tts_audio_url)) {
            ESP_LOGW(TAG, "Failed to download/play TTS audio; falling back to text-only feedback");
            played = false;
        } else if (wait_for_playback) {
            Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty();
        }
    } else {
        played = false;
    }

    bool was_open = false;
    if (!should_continue) {
        was_open = ClearServerEndedConversation("http_turn_complete");
    }
    EmitTtsMessage("stop", display_text.empty() ? nullptr : display_text.c_str(), should_continue);
    EmitEmotion(should_continue ? "happy" : "neutral");
    if (!should_continue && was_open && on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
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

    Application::GetInstance().GetAudioService().PushPcmToPlaybackQueue(std::move(pcm), sample_rate);
    return true;
}

void KidsEnglishProtocol::ResetWsTurnMetricsLocked() {
    ws_turn_metrics_ = WsTurnMetrics{};
}

void KidsEnglishProtocol::LogWsTurnMetricsLocked(const char* reason) const {
    const auto& m = ws_turn_metrics_;
    if (!m.active && m.turn_begin_ms == 0) {
        return;
    }

    ESP_LOGI(TAG,
             "WS_NATIVE_TURN_METRICS reason=%s conversationId=%s clientTurnId=%s serverTurnId=%s "
             "beginMs=%u audioSubmittedMs=%u assistantAudioStartMs=%u firstBinaryFrameMs=%u "
             "firstPcmQueueMs=%u playbackStartedMs=%u assistantAudioEndMs=%u binaryEndMs=%u "
             "turnCompleteMs=%u playbackFinishedMs=%u beginToSubmittedMs=%d "
             "submittedToAudioStartMs=%d submittedToFirstBinaryMs=%d firstBinaryToQueueMs=%d "
             "firstBinaryToPlaybackStartMs=%d playbackStartedToFinishedMs=%d "
             "submittedToTurnCompleteMs=%d audioBytes=%u audioChunks=%u playbackChunks=%u "
             "playbackSamples=%u queuePeak=%u queueMin=%u underruns=%u "
             "jitterPeakChunks=%u jitterPeakMs=%d jitterRebuffers=%u jitterReleases=%u "
             "fallback=%s fallbackReason=%s",
             reason == nullptr ? "unknown" : reason, m.conversation_id.c_str(),
             m.client_turn_id.c_str(), m.server_turn_id.c_str(), LogMs(m.turn_begin_ms),
             LogMs(m.audio_submitted_ms), LogMs(m.assistant_audio_start_ms),
             LogMs(m.first_binary_frame_ms), LogMs(m.first_pcm_queue_ms),
             LogMs(m.playback_started_ms), LogMs(m.assistant_audio_end_ms),
             LogMs(m.binary_end_ms), LogMs(m.turn_complete_ms), LogMs(m.playback_finished_ms),
             LogDeltaMs(m.turn_begin_ms, m.audio_submitted_ms),
             LogDeltaMs(m.audio_submitted_ms, m.assistant_audio_start_ms),
             LogDeltaMs(m.audio_submitted_ms, m.first_binary_frame_ms),
             LogDeltaMs(m.first_binary_frame_ms, m.first_pcm_queue_ms),
             LogDeltaMs(m.first_binary_frame_ms, m.playback_started_ms),
             LogDeltaMs(m.playback_started_ms, m.playback_finished_ms),
             LogDeltaMs(m.audio_submitted_ms, m.turn_complete_ms), (unsigned)m.audio_bytes,
             (unsigned)m.audio_chunks, (unsigned)m.playback_chunks,
             (unsigned)m.playback_samples, (unsigned)m.queue_peak_depth,
             (unsigned)m.queue_min_depth, (unsigned)m.underruns,
             (unsigned)m.jitter_buffer_peak_chunks, m.jitter_buffer_peak_ms,
             (unsigned)m.jitter_rebuffers, (unsigned)m.jitter_releases,
             m.fallback_requested ? "true" : "false", m.fallback_reason.c_str());
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
        }
    }
    if (direct_push) {
        ESP_LOGD(TAG,
                 "WS_NATIVE_JITTER_DIRECT timeMs=%u conversationId=%s turnId=%s samples=%u",
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
             "durationMs=%d ready=%s",
             LogMs(NowMs()), conversation_id.c_str(), turn_id.c_str(),
             (unsigned)buffered_chunks, buffered_ms, should_flush ? "true" : "false");
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
    auto& audio_service = Application::GetInstance().GetAudioService();
    for (auto& chunk : chunks) {
        audio_service.PushPcmToPlaybackQueue(std::move(chunk), sample_rate);
    }
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
    return ws_output_jitter_chunks_.size() >= kWsOutputJitterMinChunks &&
           WsJitterBufferDurationMsLocked() >= kWsOutputJitterMinMs;
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
    } else if (event_type == "turn.complete") {
        HandleWsTurnComplete(root);
    } else if (event_type == "server.fallback") {
        HandleWsServerFallback(root);
    } else if (event_type == "conversation.ended") {
        bool should_clear_now = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ws_should_continue_listening_ = false;
            should_clear_now = !ws_waiting_turn_complete_ && !ws_output_playback_pending_ &&
                               !ws_url_audio_in_progress_;
        }
        if (should_clear_now) {
            bool was_open = ClearServerEndedConversation("ws_conversation_ended_event");
            EmitTtsMessage("stop", nullptr, false);
            EmitEmotion("neutral");
            if (was_open && on_audio_channel_closed_ != nullptr) {
                on_audio_channel_closed_();
            }
        }
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
                 LogMs(NowMs()), conversation_id.c_str(), turn_id.c_str(), flags, (unsigned)payload_len,
                 1u);
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
        ws_config_.input_channels = JsonInt(cJSON_GetObjectItem(input_audio, "channels"),
                                            ws_config_.input_channels);
        ws_config_.input_frame_duration_ms =
            JsonInt(cJSON_GetObjectItem(input_audio, "frameDurationMs"),
                    ws_config_.input_frame_duration_ms);
    }
    ESP_LOGI(TAG, "Kids English WebSocket session.ready received");
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
    int sample_rate = JsonInt(cJSON_GetObjectItem(payload, "sampleRateHz"), kPracticeAudioSampleRate);
    int channels = JsonInt(cJSON_GetObjectItem(payload, "channels"), kPracticeAudioChannels);

    auto audio_format_object = cJSON_GetObjectItem(payload, "audioFormat");
    if (cJSON_IsObject(audio_format_object)) {
        transport = JsonString(cJSON_GetObjectItem(audio_format_object, "transport"));
        audio_format = JsonString(cJSON_GetObjectItem(audio_format_object, "codec"));
        url = JsonString(cJSON_GetObjectItem(audio_format_object, "url"));
        sample_rate = JsonInt(cJSON_GetObjectItem(audio_format_object, "sampleRateHz"), sample_rate);
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
             LogMs(now_ms), conversation_id.c_str(), turn_id.c_str(), turn_id_present ? "true" : "false",
             audio_format.c_str(), transport.c_str(), redacted_url.c_str(), sample_rate, channels);
    if (is_native_pcm_stream) {
        ESP_LOGI(TAG,
                 "WS_NATIVE_JITTER_RESET timeMs=%u conversationId=%s turnId=%s "
                 "minChunks=%u minMs=%d sampleRateHz=%d",
                 LogMs(now_ms), conversation_id.c_str(), turn_id.c_str(),
                 (unsigned)kWsOutputJitterMinChunks, kWsOutputJitterMinMs, sample_rate);
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
    bool should_emit_stop = false;
    bool should_continue = true;
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
        should_play =
            ws_output_audio_active_ && !ws_output_audio_finalized_ &&
            ((ws_output_audio_transport_ == "binary_chunk" &&
              (!ws_output_audio_buffer_.empty() || !ws_output_jitter_chunks_.empty())) ||
             ws_output_audio_transport_ == "url");
        should_emit_stop = !ws_waiting_turn_complete_ && ws_tts_playback_started_;
        should_continue = ws_should_continue_listening_;
        was_initial_audio = ws_waiting_initial_audio_;
        ws_output_audio_end_received_ = true;
        if (ws_turn_metrics_.active && conversation_id == ws_turn_metrics_.conversation_id) {
            ws_turn_metrics_.assistant_audio_end_ms = now_ms;
        }
    }
    ESP_LOGI(TAG,
             "WS_NATIVE_ASSISTANT_AUDIO_END timeMs=%u conversationId=%s turnId=%s",
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
    if (should_emit_stop) {
        Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty();
        bool self_test = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            self_test = self_test_in_progress_;
        }
        EmitTtsMessage("stop", nullptr, self_test ? false : should_continue);
        EmitEmotion(should_continue ? "happy" : "neutral");
    }
    if (was_initial_audio) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ws_waiting_initial_audio_ = false;
        }
        xEventGroupSetBits(ws_event_group_, kWsAssistantAudioEndEvent);
    }
    TrySendWsPlaybackFinished("assistant_audio_end");
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
    char* diagnostics_json = cJSON_IsObject(diagnostics) ? cJSON_PrintUnformatted(diagnostics) : nullptr;
    std::string diagnostics_text = diagnostics_json == nullptr ? "{}" : diagnostics_json;
    if (diagnostics_json != nullptr) {
        cJSON_free(diagnostics_json);
    }
    int64_t now_ms = NowMs();
    bool was_open = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (conversation_id.empty()) {
            conversation_id = ws_turn_metrics_.conversation_id;
        }
        ws_waiting_turn_complete_ = false;
        ws_should_continue_listening_ = should_continue;
        ws_tts_playback_started_ = false;
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
        was_open = channel_opened_;
    }
    ESP_LOGI(TAG,
             "WS_NATIVE_TURN_COMPLETE timeMs=%u conversationId=%s turnId=%s "
             "shouldContinueListening=%s diagnostics=%s",
             LogMs(now_ms), conversation_id.c_str(), turn_id.c_str(), should_continue ? "true" : "false",
             diagnostics_text.c_str());
    if (!WaitForWsUrlAudioIfNeeded(kUploadTimeoutMs)) {
        ESP_LOGW(TAG,
                 "Timed out waiting for WebSocket URL audio before turn.complete finalization");
    }
    Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty();
    bool self_test = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        self_test = self_test_in_progress_;
    }
    EmitTtsMessage("stop", nullptr, self_test ? false : should_continue);
    EmitEmotion(should_continue ? "happy" : "neutral");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ws_output_playback_queue_drained_ = true;
    }
    TrySendWsPlaybackFinished("turn_complete");
    if (!should_continue) {
        ClearServerEndedConversation("ws_turn_complete");
        if (was_open && on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    }
    xEventGroupSetBits(ws_event_group_, kWsTurnCompleteEvent);
}

void KidsEnglishProtocol::HandleWsServerFallback(const cJSON* root) {
    auto payload = cJSON_GetObjectItem(root, "payload");
    std::string reason = JsonString(cJSON_GetObjectItem(payload, "reason"));
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

void KidsEnglishProtocol::TrySendWsPlaybackFinished(const char* reason) {
    std::string conversation_id;
    std::string turn_id;
    bool should_send = false;
    int64_t now_ms = NowMs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ws_output_playback_pending_ || !ws_output_audio_end_received_ ||
            !ws_output_playback_queue_drained_ ||
            ws_output_playback_finished_sent_) {
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
}

void KidsEnglishProtocol::OnPcmPlaybackQueued(size_t samples, size_t queue_depth) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ws_output_playback_pending_) {
        return;
    }
    if (ws_turn_metrics_.active && ws_output_audio_conversation_id_ == ws_turn_metrics_.conversation_id &&
        ws_turn_metrics_.first_pcm_queue_ms == 0) {
        ws_turn_metrics_.first_pcm_queue_ms = NowMs();
        ESP_LOGI(TAG,
                 "WS_NATIVE_FIRST_PCM_QUEUE timeMs=%u conversationId=%s turnId=%s "
                 "samples=%u queueDepth=%u",
                 LogMs(ws_turn_metrics_.first_pcm_queue_ms), ws_output_audio_conversation_id_.c_str(),
                 ws_output_audio_turn_id_.c_str(), (unsigned)samples, (unsigned)queue_depth);
    }
    if (ws_turn_metrics_.active && ws_output_audio_conversation_id_ == ws_turn_metrics_.conversation_id) {
        ws_output_playback_queue_drained_ = false;
        ws_turn_metrics_.queue_peak_depth = std::max(ws_turn_metrics_.queue_peak_depth, queue_depth);
        if (ws_turn_metrics_.queue_min_depth == 0 || queue_depth < ws_turn_metrics_.queue_min_depth) {
            ws_turn_metrics_.queue_min_depth = queue_depth;
        }
    }
    ESP_LOGD(TAG,
             "WS_NATIVE_PCM_QUEUE_PUSH timeMs=%u conversationId=%s turnId=%s samples=%u "
             "queueDepth=%u",
             LogMs(NowMs()), ws_output_audio_conversation_id_.c_str(), ws_output_audio_turn_id_.c_str(),
             (unsigned)samples, (unsigned)queue_depth);
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
                    if (!ws_output_jitter_rebuffering_) {
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

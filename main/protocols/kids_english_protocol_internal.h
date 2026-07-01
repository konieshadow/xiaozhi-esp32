#ifndef KIDS_ENGLISH_PROTOCOL_INTERNAL_H
#define KIDS_ENGLISH_PROTOCOL_INTERNAL_H

#include "kids_english_protocol.h"

#include "application.h"
#include "assets/lang_config.h"
#include "audio_service.h"
#include "board.h"
#include "settings.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/task.h>
#include <http.h>
#include <web_socket.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifndef TAG
#define TAG "KidsEnglish"
#endif

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

namespace kids_english_protocol_internal {

static constexpr int kHealthTimeoutMs = 3000;
static constexpr int kDeviceHelloTimeoutMs = 5000;
static constexpr int kStartConversationTimeoutMs = 5000;
static constexpr int kUploadTimeoutMs = 90000;
static constexpr int kStandaloneSpeechTimeoutMs = 20000;
static constexpr int kTtsDownloadTimeoutMs = 10000;
static constexpr int kWsSessionReadyTimeoutMs = 10000;
static constexpr int kWsConversationStartTimeoutMs = 30000;
static constexpr int kWsSendTimeoutMs = 2000;
static constexpr int kWsSlowSendLogMs = 200;
static constexpr int kWsHeartbeatMissesBeforeFallback = 3;
static constexpr int kSelfTestPlaybackDrainTimeoutMs = 30000;
static constexpr size_t kMaxWelcomeAudioCandidates = 3;
static constexpr size_t kMaxWelcomeAudioCacheEntries = 3;
static constexpr char kMultipartBoundary[] = "----xiaozhi-kids-english-boundary";
static constexpr char kSelfTestSpeechText[] = "I like apples.";
static constexpr const char* kSelfTestConversationTexts[] = {
    "I like apples.",        "My apple is red.",       "I have a small dog.",
    "The dog can run fast.", "I want to read a book.",
};
static constexpr char kWsProtocol[] = "xiaozhi.conversation.ws.v1";
static constexpr char kWsAudioMagic[] = "XZWS";
static constexpr int kPracticeAudioSampleRate = 16000;
static constexpr int kPracticeAudioChannels = 1;
static constexpr int kPracticeAudioBitsPerSample = 16;
static constexpr int kWsInputFrameDurationMs = 20;
static constexpr int kWsInputFrameSamples =
    kPracticeAudioSampleRate * kWsInputFrameDurationMs / 1000;
static constexpr int kWsInputFramePaceMs = 5;
static constexpr size_t kWsOutputJitterMinChunks = 4;
static constexpr int kWsOutputJitterMinMs = 1200;
static constexpr size_t kWsOutputRebufferMinChunks = 4;
static constexpr int kWsOutputRebufferMinMs = 1200;
static constexpr size_t kWsAudioHeaderBytes = 16;
static constexpr int kMaxPracticeAudioDurationSeconds = 10;
static constexpr size_t kMaxPracticeAudioBytes = 5 * 1024 * 1024;
static constexpr size_t kWavHeaderBytes = 44;
static constexpr int64_t kUnixTimeReasonableMs = 1600000000000LL;
static constexpr char kKidsEnglishSettingsNamespace[] = "kids_english";
static constexpr char kKidsEnglishEnvironmentKey[] = "environment";
static constexpr char kKidsEnglishEnvironmentProduction[] = "production";
static constexpr char kKidsEnglishEnvironmentDevelopment[] = "development";
static constexpr EventBits_t kWsSessionReadyEvent = BIT0;
static constexpr EventBits_t kWsConversationStartedEvent = BIT1;
static constexpr EventBits_t kWsTurnCompleteEvent = BIT2;
static constexpr EventBits_t kWsReconnectRequiredEvent = BIT3;
static constexpr EventBits_t kWsDisconnectedEvent = BIT4;
static constexpr EventBits_t kWsAssistantAudioEndEvent = BIT5;
static constexpr EventBits_t kWsConversationEndedEvent = BIT6;
static constexpr EventBits_t kWsUrlAudioFinishedEvent = BIT7;
static constexpr EventBits_t kWsPlaybackFinishedEvent = BIT8;

struct WsUrlAudioTaskArgs {
    KidsEnglishProtocol* self = nullptr;
    std::string url;
};

std::string JsonString(const cJSON* item);
int JsonInt(const cJSON* item, int fallback = -1);
bool JsonBool(const cJSON* item, bool fallback = false);
std::string JsonCompactString(const cJSON* item);
uint32_t LogMs(int64_t ms);
int LogDeltaMs(int64_t from, int64_t to);
std::string JsonEscape(const std::string& text);
std::string Sha256Hex(const std::string& data);
std::string HmacSha256Hex(const std::string& key, const std::string& data);
bool ParseIsoUtcMillis(const std::string& iso, int64_t& unix_ms);
void AppendLe16(std::string& out, uint16_t value);
void AppendLe32(std::string& out, uint32_t value);
void AppendBe16(std::string& out, uint16_t value);
void AppendBe32(std::string& out, uint32_t value);
uint16_t ReadLe16(const char* data);
uint32_t ReadLe32(const char* data);
uint16_t ReadBe16(const char* data);
uint32_t ReadBe32(const char* data);
bool ResamplePcm16Mono(std::vector<int16_t>& pcm, int source_sample_rate, int target_sample_rate);

}  // namespace kids_english_protocol_internal

#endif  // KIDS_ENGLISH_PROTOCOL_INTERNAL_H

#ifndef KIDS_ENGLISH_PROTOCOL_H
#define KIDS_ENGLISH_PROTOCOL_H

#include "protocol.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <cstdint>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Http;
class WebSocket;

class KidsEnglishProtocol : public Protocol {
public:
    KidsEnglishProtocol();
    ~KidsEnglishProtocol() override;

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool SendPcmAudio(std::vector<int16_t>&& pcm);
    bool GenerateSimulatedRecordingPcm(const std::string& text, std::vector<int16_t>& pcm);
    bool RunSelfTest();
    void SetNextConversationTrigger(const std::string& trigger);
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;

protected:
    bool SendText(const std::string& text) override;

private:
    struct ConversationResponse {
        std::string conversation_id;
        std::string turn_id;
        std::string device_state;
        std::string screen_cue;
        std::string audio_format;
        std::string tts_audio_url;
        std::string tts_text;
        std::string request_id;
        bool should_continue_listening = true;
        bool has_tts_text = false;
        int asr_duration_ms = -1;
        int llm_duration_ms = -1;
        int tts_duration_ms = -1;
        int total_duration_ms = -1;
        bool provider_fallback = false;
        std::string provider_error_code;
    };

    struct StandaloneTtsResponse {
        std::string tts_audio_url;
        std::string tts_text;
        std::string audio_format;
        std::string request_id;
        bool has_tts_text = false;
        int tts_duration_ms = -1;
        int total_duration_ms = -1;
        bool provider_fallback = false;
        std::string provider_error_code;
    };

    struct StandaloneAsrResponse {
        std::string transcript;
        std::string audio_format;
        std::string request_id;
        int asr_duration_ms = -1;
        int total_duration_ms = -1;
    };

    enum class UploadResult {
        kSuccess,
        kFailed,
        kConversationEnded,
    };

    enum class ConversationTransportMode {
        kHttp,
        kWebSocket,
    };

    struct WebSocketTransportConfig {
        bool available = false;
        bool fallback_http = true;
        std::string protocol;
        std::string websocket_url;
        std::string connect_token;
        int ping_interval_ms = 15000;
        int idle_timeout_ms = 300000;
        int max_frame_bytes = 4096;
        int input_sample_rate_hz = 16000;
        int input_channels = 1;
        int input_frame_duration_ms = 20;
        int output_sample_rate_hz = 16000;
        int output_channels = 1;
    };

    std::string base_url_;
    std::string device_id_;
    std::string device_secret_;
    std::string conversation_id_;
    std::string last_turn_id_;
    std::string next_conversation_trigger_ = "manual";
    int64_t server_time_offset_ms_ = 0;
    bool channel_opened_ = false;
    bool upload_in_progress_ = false;
    bool self_test_in_progress_ = false;
    ConversationTransportMode selected_transport_ = ConversationTransportMode::kHttp;
    WebSocketTransportConfig ws_config_;
    std::unique_ptr<WebSocket> websocket_;
    EventGroupHandle_t ws_event_group_ = nullptr;
    uint32_t ws_next_seq_ = 0;
    bool ws_closing_ = false;
    bool ws_waiting_turn_complete_ = false;
    bool ws_should_continue_listening_ = true;
    bool ws_waiting_initial_audio_ = false;
    bool ws_output_audio_active_ = false;
    bool ws_output_audio_finalized_ = false;
    std::string ws_output_audio_transport_;
    std::string ws_output_audio_format_;
    std::string ws_output_audio_url_;
    std::string ws_output_audio_buffer_;
    std::vector<std::unique_ptr<AudioStreamPacket>> pending_audio_;
    std::vector<int16_t> pending_pcm_;
    mutable std::mutex mutex_;

    std::string BuildUrl(const char* path) const;
    bool RequestJson(const std::string& method, const std::string& path, const std::string& body,
                     cJSON** response_root, int timeout_ms, bool authenticated = true);
    void AddDeviceAuthHeaders(Http* http, const std::string& method, const std::string& path,
                              const std::string& body_sha256);
    std::string BuildDeviceHelloBody() const;
    std::string BuildStartConversationBody(const char* trigger) const;
    std::string BuildEndConversationBody(const char* reason) const;
    std::string BuildStandaloneTtsBody(const std::string& text) const;
    std::string BuildWsJsonEnvelope(const std::string& type, cJSON* payload,
                                    const std::string& conversation_id = "",
                                    const std::string& turn_id = "",
                                    bool include_client_ts = false);
    std::string BuildStandaloneAsrMultipartBody(const std::string& boundary, const std::string& wav,
                                                const std::string& prompt_text) const;
    std::string BuildMultipartAudioBody(const std::string& boundary, const std::string& wav,
                                        const std::string& client_turn_id,
                                        const std::string& recorded_at, int duration_ms) const;
    std::string GenerateClientTurnId() const;
    std::string CurrentUnixMillisString() const;
    std::string CurrentIsoTimestamp() const;
    void UpdateServerTimeOffset(const cJSON* server_time);
    void LogDeviceHelloResponse(const cJSON* root) const;
    void ParseConversationTransport(const cJSON* root);
    bool CheckHealth();
    bool DeviceHello();
    bool StartConversation(const char* trigger = "manual");
    bool StartHttpConversationWithFreshHello(const char* trigger);
    bool OpenWebSocketConversation(const char* trigger);
    bool ConnectWebSocket();
    bool SendWsSessionOpen();
    bool SendWsConversationStart(const char* trigger);
    bool SendWsConversationEnd(const std::string& conversation_id, const char* reason);
    bool SendWsTextFrame(const std::string& text);
    bool SendWsAudioFrame(const uint8_t* payload, size_t len, bool end_of_segment);
    uint32_t NextWsSeq();
    void CloseWebSocket();
    bool IsWebSocketConnectedLocked() const;
    bool IsWebSocketHeartbeatTimedOut() const;
    bool UploadPendingAudio();
    bool UploadPcmAudioWebSocket(const std::vector<int16_t>& pcm, const std::string& conversation_id);
    UploadResult UploadPcmAudio(std::vector<int16_t>&& pcm, const std::string& conversation_id);
    bool RequestStandaloneTts(const std::string& text, StandaloneTtsResponse& response);
    bool UploadStandaloneAsrAudio(const std::string& wav, const std::string& prompt_text,
                                  StandaloneAsrResponse& response);
    bool EndConversation(const std::string& conversation_id);
    bool DownloadAndPlayTtsAudio(const std::string& url);
    bool ParseConversationResponse(const cJSON* root, ConversationResponse& response);
    bool ParseStandaloneTtsResponse(const cJSON* root, StandaloneTtsResponse& response);
    bool ParseStandaloneAsrResponse(const cJSON* root, StandaloneAsrResponse& response);
    std::string CreateWavFile(const std::vector<int16_t>& pcm, int sample_rate) const;
    bool ParsePcm16MonoBytes(const std::string& bytes, std::vector<int16_t>& pcm) const;
    bool ParseWavPcm16Mono(const std::string& wav, std::vector<int16_t>& pcm, int& sample_rate) const;
    bool DownloadWavAudio(const std::string& url, std::vector<int16_t>& pcm, int& sample_rate,
                          std::string* content_type = nullptr);
    bool ReadHttpBody(Http* http, std::string& body);
    bool HandleConversationResponse(const ConversationResponse& response, const char* fallback_text,
                                    bool wait_for_playback = false);
    bool HandleWsAudioPlayback();
    bool HandleWsAudioUrl(const std::string& url);
    bool HandleWsAudioBuffer();
    std::string ResolveAudioUrl(const std::string& url) const;
    std::string RedactUrlForLog(const std::string& url) const;
    void HandleWsTextFrame(const char* data, size_t len);
    void HandleWsBinaryFrame(const char* data, size_t len);
    void HandleWsSessionReady(const cJSON* root);
    void HandleWsConversationStarted(const cJSON* root);
    void HandleWsAssistantAudioStart(const cJSON* root);
    void HandleWsAssistantAudioEnd(const cJSON* root);
    void HandleWsTurnComplete(const cJSON* root);
    void HandleWsError(const cJSON* root);
    void MarkWebSocketFallback();
    bool HandleStandaloneTtsResponse(const StandaloneTtsResponse& response,
                                     bool wait_for_playback = false);
    bool IsConversationEndedError(const cJSON* root) const;
    void EmitTtsMessage(const char* state, const char* text = nullptr,
                        bool continue_listening = true);
    void EmitSttMessage(const std::string& text);
    void EmitAssistantMessage(const std::string& text);
    void EmitEmotion(const char* emotion);
    void EmitConversationStarted();
    void DispatchJson(cJSON* root);
    std::string GetErrorMessage(const cJSON* root) const;
    static std::string TrimTrailingSlash(std::string url);
};

#endif  // KIDS_ENGLISH_PROTOCOL_H

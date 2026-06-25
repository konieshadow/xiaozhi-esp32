#ifndef KIDS_ENGLISH_PROTOCOL_H
#define KIDS_ENGLISH_PROTOCOL_H

#include "protocol.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Http;

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

    std::string base_url_;
    std::string device_id_;
    std::string device_secret_;
    std::string conversation_id_;
    std::string last_turn_id_;
    std::string next_conversation_trigger_ = "manual";
    int64_t server_time_offset_ms_ = 0;
    bool channel_opened_ = false;
    bool upload_in_progress_ = false;
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
    bool CheckHealth();
    bool DeviceHello();
    bool StartConversation(const char* trigger = "manual");
    bool UploadPendingAudio();
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
    bool ParseWavPcm16Mono(const std::string& wav, std::vector<int16_t>& pcm, int& sample_rate) const;
    bool DownloadWavAudio(const std::string& url, std::vector<int16_t>& pcm, int& sample_rate,
                          std::string* content_type = nullptr);
    bool ReadHttpBody(Http* http, std::string& body);
    bool HandleConversationResponse(const ConversationResponse& response, const char* fallback_text,
                                    bool wait_for_playback = false);
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

#ifndef KIDS_ENGLISH_PROTOCOL_H
#define KIDS_ENGLISH_PROTOCOL_H

#include "protocol.h"

#include <memory>
#include <mutex>
#include <string>
#include <cstdint>
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
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;

protected:
    bool SendText(const std::string& text) override;

private:
    struct PracticeResult {
        std::string recognized_text;
        int score = 0;
        std::string feedback;
        std::string correction;
        std::string next_prompt_id;
        std::string next_prompt_text;
        std::string tts_text;
        std::string tts_audio_url;
        std::string request_id;
        std::string provider;
        std::string mode;
        std::string audio_mime_type;
        int asr_duration_ms = -1;
        int tts_duration_ms = -1;
        int total_duration_ms = -1;
        int audio_bytes = -1;
        double audio_duration_seconds = -1.0;
    };

    std::string base_url_;
    std::string prompt_id_;
    std::string prompt_text_;
    std::string prompt_level_;
    bool channel_opened_ = false;
    bool upload_in_progress_ = false;
    std::vector<std::unique_ptr<AudioStreamPacket>> pending_audio_;
    std::vector<int16_t> pending_pcm_;
    mutable std::mutex mutex_;

    std::string BuildUrl(const char* path) const;
    bool RequestJson(const std::string& method, const std::string& path, const std::string& body,
                     cJSON** response_root, int timeout_ms);
    bool CheckHealth();
    bool StartSession();
    bool UploadPendingAudio();
    bool DownloadAndPlayTtsAudio(const std::string& url);
    bool ParseStartSessionResponse(const cJSON* root);
    bool ParsePracticeResult(const cJSON* root, PracticeResult& result);
    std::string CreateWavFile(const std::vector<int16_t>& pcm, int sample_rate) const;
    bool ParseWavPcm16Mono(const std::string& wav, std::vector<int16_t>& pcm, int& sample_rate) const;
    bool ReadHttpBody(Http* http, std::string& body);
    bool WriteMultipartField(Http* http, const std::string& boundary, const std::string& name,
                             const std::string& value);
    bool WriteMultipartAudio(Http* http, const std::string& boundary, const std::string& wav);
    bool WriteString(Http* http, const std::string& data);
    bool FinishChunkedBody(Http* http);
    void EmitPromptMessage();
    void EmitPracticeResult(const PracticeResult& result);
    void EmitTtsMessage(const char* state, const char* text = nullptr);
    void EmitSttMessage(const std::string& text);
    void EmitAssistantMessage(const std::string& text);
    void EmitEmotion(const char* emotion);
    void DispatchJson(cJSON* root);
    std::string GetErrorMessage(const cJSON* root) const;
    static std::string TrimTrailingSlash(std::string url);
};

#endif  // KIDS_ENGLISH_PROTOCOL_H

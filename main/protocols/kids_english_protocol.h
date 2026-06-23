#ifndef KIDS_ENGLISH_PROTOCOL_H
#define KIDS_ENGLISH_PROTOCOL_H

#include "protocol.h"

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
    };

    std::string base_url_;
    std::string prompt_id_;
    std::string prompt_text_;
    std::string prompt_level_;
    bool channel_opened_ = false;
    bool upload_in_progress_ = false;
    std::vector<std::unique_ptr<AudioStreamPacket>> pending_audio_;
    mutable std::mutex mutex_;

    std::string BuildUrl(const char* path) const;
    bool RequestJson(const std::string& method, const std::string& path, const std::string& body,
                     cJSON** response_root, int timeout_ms);
    bool CheckHealth();
    bool StartSession();
    bool UploadPendingAudio();
    bool ParseStartSessionResponse(const cJSON* root);
    bool ParsePracticeResult(const cJSON* root, PracticeResult& result);
    bool WriteMultipartField(Http* http, const std::string& boundary, const std::string& name,
                             const std::string& value);
    bool WriteMultipartAudio(Http* http, const std::string& boundary,
                             const std::vector<std::unique_ptr<AudioStreamPacket>>& packets);
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

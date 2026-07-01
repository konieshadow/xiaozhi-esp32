#include "kids_english_protocol_internal.h"

using namespace kids_english_protocol_internal;

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

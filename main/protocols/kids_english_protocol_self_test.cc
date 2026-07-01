#include "kids_english_protocol_internal.h"

using namespace kids_english_protocol_internal;

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

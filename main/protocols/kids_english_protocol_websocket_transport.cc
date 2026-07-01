#include "kids_english_protocol_internal.h"

using namespace kids_english_protocol_internal;

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

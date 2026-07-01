#include "kids_english_protocol_internal.h"

using namespace kids_english_protocol_internal;

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
        m.turn_audio_end_requested ? "true" : "false", m.turn_audio_end_sent ? "true" : "false",
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

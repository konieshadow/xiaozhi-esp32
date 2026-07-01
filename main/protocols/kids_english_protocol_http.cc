#include "kids_english_protocol_internal.h"

using namespace kids_english_protocol_internal;

std::string KidsEnglishProtocol::BuildUrl(const char* path) const { return base_url_ + path; }

void KidsEnglishProtocol::AddDeviceAuthHeaders(Http* http, const std::string& method,
                                               const std::string& path,
                                               const std::string& body_sha256) {
    if (device_id_.empty() || device_secret_.empty()) {
        ESP_LOGW(TAG, "Kids English device ID or secret is empty");
    }

    std::string timestamp = CurrentUnixMillisString();
    uint32_t random_a = esp_random();
    uint32_t random_b = esp_random();
    std::string nonce = Board::GetInstance().GetUuid() + "-" +
                        std::to_string(esp_timer_get_time()) + "-" + std::to_string(random_a) +
                        std::to_string(random_b);
    std::string payload =
        method + "\n" + path + "\n" + timestamp + "\n" + nonce + "\n" + body_sha256;
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

std::string KidsEnglishProtocol::BuildStandaloneAsrMultipartBody(
    const std::string& boundary, const std::string& wav, const std::string& prompt_text) const {
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
    body.reserve(boundary.size() * 6 + wav.size() + client_turn_id.size() + recorded_at.size() +
                 256);
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

int64_t KidsEnglishProtocol::NowMs() const { return esp_timer_get_time() / 1000; }

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
             "conversationTransport.selected=%s reason=%s protocol=%s",
             device_id_.c_str(), JsonString(device_state).c_str(),
             JsonString(cJSON_GetObjectItem(settings, "languageLevel")).c_str(),
             JsonString(cJSON_GetObjectItem(settings, "voice")).c_str(),
             JsonInt(cJSON_GetObjectItem(settings, "dailyPracticeMinutes"), 0),
             cJSON_IsTrue(audio_upload) ? "true" : "false",
             cJSON_IsTrue(tts_url) ? "true" : "false", cJSON_IsTrue(streaming) ? "true" : "false",
             JsonString(transport_selected).c_str(), JsonString(transport_reason).c_str(),
             JsonString(transport_protocol).c_str());
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

bool KidsEnglishProtocol::CheckHealth(bool set_error) {
    cJSON* root = nullptr;
    bool ok = RequestJson("GET", "/health", "", &root, kHealthTimeoutMs, false);
    if (root != nullptr) {
        auto data = cJSON_GetObjectItem(root, "data");
        UpdateServerTimeOffset(cJSON_GetObjectItem(data, "timestamp"));
        cJSON_Delete(root);
    }
    if (!ok) {
        if (set_error) {
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        }
    }
    return ok;
}

bool KidsEnglishProtocol::DeviceHello(bool set_error, bool schedule_welcome_prefetch,
                                      bool update_transport) {
    cJSON* root = nullptr;
    std::string body = BuildDeviceHelloBody();
    if (!RequestJson("POST", "/api/device/hello", body, &root, kDeviceHelloTimeoutMs)) {
        if (set_error) {
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        }
        return false;
    }

    auto data = cJSON_GetObjectItem(root, "data");
    UpdateServerTimeOffset(cJSON_GetObjectItem(data, "serverTime"));
    LogDeviceHelloResponse(root);
    auto settings = cJSON_GetObjectItem(data, "settings");
    std::string voice = JsonString(cJSON_GetObjectItem(settings, "voice"));
    StoreWelcomeAudioCandidates(ParseWelcomeAudioCandidates(root), voice,
                                schedule_welcome_prefetch);
    if (update_transport) {
        ParseConversationTransport(root);
    }
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
    if (!RequestJson("POST", "/api/conversations/start", body, &root,
                     kStartConversationTimeoutMs)) {
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
    return HandleConversationResponse(response, "Let's speak English.", true, true, true);
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
    http->SetHeader("Content-Type",
                    std::string("multipart/form-data; boundary=") + kMultipartBoundary);

    std::string wav = CreateWavFile(pcm, kPracticeAudioSampleRate);
    std::string client_turn_id = GenerateClientTurnId();
    std::string recorded_at = CurrentIsoTimestamp();
    std::string multipart_body =
        BuildMultipartAudioBody(kMultipartBoundary, wav, client_turn_id, recorded_at, duration_ms);
    std::string path = "/api/conversations/" + conversation_id + "/turns/audio";
    AddDeviceAuthHeaders(http.get(), "POST", path, Sha256Hex(""));
    http->SetContent(std::move(multipart_body));
    auto url = BuildUrl(path.c_str());
    ESP_LOGI(
        TAG,
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
                     "Conversation already ended after upload attempt; local conversation state "
                     "cleared");
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
    std::string multipart_body =
        BuildStandaloneAsrMultipartBody(kMultipartBoundary, wav, prompt_text);

    http->SetTimeout(kStandaloneSpeechTimeoutMs);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Content-Type",
                    std::string("multipart/form-data; boundary=") + kMultipartBoundary);
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
             response.request_id.c_str(), response.transcript.c_str(),
             response.audio_format.c_str(), response.asr_duration_ms, response.total_duration_ms);
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

bool KidsEnglishProtocol::ParseConversationResponse(const cJSON* root,
                                                    ConversationResponse& result) {
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
    auto welcome_audio_cached = cJSON_GetObjectItem(diagnostics, "welcomeAudioCached");
    auto welcome_audio_cache_key = cJSON_GetObjectItem(diagnostics, "welcomeAudioCacheKey");

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
    result.welcome_audio_cached = JsonBool(welcome_audio_cached);
    result.welcome_audio_cache_key = JsonString(welcome_audio_cache_key);
    if (!result.tts_audio_url.empty() && !result.has_tts_text) {
        ESP_LOGW(TAG,
                 "Conversation response missing ttsText: requestId=%s ttsAudioUrl=%s "
                 "providerFallback=%s providerErrorCode=%s",
                 result.request_id.c_str(), RedactUrlForLog(result.tts_audio_url).c_str(),
                 result.provider_fallback ? "true" : "false", result.provider_error_code.c_str());
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
                 result.provider_fallback ? "true" : "false", result.provider_error_code.c_str());
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

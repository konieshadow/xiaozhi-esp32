#include "kids_english_protocol_internal.h"

using namespace kids_english_protocol_internal;

KidsEnglishProtocol::KidsEnglishProtocol()
    : base_url_(GetConfiguredBaseUrl()),
      device_id_(CONFIG_KIDS_ENGLISH_DEVICE_ID),
      device_secret_(CONFIG_KIDS_ENGLISH_DEVICE_SECRET) {
    server_sample_rate_ = kPracticeAudioSampleRate;
    server_frame_duration_ = OPUS_FRAME_DURATION_MS;
    ws_event_group_ = xEventGroupCreate();
    ESP_LOGI(TAG, "Kids English environment: %s baseUrl=%s", GetConfiguredEnvironmentName().c_str(),
             base_url_.c_str());
}

KidsEnglishProtocol::~KidsEnglishProtocol() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        welcome_audio_destroying_ = true;
        welcome_audio_prefetch_pending_ = false;
    }
    while (true) {
        bool prefetch_in_progress = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            prefetch_in_progress = welcome_audio_prefetch_in_progress_;
        }
        if (!prefetch_in_progress) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    CloseAudioChannel(false);
    if (ws_event_group_ != nullptr) {
        vEventGroupDelete(ws_event_group_);
        ws_event_group_ = nullptr;
    }
}

KidsEnglishProtocol::Environment KidsEnglishProtocol::GetConfiguredEnvironment() {
    Settings settings(kKidsEnglishSettingsNamespace, false);
    auto environment =
        settings.GetString(kKidsEnglishEnvironmentKey, kKidsEnglishEnvironmentProduction);
    if (environment == kKidsEnglishEnvironmentDevelopment) {
        return Environment::kDevelopment;
    }
    return Environment::kProduction;
}

std::string KidsEnglishProtocol::GetConfiguredEnvironmentName() {
    return GetEnvironmentName(GetConfiguredEnvironment());
}

std::string KidsEnglishProtocol::GetConfiguredBaseUrl() {
    return GetBaseUrl(GetConfiguredEnvironment());
}

std::string KidsEnglishProtocol::GetEnvironmentName(Environment environment) {
    switch (environment) {
        case Environment::kDevelopment:
            return kKidsEnglishEnvironmentDevelopment;
        case Environment::kProduction:
        default:
            return kKidsEnglishEnvironmentProduction;
    }
}

std::string KidsEnglishProtocol::GetBaseUrl(Environment environment) {
    switch (environment) {
        case Environment::kDevelopment:
            return TrimTrailingSlash(CONFIG_KIDS_ENGLISH_DEVELOPMENT_SERVER_URL);
        case Environment::kProduction:
        default:
            return TrimTrailingSlash(CONFIG_KIDS_ENGLISH_SERVER_URL);
    }
}

void KidsEnglishProtocol::SetConfiguredEnvironment(Environment environment) {
    Settings settings(kKidsEnglishSettingsNamespace, true);
    settings.SetString(kKidsEnglishEnvironmentKey, GetEnvironmentName(environment));
}

bool KidsEnglishProtocol::Start() {
    if (on_connected_ != nullptr) {
        on_connected_();
    }
    PrefetchWelcomeAudioAsync(true);
    return true;
}

bool KidsEnglishProtocol::OpenAudioChannel() {
    error_occurred_ = false;
    if (base_url_.empty()) {
        SetError("Kids English server URL is empty");
        return false;
    }

    if (!CheckHealth()) {
        return false;
    }
    if (!DeviceHello()) {
        return false;
    }
    std::string trigger;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        trigger = next_conversation_trigger_;
        next_conversation_trigger_ = "manual";
    }

    bool opened = false;
    ConversationTransportMode selected_transport;
    WebSocketTransportConfig ws_config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selected_transport = selected_transport_;
        ws_config = ws_config_;
    }
    if (selected_transport == ConversationTransportMode::kWebSocket) {
        opened = OpenWebSocketConversation(trigger.c_str());
        if (!opened) {
            ESP_LOGW(TAG, "Kids English WebSocket conversation unavailable");
            CloseWebSocket();
        }
    } else {
        opened = StartConversation(trigger.c_str());
    }
    if (!opened) {
        return false;
    }
    EmitConversationStarted();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_audio_.clear();
        pending_pcm_.clear();
        channel_opened_ = true;
    }
    FlushPendingTtsStop();

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

void KidsEnglishProtocol::CloseAudioChannel(bool send_goodbye) {
    std::string conversation_to_end;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!channel_opened_) {
            pending_audio_.clear();
            pending_pcm_.clear();
            return;
        }
        channel_opened_ = false;
        pending_audio_.clear();
        pending_pcm_.clear();
        conversation_to_end = conversation_id_;
        conversation_id_.clear();
        session_id_.clear();
        pending_tts_stop_ = false;
        pending_tts_stop_text_.clear();
        pending_tts_stop_end_reason_.clear();
        pending_tts_stop_continue_listening_ = true;
    }

    ConversationTransportMode transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = selected_transport_;
    }
    if (send_goodbye && !conversation_to_end.empty()) {
        if (transport == ConversationTransportMode::kWebSocket) {
            SendWsConversationEnd(conversation_to_end, "device_end");
        } else {
            EndConversation(conversation_to_end);
        }
    }
    CloseWebSocket();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selected_transport_ = ConversationTransportMode::kHttp;
    }

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool KidsEnglishProtocol::IsAudioChannelOpened() const {
    ConversationTransportMode transport;
    bool opened = false;
    bool ws_connected = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        opened = channel_opened_ && !error_occurred_;
        transport = selected_transport_;
        ws_connected = IsWebSocketConnectedLocked();
    }
    if (!opened) {
        return false;
    }
    if (transport == ConversationTransportMode::kWebSocket) {
        return ws_connected && !IsWebSocketHeartbeatTimedOut();
    }
    return true;
}

bool KidsEnglishProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (packet == nullptr || packet->payload.empty()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!channel_opened_ || upload_in_progress_) {
        return false;
    }
    pending_audio_.push_back(std::move(packet));
    return true;
}

bool KidsEnglishProtocol::SendPcmAudio(std::vector<int16_t>&& pcm) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!channel_opened_ || upload_in_progress_) {
        return false;
    }
    pending_pcm_ = std::move(pcm);
    return true;
}

bool KidsEnglishProtocol::SubmitPcmAudio(std::vector<int16_t>&& pcm) {
    if (pcm.empty()) {
        ESP_LOGW(TAG, "No PCM audio captured for current prompt");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!channel_opened_ || upload_in_progress_) {
            ESP_LOGW(TAG, "Cannot submit Kids English PCM: channelOpen=%s uploadInProgress=%s",
                     channel_opened_ ? "true" : "false", upload_in_progress_ ? "true" : "false");
            return false;
        }
        upload_in_progress_ = true;
    }

    bool ok = UploadPcmAudioForCurrentConversation(std::move(pcm));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_audio_.clear();
        pending_pcm_.clear();
        upload_in_progress_ = false;
    }

    if (!ok) {
        SetError(Lang::Strings::SERVER_ERROR);
    }
    return ok;
}

void KidsEnglishProtocol::SetNextConversationTrigger(const std::string& trigger) {
    if (trigger != "wake_word" && trigger != "touch" && trigger != "manual") {
        ESP_LOGW(TAG, "Ignoring invalid conversation trigger: %s", trigger.c_str());
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    next_conversation_trigger_ = trigger;
}

void KidsEnglishProtocol::SendStartListening(ListeningMode mode) {
    (void)mode;
    std::lock_guard<std::mutex> lock(mutex_);
    pending_audio_.clear();
    pending_pcm_.clear();
}

void KidsEnglishProtocol::SendStopListening() {
    std::vector<std::unique_ptr<AudioStreamPacket>> audio_to_upload;
    std::vector<int16_t> pcm_to_upload;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!channel_opened_ || upload_in_progress_) {
            return;
        }
        audio_to_upload = std::move(pending_audio_);
        pcm_to_upload = std::move(pending_pcm_);
        pending_audio_.clear();
        pending_pcm_.clear();
        upload_in_progress_ = true;
    }

    if (audio_to_upload.empty() && pcm_to_upload.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        upload_in_progress_ = false;
        ESP_LOGW(TAG, "No audio captured for current prompt");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_audio_ = std::move(audio_to_upload);
        pending_pcm_ = std::move(pcm_to_upload);
    }

    bool ok = UploadPendingAudio();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_audio_.clear();
        pending_pcm_.clear();
        upload_in_progress_ = false;
    }

    if (!ok) {
        SetError(Lang::Strings::SERVER_ERROR);
    }
}

bool KidsEnglishProtocol::SendText(const std::string& text) {
    if (text.empty()) {
        return true;
    }
    if (text[0] == '{') {
        ESP_LOGI(TAG, "Ignoring JSON command in kids English HTTP protocol: %s", text.c_str());
        return true;
    }

    StandaloneTtsResponse response;
    if (!RequestStandaloneTts(text, response)) {
        return false;
    }
    return HandleStandaloneTtsResponse(response, true);
}

std::string KidsEnglishProtocol::TrimTrailingSlash(std::string url) {
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "kids_english_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include <utility>
#include <ctime>

#define TAG "Application"

namespace {
std::string PrintJsonUnformatted(const cJSON* root) {
    char* printed = cJSON_PrintUnformatted(root);
    std::string result = printed == nullptr ? "{}" : printed;
    if (printed != nullptr) {
        cJSON_free(printed);
    }
    return result;
}
}  // namespace

#if CONFIG_USE_KIDS_ENGLISH_SERVER
#ifndef CONFIG_KIDS_ENGLISH_INITIAL_SILENCE_TIMEOUT_MS
#define CONFIG_KIDS_ENGLISH_INITIAL_SILENCE_TIMEOUT_MS 7000
#endif
#ifndef CONFIG_KIDS_ENGLISH_END_SILENCE_TIMEOUT_MS
#define CONFIG_KIDS_ENGLISH_END_SILENCE_TIMEOUT_MS 800
#endif
#ifndef CONFIG_KIDS_ENGLISH_MAX_RECORDING_DURATION_MS
#define CONFIG_KIDS_ENGLISH_MAX_RECORDING_DURATION_MS 10000
#endif

namespace {
constexpr int64_t kKidsEnglishInitialSilenceTimeoutMs =
    CONFIG_KIDS_ENGLISH_INITIAL_SILENCE_TIMEOUT_MS;
constexpr int64_t kKidsEnglishEndSilenceTimeoutMs = CONFIG_KIDS_ENGLISH_END_SILENCE_TIMEOUT_MS;
constexpr int64_t kKidsEnglishMaxRecordingDurationMs =
    CONFIG_KIDS_ENGLISH_MAX_RECORDING_DURATION_MS;
constexpr int64_t kKidsEnglishRecordingCheckIntervalMs = 100;
}  // namespace
#endif


Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

#if CONFIG_USE_KIDS_ENGLISH_SERVER
    esp_timer_create_args_t kids_recording_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_KIDS_RECORDING_CHECK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "kids_recording_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&kids_recording_timer_args, &kids_english_recording_timer_handle_);
#endif
}

Application::~Application() {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    if (kids_english_recording_timer_handle_ != nullptr) {
        if (esp_timer_is_active(kids_english_recording_timer_handle_)) {
            esp_timer_stop(kids_english_recording_timer_handle_);
        }
        esp_timer_delete(kids_english_recording_timer_handle_);
    }
#endif
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    UpdateKidsEnglishConversationStatus();
#endif
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
        Schedule([this, speaking]() {
            HandleKidsEnglishVadChange(speaking);
        });
#endif
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_playback_queue_pushed = [this](size_t samples, size_t queue_depth) {
        if (protocol_) {
            protocol_->OnPcmPlaybackQueued(samples, queue_depth);
        }
    };
    callbacks.on_playback_started = [this](size_t samples, size_t remaining_queue_depth) {
        if (protocol_) {
            protocol_->OnAudioPlaybackStarted(samples, remaining_queue_depth);
        }
    };
    callbacks.on_playback_finished = [this](size_t samples, bool queue_drained) {
        if (protocol_) {
            protocol_->OnAudioPlaybackFinished(samples, queue_drained);
        }
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_KIDS_RECORDING_CHECK |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

#if CONFIG_USE_KIDS_ENGLISH_SERVER
        if (bits & MAIN_EVENT_KIDS_RECORDING_CHECK) {
            CheckKidsEnglishRecordingAutoStop();
        }
#endif

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
#if CONFIG_USE_KIDS_ENGLISH_SERVER
            if (GetDeviceState() == kDeviceStateIdle) {
                UpdateKidsEnglishConversationStatus();
            }
            CheckKidsEnglishRecordingAutoStop();
#endif
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
#if CONFIG_USE_KIDS_ENGLISH_SERVER
        if (kids_english_open_channel_task_handle_ != nullptr) {
            kids_english_reset_protocol_pending_ = true;
        } else
#endif
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });

    MaybeStartKidsEnglishSelfTest();
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

#if CONFIG_USE_KIDS_ENGLISH_SERVER
    ESP_LOGI(TAG, "Using Kids English HTTP conversation protocol");
    protocol_ = std::make_unique<KidsEnglishProtocol>();
#else
    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }
#endif

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        if (root == nullptr) {
            ESP_LOGW(TAG, "Ignoring null incoming JSON message");
            return;
        }
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming JSON message missing string type");
            return;
        }
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) {
                ESP_LOGW(TAG, "Ignoring TTS message missing string state: %s",
                         PrintJsonUnformatted(root).c_str());
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
#if CONFIG_USE_KIDS_ENGLISH_SERVER
                    kids_english_submission_waiting_for_response_ = false;
                    if (GetDeviceState() == kDeviceStateConnecting) {
                        SetDeviceState(kDeviceStateListening);
                    }
                    UpdateKidsEnglishConversationStatus("AI回复");
#endif
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                auto continue_listening = cJSON_GetObjectItem(root, "continue_listening");
                bool should_continue_listening = cJSON_IsTrue(continue_listening);
                Schedule([this, should_continue_listening]() {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
                    auto state = GetDeviceState();
                    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
                        if (should_continue_listening) {
                            SetDeviceState(kDeviceStateListening);
                        } else {
                            SetDeviceState(kDeviceStateIdle);
                        }
                    }
#else
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
#endif
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "conversation") == 0) {
            auto event = cJSON_GetObjectItem(root, "event");
            if (cJSON_IsString(event) && strcmp(event->valuestring, "started") == 0) {
                MarkKidsEnglishConversationStarted();
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", PrintJsonUnformatted(root).c_str());
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = PrintJsonUnformatted(payload)]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::ShowDebugInfo() {
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();

        int battery_level = 0;
        bool charging = false;
        bool discharging = false;
        float voltage = 0.0f;
        bool has_battery = board.GetBatteryLevel(battery_level, charging, discharging);
        bool has_voltage = board.GetBatteryVoltage(voltage);

        char message[256];
        snprintf(message, sizeof(message),
                 "Debug\nBoard: %s\nState: %d\nHeap: %u\nUUID: %.8s\nBattery: %s",
                 BOARD_NAME, static_cast<int>(GetDeviceState()),
                 static_cast<unsigned>(SystemInfo::GetFreeHeapSize()), board.GetUuid().c_str(),
                 has_battery ? (has_voltage ? "ok" : "level") : "n/a");
        display->SetChatMessage("system", message);

        if (has_battery) {
            char battery_message[64];
            if (has_voltage) {
                snprintf(battery_message, sizeof(battery_message), "Battery %d%% %.2fV",
                         battery_level, voltage);
            } else {
                snprintf(battery_message, sizeof(battery_message), "Battery %d%%", battery_level);
            }
            display->ShowNotification(battery_message);
        }
    });
}

void Application::ClearDebugMessages() {
    Schedule([]() {
        auto display = Board::GetInstance().GetDisplay();
        display->ClearChatMessages();
        display->SetStatus(Lang::Strings::STANDBY);
    });
}

void Application::AdjustOutputVolume(int delta) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    SetOutputVolume(codec->output_volume() + delta);
}

void Application::SetOutputVolume(int volume) {
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    if (codec == nullptr) {
        return;
    }

    codec->SetOutputVolume(volume);
    auto display = board.GetDisplay();
    display->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume), 1500);
    display->UpdateStatusBar(true);
}

void Application::ResetKidsEnglishDailyStatusIfNeeded() {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    time_t now = time(nullptr);
    struct tm tm_now;
    if (localtime_r(&now, &tm_now) == nullptr || tm_now.tm_year < 2025 - 1900) {
        return;
    }

    if (kids_english_daily_conversation_yday_ < 0) {
        kids_english_daily_conversation_yday_ = tm_now.tm_yday;
        return;
    }
    if (kids_english_daily_conversation_yday_ != tm_now.tm_yday) {
        kids_english_daily_conversation_yday_ = tm_now.tm_yday;
        kids_english_daily_conversation_count_ = 0;
    }
#endif
}

void Application::UpdateKidsEnglishConversationStatus(const char* phase) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    ResetKidsEnglishDailyStatusIfNeeded();

    char text[24];
    if (phase != nullptr && phase[0] != '\0') {
        snprintf(text, sizeof(text), "%s", phase);
    } else {
        switch (GetDeviceState()) {
            case kDeviceStateConnecting:
                snprintf(text, sizeof(text), "连接中");
                break;
            case kDeviceStateListening:
                snprintf(text, sizeof(text), "听你说");
                break;
            case kDeviceStateSpeaking:
                snprintf(text, sizeof(text),
                         kids_english_submission_waiting_for_response_ ? "提交中" : "AI回复");
                break;
            default:
                snprintf(text, sizeof(text), "今日%d轮", kids_english_daily_conversation_count_);
                break;
        }
    }
    Board::GetInstance().GetDisplay()->SetConversationStatus(text);
#else
    (void)phase;
#endif
}

void Application::MarkKidsEnglishConversationStarted() {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    Schedule([this]() {
        ResetKidsEnglishDailyStatusIfNeeded();
        ++kids_english_daily_conversation_count_;
        UpdateKidsEnglishConversationStatus();
    });
#endif
}

void Application::SendSimulatedRecording(const std::string& text) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    Schedule([this, text]() {
        if (kids_english_submit_recording_task_handle_ != nullptr) {
            Board::GetInstance().GetDisplay()->ShowNotification("录音提交中");
            return;
        }

        bool should_reload_protocol = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            should_reload_protocol = !kids_english_simulated_recording_active_ &&
                                     kids_english_simulated_recording_task_handle_ == nullptr &&
                                     kids_english_simulated_recording_queue_.empty() &&
                                     kids_english_simulated_recording_pcm_.empty();
        }
        if (should_reload_protocol &&
            (GetDeviceState() == kDeviceStateIdle || GetDeviceState() == kDeviceStateUnknown) &&
            (protocol_ == nullptr || !protocol_->IsAudioChannelOpened())) {
            ReloadKidsEnglishProtocol();
            ESP_LOGI(TAG, "Preparing Kids English simulated recording environment=%s url=%s",
                     KidsEnglishProtocol::GetConfiguredEnvironmentName().c_str(),
                     KidsEnglishProtocol::GetConfiguredBaseUrl().c_str());
        }

        bool should_start_task = false;
        size_t queued_segments = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            kids_english_simulated_recording_queue_.push_back(text);
            kids_english_simulated_recording_active_ = true;
            queued_segments = kids_english_simulated_recording_queue_.size();
            should_start_task = kids_english_simulated_recording_task_handle_ == nullptr;
        }
        Board::GetInstance().GetDisplay()->ShowNotification(
            queued_segments > 1 ? "模拟录音已加入队列" : "准备模拟录音", 2000);
        if (!should_start_task) {
            return;
        }

        if (!StartKidsEnglishSimulatedRecordingTask()) {
            Board::GetInstance().GetDisplay()->ShowNotification("模拟录音启动失败");
        }
    });
#else
    (void)text;
#endif
}

void Application::StartKidsEnglishSelfTest() {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    Schedule([this]() {
        if (kids_english_self_test_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Kids English self-test task already running");
            Board::GetInstance().GetDisplay()->ShowNotification("自测已在运行", 2000);
            return;
        }
        if (kids_english_open_channel_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Kids English connection task is running; self-test postponed");
            Board::GetInstance().GetDisplay()->ShowNotification("连接中，稍后再试", 2000);
            return;
        }

        ReloadKidsEnglishProtocol();

        ESP_LOGI(TAG, "Scheduling Kids English hardware self-test environment=%s url=%s",
                 KidsEnglishProtocol::GetConfiguredEnvironmentName().c_str(),
                 KidsEnglishProtocol::GetConfiguredBaseUrl().c_str());
        auto created = xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->KidsEnglishSelfTestTask();
            app->kids_english_self_test_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "kids_self_test", 4096 * 2, this, 3, &kids_english_self_test_task_handle_);
        if (created != pdPASS) {
            kids_english_self_test_task_handle_ = nullptr;
            ESP_LOGE(TAG, "Failed to create Kids English self-test task");
            Board::GetInstance().GetDisplay()->ShowNotification("自测启动失败", 2000);
            return;
        }
        Board::GetInstance().GetDisplay()->ShowNotification("自测开始", 2000);
    });
#else
    ESP_LOGW(TAG, "Kids English self-test unavailable: server support disabled");
#endif
}

void Application::ReloadKidsEnglishProtocol() {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    if (kids_english_open_channel_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Kids English connection task is running; deferring protocol reload");
        kids_english_reload_protocol_pending_ = true;
        return;
    }
    if (protocol_ != nullptr) {
        protocol_->CloseAudioChannel(false);
        protocol_.reset();
    }
    InitializeProtocol();
#endif
}

void Application::SetKidsEnglishEnvironment(KidsEnglishProtocol::Environment environment) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    Schedule([this, environment]() {
        auto display = Board::GetInstance().GetDisplay();
        auto state = GetDeviceState();
        if (state != kDeviceStateIdle && state != kDeviceStateUnknown) {
            display->ShowNotification("请先结束当前练习", 2000);
            return;
        }
        if (kids_english_self_test_task_handle_ != nullptr ||
            kids_english_open_channel_task_handle_ != nullptr ||
            kids_english_submit_recording_task_handle_ != nullptr ||
            kids_english_simulated_recording_task_handle_ != nullptr) {
            display->ShowNotification("任务运行中，稍后再切换", 2000);
            return;
        }

        KidsEnglishProtocol::SetConfiguredEnvironment(environment);
        ReloadKidsEnglishProtocol();
        DismissAlert();

        std::string message = "已切换到";
        message += environment == KidsEnglishProtocol::Environment::kProduction ? "生产环境" : "开发环境";
        display->ShowNotification(message, 2000);
        ESP_LOGI(TAG, "Kids English environment switched to %s url=%s",
                 KidsEnglishProtocol::GetConfiguredEnvironmentName().c_str(),
                 KidsEnglishProtocol::GetConfiguredBaseUrl().c_str());
    });
#else
    (void)environment;
#endif
}

bool Application::StartKidsEnglishSimulatedRecordingTask() {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (kids_english_simulated_recording_task_handle_ != nullptr) {
            return true;
        }
    }

    BaseType_t created = xTaskCreate([](void* arg) {
        auto& app = Application::GetInstance();
        app.KidsEnglishSimulatedRecordingTask("");
        bool has_more_segments = false;
        {
            std::lock_guard<std::mutex> lock(app.mutex_);
            app.kids_english_simulated_recording_task_handle_ = nullptr;
            has_more_segments = !app.kids_english_simulated_recording_queue_.empty();
        }
        if (has_more_segments) {
            app.StartKidsEnglishSimulatedRecordingTask();
        }
        vTaskDelete(NULL);
    }, "kids_sim_record", 4096 * 2, nullptr, 3,
       &kids_english_simulated_recording_task_handle_);
    if (created != pdPASS) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            kids_english_simulated_recording_queue_.clear();
            kids_english_simulated_recording_pcm_.clear();
            kids_english_simulated_recording_text_.clear();
            kids_english_simulated_recording_active_ = false;
            kids_english_simulated_submit_requested_ = false;
        }
        kids_english_simulated_recording_task_handle_ = nullptr;
        return false;
    }
    return true;
#else
    return false;
#endif
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
            static_cast<KidsEnglishProtocol*>(protocol_.get())->SetNextConversationTrigger("manual");
#endif
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
#if CONFIG_USE_KIDS_ENGLISH_SERVER
            Schedule([this, mode]() {
                StartKidsEnglishOpenAudioChannelTask(mode, "");
            });
#else
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
#endif
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
        SubmitKidsEnglishRecording();
#else
        protocol_->CloseAudioChannel();
#endif
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            if (GetDeviceState() == kDeviceStateConnecting) {
                SetDeviceState(kDeviceStateIdle);
            }
            return;
        }
    }

    SetListeningMode(mode);
}

bool Application::StartKidsEnglishOpenAudioChannelTask(ListeningMode mode, std::string wake_word) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    if (GetDeviceState() != kDeviceStateConnecting) {
        return false;
    }
    if (kids_english_open_channel_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Kids English connection task already running");
        Board::GetInstance().GetDisplay()->ShowNotification("连接中");
        return false;
    }
    if (protocol_ == nullptr) {
        ESP_LOGE(TAG, "Kids English connection failed: protocol not initialized");
        SetDeviceState(kDeviceStateIdle);
        return false;
    }
    listening_mode_ = mode;

    struct OpenArgs {
        Application* app;
        ListeningMode mode;
        std::string wake_word;
    };
    auto args = new OpenArgs{this, mode, std::move(wake_word)};
    BaseType_t created = xTaskCreate([](void* arg) {
        std::unique_ptr<OpenArgs> args(static_cast<OpenArgs*>(arg));
        args->app->KidsEnglishOpenAudioChannelTask(args->mode, std::move(args->wake_word));
        vTaskDelete(NULL);
    }, "kids_open_channel", 4096 * 4, args, 3, &kids_english_open_channel_task_handle_);
    if (created != pdPASS) {
        delete args;
        kids_english_open_channel_task_handle_ = nullptr;
        ESP_LOGE(TAG, "Failed to create Kids English connection task");
        Board::GetInstance().GetDisplay()->ShowNotification("连接启动失败");
        SetDeviceState(kDeviceStateIdle);
        return false;
    }
    return true;
#else
    (void)mode;
    (void)wake_word;
    return false;
#endif
}

void Application::KidsEnglishOpenAudioChannelTask(ListeningMode mode, std::string wake_word) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    bool opened = false;
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (protocol_ == nullptr) {
        ESP_LOGE(TAG, "Kids English connection task failed: protocol not initialized");
    } else if (GetDeviceState() != kDeviceStateConnecting) {
        ESP_LOGW(TAG, "Kids English connection task canceled: state=%d",
                 static_cast<int>(GetDeviceState()));
    } else if (protocol_->IsAudioChannelOpened()) {
        opened = true;
    } else {
        ESP_LOGI(TAG, "Opening Kids English audio channel in background task");
        opened = protocol_->OpenAudioChannel();
        ESP_LOGI(TAG, "Kids English audio channel open finished: ok=%s",
                 opened ? "true" : "false");
    }

    Schedule([this, mode, wake_word = std::move(wake_word), opened]() {
        kids_english_open_channel_task_handle_ = nullptr;
        kids_english_submission_waiting_for_response_ = false;
        if (kids_english_reload_protocol_pending_ || kids_english_reset_protocol_pending_) {
            bool should_reload = kids_english_reload_protocol_pending_;
            kids_english_reload_protocol_pending_ = false;
            kids_english_reset_protocol_pending_ = false;
            if (protocol_ != nullptr) {
                protocol_->CloseAudioChannel(false);
                protocol_.reset();
            }
            if (GetDeviceState() == kDeviceStateConnecting) {
                SetDeviceState(kDeviceStateIdle);
            }
            if (should_reload) {
                InitializeProtocol();
            }
            return;
        }
        if (!opened) {
            auto state = GetDeviceState();
            if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
                state == kDeviceStateSpeaking) {
                SetDeviceState(kDeviceStateIdle);
            }
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
            Board::GetInstance().GetDisplay()->ShowNotification(Lang::Strings::SERVER_NOT_CONNECTED);
            return;
        }
        if (wake_word.empty()) {
            if (GetDeviceState() == kDeviceStateConnecting) {
                SetListeningMode(mode);
            }
        } else {
            FinishWakeWordInvoke(wake_word);
        }
    });
#else
    (void)mode;
    (void)wake_word;
#endif
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
            static_cast<KidsEnglishProtocol*>(protocol_.get())->SetNextConversationTrigger("manual");
#endif
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
#if CONFIG_USE_KIDS_ENGLISH_SERVER
            Schedule([this]() {
                StartKidsEnglishOpenAudioChannelTask(kListeningModeManualStop, "");
            });
#else
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
#endif
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
            SubmitKidsEnglishRecording();
#else
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
#endif
        }
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
            static_cast<KidsEnglishProtocol*>(protocol_.get())->SetNextConversationTrigger("wake_word");
#endif
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue with OpenAudioChannel which may block for ~1 second
#if CONFIG_USE_KIDS_ENGLISH_SERVER
            Schedule([this, wake_word]() {
                StartKidsEnglishOpenAudioChannelTask(GetDefaultListeningMode(), wake_word);
            });
#else
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
#endif
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            audio_service_.EnableWakeWordDetection(true);
            if (GetDeviceState() == kDeviceStateConnecting) {
                SetDeviceState(kDeviceStateIdle);
            }
            return;
        }
    }

    FinishWakeWordInvoke(wake_word);
}

void Application::FinishWakeWordInvoke(const std::string& wake_word) {
    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::SubmitKidsEnglishRecording() {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    std::vector<int16_t> simulated_pcm;
    bool wait_for_simulated_generation = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool has_simulated_recording = kids_english_simulated_recording_active_ ||
                                       !kids_english_simulated_recording_pcm_.empty() ||
                                       !kids_english_simulated_recording_queue_.empty();
        if (has_simulated_recording) {
            if (kids_english_simulated_recording_task_handle_ != nullptr ||
                !kids_english_simulated_recording_queue_.empty()) {
                kids_english_simulated_submit_requested_ = true;
                wait_for_simulated_generation = true;
            } else {
                simulated_pcm = std::move(kids_english_simulated_recording_pcm_);
                kids_english_simulated_recording_text_.clear();
                kids_english_simulated_recording_active_ = false;
                kids_english_simulated_submit_requested_ = false;
            }
        }
    }
    if (wait_for_simulated_generation) {
        StopKidsEnglishRecordingDetection();
        Board::GetInstance().GetDisplay()->ShowNotification("模拟录音生成中");
        return;
    }
    if (!simulated_pcm.empty()) {
        ESP_LOGI(TAG, "Kids English simulated PCM capture: samples=%u bytes=%u durationMs=%u",
                 (unsigned)simulated_pcm.size(), (unsigned)(simulated_pcm.size() * sizeof(int16_t)),
                 (unsigned)(simulated_pcm.size() * 1000 / 16000));
        ESP_LOGI(TAG, "Submitting Kids English simulated recording environment=%s url=%s",
                 KidsEnglishProtocol::GetConfiguredEnvironmentName().c_str(),
                 KidsEnglishProtocol::GetConfiguredBaseUrl().c_str());
        StartKidsEnglishRecordingSubmission(std::move(simulated_pcm), "simulated");
        return;
    }

    StopKidsEnglishRecordingDetection();

    if (kids_english_submit_recording_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Kids English recording submission already running");
        Board::GetInstance().GetDisplay()->ShowNotification("录音提交中");
        return;
    }

    audio_service_.EnableVoiceProcessing(false);
    vTaskDelay(pdMS_TO_TICKS(OPUS_FRAME_DURATION_MS * 2));
    auto pcm = audio_service_.EndPcmCapture();
    ESP_LOGI(TAG, "Kids English PCM capture: samples=%u bytes=%u durationMs=%u",
             (unsigned)pcm.size(), (unsigned)(pcm.size() * sizeof(int16_t)),
             (unsigned)(pcm.size() * 1000 / 16000));
    while (audio_service_.PopPacketFromSendQueue()) {
    }

    if (pcm.empty()) {
        ESP_LOGW(TAG, "No Kids English PCM recording captured");
        CancelKidsEnglishRecording("empty_pcm");
        return;
    }

    StartKidsEnglishRecordingSubmission(std::move(pcm), "microphone");
#endif
}

void Application::CancelKidsEnglishRecording(const char* reason) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    ESP_LOGI(TAG, "Canceling Kids English recording: %s", reason);
    StopKidsEnglishRecordingDetection();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        kids_english_simulated_recording_queue_.clear();
        kids_english_simulated_recording_pcm_.clear();
        kids_english_simulated_recording_text_.clear();
        kids_english_simulated_recording_active_ = false;
        kids_english_simulated_submit_requested_ = false;
    }
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EndPcmCapture();
    while (audio_service_.PopPacketFromSendQueue()) {
    }
    if (protocol_ != nullptr && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel(false);
    } else {
        SetDeviceState(kDeviceStateIdle);
    }
#else
    (void)reason;
#endif
}

bool Application::StartKidsEnglishRecordingSubmission(std::vector<int16_t>&& pcm, const char* source) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    StopKidsEnglishRecordingDetection();

    if (kids_english_submit_recording_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Kids English recording submission already running");
        Board::GetInstance().GetDisplay()->ShowNotification("录音提交中");
        return false;
    }
    if (pcm.empty()) {
        ESP_LOGW(TAG, "Refusing to submit empty Kids English %s recording", source);
        SetDeviceState(kDeviceStateIdle);
        return false;
    }
    if (protocol_ == nullptr) {
        ESP_LOGE(TAG, "Kids English %s recording submission failed: protocol not initialized", source);
        SetDeviceState(kDeviceStateIdle);
        return false;
    }

    auto task_pcm = new std::vector<int16_t>(std::move(pcm));
    kids_english_submission_waiting_for_response_ = true;
    SetDeviceState(kDeviceStateSpeaking);
    UpdateKidsEnglishConversationStatus("提交中");
    BaseType_t created = xTaskCreate([](void* arg) {
        std::unique_ptr<std::vector<int16_t>> pcm(static_cast<std::vector<int16_t>*>(arg));
        auto& app = Application::GetInstance();
        app.KidsEnglishSubmitRecordingTask(std::move(*pcm));
        app.kids_english_submit_recording_task_handle_ = nullptr;
        vTaskDelete(NULL);
    }, "kids_submit", 4096 * 3, task_pcm, 3, &kids_english_submit_recording_task_handle_);
    if (created != pdPASS) {
        delete task_pcm;
        kids_english_submit_recording_task_handle_ = nullptr;
        kids_english_submission_waiting_for_response_ = false;
        ESP_LOGE(TAG, "Failed to create Kids English %s recording submission task", source);
        Board::GetInstance().GetDisplay()->ShowNotification("录音提交启动失败");
        SetDeviceState(kDeviceStateIdle);
        return false;
    }
    ESP_LOGI(TAG, "Queued Kids English %s recording submission", source);
    return true;
#else
    (void)pcm;
    (void)source;
    return false;
#endif
}

void Application::KidsEnglishSubmitRecordingTask(std::vector<int16_t> pcm) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    bool submitted = false;
    if (protocol_ == nullptr) {
        ESP_LOGE(TAG, "Kids English recording submission failed: protocol not initialized");
    } else {
        auto kids_protocol = static_cast<KidsEnglishProtocol*>(protocol_.get());
        ESP_LOGI(TAG, "Submitting Kids English recording to server");
        submitted = kids_protocol->SubmitPcmAudio(std::move(pcm));
        ESP_LOGI(TAG, "Kids English recording submission finished: ok=%s",
                 submitted ? "true" : "false");
    }

    if (!submitted) {
        Schedule([]() {
            auto& app = Application::GetInstance();
            app.kids_english_submission_waiting_for_response_ = false;
            auto display = Board::GetInstance().GetDisplay();
            display->ShowNotification("录音提交失败");
            app.SetDeviceState(kDeviceStateIdle);
        });
    }
#else
    (void)pcm;
#endif
}

void Application::StartKidsEnglishRecordingDetection() {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    int64_t now_ms = esp_timer_get_time() / 1000;
    kids_english_recording_detector_active_ = true;
    kids_english_recording_has_voice_ = false;
    kids_english_recording_started_at_ms_ = now_ms;
    kids_english_recording_silence_started_at_ms_ = now_ms;
    StartKidsEnglishRecordingCheckTimer();
    ESP_LOGI(TAG, "Kids English recording detector started");
#endif
}

void Application::StopKidsEnglishRecordingDetection() {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    if (!kids_english_recording_detector_active_) {
        return;
    }
    kids_english_recording_detector_active_ = false;
    kids_english_recording_has_voice_ = false;
    kids_english_recording_started_at_ms_ = 0;
    kids_english_recording_silence_started_at_ms_ = 0;
    StopKidsEnglishRecordingCheckTimer();
    ESP_LOGI(TAG, "Kids English recording detector stopped");
#endif
}

void Application::StartKidsEnglishRecordingCheckTimer() {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    if (kids_english_recording_timer_handle_ == nullptr) {
        return;
    }
    if (esp_timer_is_active(kids_english_recording_timer_handle_)) {
        esp_timer_stop(kids_english_recording_timer_handle_);
    }
    esp_timer_start_periodic(kids_english_recording_timer_handle_,
                             kKidsEnglishRecordingCheckIntervalMs * 1000);
#endif
}

void Application::StopKidsEnglishRecordingCheckTimer() {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    if (kids_english_recording_timer_handle_ == nullptr) {
        return;
    }
    if (esp_timer_is_active(kids_english_recording_timer_handle_)) {
        esp_timer_stop(kids_english_recording_timer_handle_);
    }
#endif
}

void Application::HandleKidsEnglishVadChange(bool speaking) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    if (!kids_english_recording_detector_active_ || GetDeviceState() != kDeviceStateListening) {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (speaking) {
        if (!kids_english_recording_has_voice_) {
            ESP_LOGI(TAG, "Kids English recording detected speech");
        }
        kids_english_recording_has_voice_ = true;
        kids_english_recording_silence_started_at_ms_ = 0;
    } else {
        kids_english_recording_silence_started_at_ms_ = now_ms;
        ESP_LOGI(TAG, "Kids English recording detected silence");
    }
#else
    (void)speaking;
#endif
}

void Application::CheckKidsEnglishRecordingAutoStop() {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    if (!kids_english_recording_detector_active_ || GetDeviceState() != kDeviceStateListening) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (kids_english_simulated_recording_active_ &&
            (kids_english_simulated_recording_task_handle_ != nullptr ||
             !kids_english_simulated_recording_queue_.empty())) {
            return;
        }
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t recording_ms = now_ms - kids_english_recording_started_at_ms_;
    if (!kids_english_recording_has_voice_) {
        if (recording_ms >= kKidsEnglishInitialSilenceTimeoutMs) {
            ESP_LOGW(TAG, "Kids English recording timed out waiting for speech");
            Board::GetInstance().GetDisplay()->ShowNotification("未检测到语音", 2000);
            CancelKidsEnglishRecording("initial_silence_timeout");
        }
        return;
    }

    bool silence_timeout =
        kids_english_recording_silence_started_at_ms_ > 0 &&
        now_ms - kids_english_recording_silence_started_at_ms_ >= kKidsEnglishEndSilenceTimeoutMs;
    bool max_duration_timeout = recording_ms >= kKidsEnglishMaxRecordingDurationMs;
    if (!silence_timeout && !max_duration_timeout) {
        return;
    }

    ESP_LOGI(TAG, "Kids English recording auto stop: silence=%d maxDuration=%d durationMs=%d",
             silence_timeout, max_duration_timeout, static_cast<int>(recording_ms));
    SubmitKidsEnglishRecording();
#endif
}

void Application::MaybeStartKidsEnglishSelfTest() {
#if CONFIG_USE_KIDS_ENGLISH_SERVER && CONFIG_KIDS_ENGLISH_AUTO_SELF_TEST
    ESP_LOGI(TAG, "KIDS_ENGLISH_AUTO_SELF_TEST enabled; starting Kids English self-test");
    StartKidsEnglishSelfTest();
#endif
}

void Application::KidsEnglishSelfTestTask() {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    vTaskDelay(pdMS_TO_TICKS(1500));
    Schedule([]() {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus("Kids English test");
        display->SetChatMessage("system", "Running Kids English self-test");
    });

    bool passed = false;
    if (protocol_ == nullptr) {
        ESP_LOGE(TAG, "KIDS_ENGLISH_SELF_TEST_FAIL step=protocol reason=not_initialized");
    } else {
        SetDeviceState(kDeviceStateConnecting);
        auto& board = Board::GetInstance();
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        passed = static_cast<KidsEnglishProtocol*>(protocol_.get())->RunSelfTest();
        audio_service_.WaitForPlaybackQueueEmpty();
        if (protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel(false);
        }
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    }

    Schedule([this, passed]() {
        auto display = Board::GetInstance().GetDisplay();
        display->SetChatMessage("system", passed ? "Kids English self-test passed"
                                                 : "Kids English self-test failed");
        SetDeviceState(kDeviceStateIdle);
    });
#endif
}

void Application::KidsEnglishSimulatedRecordingTask(std::string text) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    (void)text;

    bool ok = false;
    if (protocol_ == nullptr) {
        ESP_LOGE(TAG, "Simulated recording failed: protocol not initialized");
    } else {
        auto kids_protocol = static_cast<KidsEnglishProtocol*>(protocol_.get());
        if (!protocol_->IsAudioChannelOpened()) {
            kids_protocol->SetNextConversationTrigger("touch");
            SetDeviceState(kDeviceStateConnecting);
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            if (!protocol_->OpenAudioChannel()) {
                ESP_LOGE(TAG, "Simulated recording failed to open audio channel");
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    kids_english_simulated_recording_queue_.clear();
                    kids_english_simulated_recording_pcm_.clear();
                    kids_english_simulated_recording_text_.clear();
                    kids_english_simulated_recording_active_ = false;
                    kids_english_simulated_submit_requested_ = false;
                }
                SetDeviceState(kDeviceStateIdle);
            }
        }

        if (protocol_->IsAudioChannelOpened()) {
            audio_service_.WaitForPlaybackQueueEmpty();
            SetListeningMode(kListeningModeManualStop);
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EndPcmCapture();
            while (audio_service_.PopPacketFromSendQueue()) {
            }

            while (true) {
                std::string next_text;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (kids_english_simulated_recording_queue_.empty()) {
                        break;
                    }
                    next_text = std::move(kids_english_simulated_recording_queue_.front());
                    kids_english_simulated_recording_queue_.pop_front();
                }

                Schedule([next_text]() {
                    auto display = Board::GetInstance().GetDisplay();
                    display->SetChatMessage("user", next_text.c_str());
                });

                std::vector<int16_t> pcm;
                if (!kids_protocol->GenerateSimulatedRecordingPcm(next_text, pcm)) {
                    ESP_LOGE(TAG, "Simulated recording segment generation failed: %s",
                             next_text.c_str());
                    break;
                }
                if (!AppendKidsEnglishSimulatedRecordingSegment(next_text, std::move(pcm))) {
                    ESP_LOGE(TAG, "Failed to append simulated recording segment: %s",
                             next_text.c_str());
                    break;
                }
                ok = true;
            }
        }
    }

    Schedule([ok]() {
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification(ok ? "模拟录音已加入" : "模拟录音失败", 3000);
        if (!ok) {
            display->SetChatMessage("system", "Simulated recording failed");
        }
    });

    bool submit_requested = false;
    std::vector<int16_t> pcm_to_submit;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        submit_requested = kids_english_simulated_submit_requested_;
        if (submit_requested && kids_english_simulated_recording_queue_.empty()) {
            kids_english_simulated_submit_requested_ = false;
            pcm_to_submit = std::move(kids_english_simulated_recording_pcm_);
            kids_english_simulated_recording_text_.clear();
            kids_english_simulated_recording_active_ = false;
        }
    }
    if (submit_requested && !pcm_to_submit.empty()) {
        StartKidsEnglishRecordingSubmission(std::move(pcm_to_submit), "simulated");
    }
    if (!ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        kids_english_simulated_recording_queue_.clear();
        kids_english_simulated_recording_pcm_.clear();
        kids_english_simulated_recording_text_.clear();
        kids_english_simulated_recording_active_ = false;
        kids_english_simulated_submit_requested_ = false;
    }
#else
    (void)text;
#endif
}

bool Application::AppendKidsEnglishSimulatedRecordingSegment(const std::string& text,
                                                            std::vector<int16_t>&& pcm) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    if (pcm.empty()) {
        return false;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    size_t total_samples = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!kids_english_simulated_recording_pcm_.empty()) {
            constexpr int kSimulatedSegmentGapSamples = 16000 / 4;
            kids_english_simulated_recording_pcm_.insert(
                kids_english_simulated_recording_pcm_.end(), kSimulatedSegmentGapSamples, 0);
        }
        kids_english_simulated_recording_pcm_.insert(kids_english_simulated_recording_pcm_.end(),
                                                     pcm.begin(), pcm.end());
        if (!kids_english_simulated_recording_text_.empty()) {
            kids_english_simulated_recording_text_ += " ";
        }
        kids_english_simulated_recording_text_ += text;
        kids_english_simulated_recording_active_ = true;
        kids_english_recording_detector_active_ = true;
        kids_english_recording_has_voice_ = true;
        if (kids_english_recording_started_at_ms_ == 0) {
            kids_english_recording_started_at_ms_ = now_ms;
        }
        kids_english_recording_silence_started_at_ms_ = now_ms;
        StartKidsEnglishRecordingCheckTimer();
        total_samples = kids_english_simulated_recording_pcm_.size();
    }

    ESP_LOGI(TAG, "Appended simulated Kids English recording: text=%s totalSamples=%u durationMs=%u",
             text.c_str(), (unsigned)total_samples, (unsigned)(total_samples * 1000 / 16000));
    return true;
#else
    (void)text;
    (void)pcm;
    return false;
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
#if CONFIG_USE_KIDS_ENGLISH_SERVER
            StopKidsEnglishRecordingDetection();
            UpdateKidsEnglishConversationStatus();
#endif
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();  // Clear messages first
            display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
#if CONFIG_USE_KIDS_ENGLISH_SERVER
            UpdateKidsEnglishConversationStatus("连接中");
#endif
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening: {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
            UpdateKidsEnglishConversationStatus("听你说");
#endif
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
#if CONFIG_USE_KIDS_ENGLISH_SERVER
            bool using_simulated_recording = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                using_simulated_recording = kids_english_simulated_recording_active_ ||
                                            kids_english_simulated_recording_task_handle_ != nullptr ||
                                            !kids_english_simulated_recording_queue_.empty();
            }
#endif

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for playback queue to be empty before enabling voice processing
                // This prevents audio truncation when STOP arrives late due to network jitter
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                // Send the start listening command
                while (audio_service_.PopPacketFromSendQueue());
                protocol_->SendStartListening(listening_mode_);
#if CONFIG_USE_KIDS_ENGLISH_SERVER
                if (using_simulated_recording) {
                    audio_service_.EndPcmCapture();
                    audio_service_.EnableVoiceProcessing(false);
                } else {
                    audio_service_.BeginPcmCapture();
                    audio_service_.EnableVoiceProcessing(true);
                    if (listening_mode_ == kListeningModeAutoStop) {
                        StartKidsEnglishRecordingDetection();
                    }
                }
#else
                audio_service_.EnableVoiceProcessing(true);
#endif
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // Enable wake word detection in listening mode (configured via Kconfig)
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // Disable wake word detection in listening mode
            audio_service_.EnableWakeWordDetection(false);
#endif
            
            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        }
        case kDeviceStateSpeaking:
#if CONFIG_USE_KIDS_ENGLISH_SERVER
            StopKidsEnglishRecordingDetection();
            UpdateKidsEnglishConversationStatus();
#endif
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
#if !CONFIG_USE_KIDS_ENGLISH_SERVER
            audio_service_.ResetDecoder();
#endif
            break;
        case kDeviceStateWifiConfiguring:
#if CONFIG_USE_KIDS_ENGLISH_SERVER
            StopKidsEnglishRecordingDetection();
#endif
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
#if CONFIG_USE_AUDIO_PROCESSOR
    return kListeningModeAutoStop;
#else
    return kListeningModeManualStop;
#endif
#else
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
#endif
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    if (kids_english_open_channel_task_handle_ != nullptr) {
        kids_english_reset_protocol_pending_ = true;
    } else
#endif
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
#if CONFIG_USE_KIDS_ENGLISH_SERVER
    if (kids_english_open_channel_task_handle_ == nullptr)
#endif
    {
        protocol_.reset();
    }
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

#if CONFIG_USE_KIDS_ENGLISH_SERVER
    if (kids_english_open_channel_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Firmware upgrade postponed: Kids English connection task is running");
        display->ShowNotification("连接中，稍后升级", 2000);
        return false;
    }
#endif

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
            static_cast<KidsEnglishProtocol*>(protocol_.get())->SetNextConversationTrigger("wake_word");
#endif
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
#if CONFIG_USE_KIDS_ENGLISH_SERVER
            Schedule([this, wake_word]() {
                StartKidsEnglishOpenAudioChannelTask(GetDefaultListeningMode(), wake_word);
            });
#else
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
#endif
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload](){ 
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    Schedule([this]() {
#if CONFIG_USE_KIDS_ENGLISH_SERVER
        if (kids_english_open_channel_task_handle_ != nullptr) {
            kids_english_reset_protocol_pending_ = true;
            return;
        }
#endif
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

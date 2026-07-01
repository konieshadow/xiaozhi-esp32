#include "kids_english_protocol_internal.h"

using namespace kids_english_protocol_internal;

std::vector<KidsEnglishProtocol::WelcomeAudioCandidate>
KidsEnglishProtocol::ParseWelcomeAudioCandidates(const cJSON* root) const {
    std::vector<WelcomeAudioCandidate> candidates;
    auto data = cJSON_GetObjectItem(root, "data");
    auto candidate_array = cJSON_GetObjectItem(data, "welcomeAudioCandidates");
    if (!cJSON_IsArray(candidate_array)) {
        ESP_LOGI(TAG, "WELCOME_AUDIO_CANDIDATES total=0 ready=0 usable=0");
        return candidates;
    }

    int total = cJSON_GetArraySize(candidate_array);
    int ready = 0;
    int usable = 0;
    cJSON* item = nullptr;
    cJSON_ArrayForEach (item, candidate_array) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        WelcomeAudioCandidate candidate;
        candidate.id = JsonString(cJSON_GetObjectItem(item, "id"));
        candidate.tts_text = JsonString(cJSON_GetObjectItem(item, "ttsText"));
        candidate.tts_audio_url =
            ResolveAudioUrl(JsonString(cJSON_GetObjectItem(item, "ttsAudioUrl")));
        candidate.audio_format = JsonString(cJSON_GetObjectItem(item, "audioFormat"));
        candidate.status = JsonString(cJSON_GetObjectItem(item, "status"));
        candidate.cache_key = JsonString(cJSON_GetObjectItem(item, "cacheKey"));
        candidate.voice = JsonString(cJSON_GetObjectItem(item, "voice"));
        bool is_ready = candidate.status == "ready";
        bool is_usable = is_ready && !candidate.tts_audio_url.empty() &&
                         candidate.audio_format == "wav" && !candidate.cache_key.empty();
        if (is_ready) {
            ++ready;
        }
        if (is_usable) {
            ++usable;
            if (candidates.size() < kMaxWelcomeAudioCandidates) {
                candidates.push_back(std::move(candidate));
            }
        }
    }

    ESP_LOGI(TAG, "WELCOME_AUDIO_CANDIDATES total=%d ready=%d usable=%d stored=%u", total, ready,
             usable, (unsigned)candidates.size());
    return candidates;
}

void KidsEnglishProtocol::StoreWelcomeAudioCandidates(
    std::vector<WelcomeAudioCandidate>&& candidates, const std::string& voice,
    bool schedule_prefetch) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (voice != welcome_audio_voice_) {
            ESP_LOGI(TAG, "WELCOME_AUDIO_CACHE_CLEAR oldVoice=%s newVoice=%s",
                     welcome_audio_voice_.c_str(), voice.c_str());
            welcome_audio_cache_.clear();
            welcome_audio_voice_ = voice;
        }
        for (auto& candidate : candidates) {
            if (candidate.voice.empty()) {
                candidate.voice = voice;
            }
        }
        welcome_audio_candidates_ = std::move(candidates);
    }
    if (schedule_prefetch) {
        PrefetchWelcomeAudioAsync(false);
    }
}

void KidsEnglishProtocol::PrefetchWelcomeAudioAsync(bool refresh_device_hello) {
    bool should_start = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (welcome_audio_destroying_) {
            return;
        }
        welcome_audio_refresh_requested_ = welcome_audio_refresh_requested_ || refresh_device_hello;
        if (welcome_audio_prefetch_in_progress_) {
            welcome_audio_prefetch_pending_ = true;
            ESP_LOGD(TAG, "WELCOME_AUDIO_PREFETCH_PENDING");
            return;
        }
        welcome_audio_prefetch_in_progress_ = true;
        should_start = true;
    }
    if (!should_start) {
        return;
    }

    BaseType_t created = xTaskCreate(
        [](void* arg) {
            auto* self = static_cast<KidsEnglishProtocol*>(arg);
            if (self != nullptr) {
                self->PrefetchWelcomeAudioTask();
            }
            vTaskDelete(NULL);
        },
        "kids_welcome_audio", 4096 * 2, this, 2, nullptr);
    if (created != pdPASS) {
        std::lock_guard<std::mutex> lock(mutex_);
        welcome_audio_prefetch_in_progress_ = false;
        welcome_audio_prefetch_pending_ = false;
        ESP_LOGW(TAG, "WELCOME_AUDIO_PREFETCH_TASK_FAILED");
    }
}

void KidsEnglishProtocol::PrefetchWelcomeAudioTask() {
    while (true) {
        bool refresh_device_hello = false;
        bool should_stop = false;
        std::vector<WelcomeAudioCandidate> candidates;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            should_stop = welcome_audio_destroying_;
            if (should_stop) {
                welcome_audio_prefetch_in_progress_ = false;
                welcome_audio_prefetch_pending_ = false;
                welcome_audio_refresh_requested_ = false;
            }
            refresh_device_hello = welcome_audio_refresh_requested_;
            welcome_audio_refresh_requested_ = false;
            candidates = welcome_audio_candidates_;
            welcome_audio_prefetch_pending_ = false;
        }
        if (should_stop) {
            break;
        }

        if (refresh_device_hello) {
            if (!CheckHealth(false) || !DeviceHello(false, false, false)) {
                ESP_LOGW(TAG, "WELCOME_AUDIO_PREFETCH_HELLO_FAILED");
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                candidates = welcome_audio_candidates_;
            }
        }

        for (const auto& candidate : candidates) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (welcome_audio_destroying_) {
                    break;
                }
                if (IsWelcomeAudioCachedLocked(candidate.cache_key, candidate.voice)) {
                    ESP_LOGI(TAG, "WELCOME_AUDIO_PREFETCH_SKIP cacheKey=%s voice=%s reason=cached",
                             candidate.cache_key.c_str(), candidate.voice.c_str());
                    continue;
                }
            }

            ESP_LOGI(TAG, "WELCOME_AUDIO_PREFETCH_BEGIN cacheKey=%s voice=%s url=%s",
                     candidate.cache_key.c_str(), candidate.voice.c_str(),
                     RedactUrlForLog(candidate.tts_audio_url).c_str());
            std::vector<int16_t> pcm;
            int sample_rate = 0;
            if (!DownloadWavAudio(candidate.tts_audio_url, pcm, sample_rate)) {
                ESP_LOGW(TAG, "WELCOME_AUDIO_PREFETCH_FAILED cacheKey=%s voice=%s url=%s",
                         candidate.cache_key.c_str(), candidate.voice.c_str(),
                         RedactUrlForLog(candidate.tts_audio_url).c_str());
                continue;
            }
            size_t samples = pcm.size();
            if (StoreWelcomeAudioCache(candidate, std::move(pcm), sample_rate)) {
                ESP_LOGI(TAG,
                         "WELCOME_AUDIO_PREFETCH_OK cacheKey=%s voice=%s sampleRate=%d "
                         "samples=%u",
                         candidate.cache_key.c_str(), candidate.voice.c_str(), sample_rate,
                         (unsigned)samples);
            } else {
                ESP_LOGW(TAG, "WELCOME_AUDIO_PREFETCH_FAILED cacheKey=%s voice=%s reason=store",
                         candidate.cache_key.c_str(), candidate.voice.c_str());
            }
        }

        bool rerun = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rerun = (welcome_audio_prefetch_pending_ || welcome_audio_refresh_requested_) &&
                    !welcome_audio_destroying_;
            if (!rerun) {
                welcome_audio_prefetch_in_progress_ = false;
                welcome_audio_prefetch_pending_ = false;
                welcome_audio_refresh_requested_ = false;
            }
        }
        if (!rerun) {
            break;
        }
    }
}

bool KidsEnglishProtocol::IsWelcomeAudioCachedLocked(const std::string& cache_key,
                                                     const std::string& voice) const {
    if (cache_key.empty()) {
        return false;
    }
    return std::any_of(welcome_audio_cache_.begin(), welcome_audio_cache_.end(),
                       [&](const WelcomeAudioCacheEntry& entry) {
                           return entry.cache_key == cache_key && entry.voice == voice &&
                                  !entry.pcm.empty() && entry.sample_rate > 0;
                       });
}

bool KidsEnglishProtocol::StoreWelcomeAudioCache(const WelcomeAudioCandidate& candidate,
                                                 std::vector<int16_t>&& pcm, int sample_rate) {
    if (candidate.cache_key.empty() || pcm.empty() || sample_rate <= 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (welcome_audio_destroying_) {
        return false;
    }
    auto existing = std::find_if(welcome_audio_cache_.begin(), welcome_audio_cache_.end(),
                                 [&](const WelcomeAudioCacheEntry& entry) {
                                     return entry.cache_key == candidate.cache_key &&
                                            entry.voice == candidate.voice;
                                 });
    if (existing != welcome_audio_cache_.end()) {
        existing->pcm = std::move(pcm);
        existing->sample_rate = sample_rate;
        return true;
    }
    if (welcome_audio_cache_.size() >= kMaxWelcomeAudioCacheEntries) {
        welcome_audio_cache_.erase(welcome_audio_cache_.begin());
    }
    WelcomeAudioCacheEntry entry;
    entry.cache_key = candidate.cache_key;
    entry.voice = candidate.voice;
    entry.pcm = std::move(pcm);
    entry.sample_rate = sample_rate;
    welcome_audio_cache_.push_back(std::move(entry));
    return true;
}

bool KidsEnglishProtocol::TryPlayWelcomeAudioCache(const ConversationResponse& response) {
    if (!response.welcome_audio_cached || response.welcome_audio_cache_key.empty()) {
        if (response.welcome_audio_cached || !response.welcome_audio_cache_key.empty()) {
            ESP_LOGI(TAG,
                     "WELCOME_AUDIO_CACHE_MISS requestId=%s cached=%s cacheKey=%s "
                     "reason=disabled",
                     response.request_id.c_str(), response.welcome_audio_cached ? "true" : "false",
                     response.welcome_audio_cache_key.c_str());
        }
        return false;
    }

    std::vector<int16_t> pcm;
    int sample_rate = 0;
    std::string voice;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        voice = welcome_audio_voice_;
        auto entry = std::find_if(welcome_audio_cache_.begin(), welcome_audio_cache_.end(),
                                  [&](const WelcomeAudioCacheEntry& item) {
                                      return item.cache_key == response.welcome_audio_cache_key &&
                                             item.voice == voice && !item.pcm.empty() &&
                                             item.sample_rate > 0;
                                  });
        if (entry == welcome_audio_cache_.end()) {
            ESP_LOGI(TAG,
                     "WELCOME_AUDIO_CACHE_MISS requestId=%s cached=true cacheKey=%s voice=%s "
                     "reason=not_found",
                     response.request_id.c_str(), response.welcome_audio_cache_key.c_str(),
                     voice.c_str());
            return false;
        }
        pcm = entry->pcm;
        sample_rate = entry->sample_rate;
    }

    ESP_LOGI(TAG,
             "WELCOME_AUDIO_CACHE_HIT requestId=%s cacheKey=%s voice=%s sampleRate=%d samples=%u",
             response.request_id.c_str(), response.welcome_audio_cache_key.c_str(), voice.c_str(),
             sample_rate, (unsigned)pcm.size());
    return PlayPcmAudio(std::move(pcm), sample_rate);
}

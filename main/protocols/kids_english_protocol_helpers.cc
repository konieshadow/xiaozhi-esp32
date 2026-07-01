#include "kids_english_protocol_internal.h"

#include <esp_ae_rate_cvt.h>
#include <esp_audio_types.h>
#include <mbedtls/md.h>

#include <sys/time.h>

namespace kids_english_protocol_internal {

std::string JsonString(const cJSON* item) { return cJSON_IsString(item) ? item->valuestring : ""; }

int JsonInt(const cJSON* item, int fallback) {
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

bool JsonBool(const cJSON* item, bool fallback) {
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return fallback;
}

std::string JsonCompactString(const cJSON* item) {
    if (item == nullptr) {
        return "{}";
    }
    char* printed = cJSON_PrintUnformatted(item);
    std::string text = printed == nullptr ? "{}" : printed;
    if (printed != nullptr) {
        cJSON_free(printed);
    }
    return text;
}

uint32_t LogMs(int64_t ms) { return ms > 0 ? static_cast<uint32_t>(ms) : 0; }

int LogDeltaMs(int64_t from, int64_t to) {
    return from > 0 && to > 0 ? static_cast<int>(to - from) : -1;
}

std::string JsonEscape(const std::string& text) {
    cJSON* value = cJSON_CreateString(text.c_str());
    char* printed = cJSON_PrintUnformatted(value);
    std::string escaped = printed == nullptr ? "\"\"" : printed;
    if (printed != nullptr) {
        cJSON_free(printed);
    }
    cJSON_Delete(value);
    return escaped;
}

std::string ToHex(const uint8_t* data, size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.resize(size * 2);
    for (size_t i = 0; i < size; ++i) {
        hex[i * 2] = kHex[(data[i] >> 4) & 0x0f];
        hex[i * 2 + 1] = kHex[data[i] & 0x0f];
    }
    return hex;
}

std::string Sha256Hex(const std::string& data) {
    uint8_t digest[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_setup(&ctx, info, 0) != 0 || mbedtls_md_starts(&ctx) != 0 ||
        mbedtls_md_update(&ctx, reinterpret_cast<const unsigned char*>(data.data()), data.size()) !=
            0 ||
        mbedtls_md_finish(&ctx, digest) != 0) {
        ESP_LOGE(TAG, "SHA256 calculation failed");
        std::memset(digest, 0, sizeof(digest));
    }
    mbedtls_md_free(&ctx);
    return ToHex(digest, sizeof(digest));
}

std::string HmacSha256Hex(const std::string& key, const std::string& data) {
    uint8_t digest[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_setup(&ctx, info, 1) != 0 ||
        mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const unsigned char*>(key.data()),
                               key.size()) != 0 ||
        mbedtls_md_hmac_update(&ctx, reinterpret_cast<const unsigned char*>(data.data()),
                               data.size()) != 0 ||
        mbedtls_md_hmac_finish(&ctx, digest) != 0) {
        ESP_LOGE(TAG, "HMAC-SHA256 calculation failed");
        std::memset(digest, 0, sizeof(digest));
    }
    mbedtls_md_free(&ctx);
    return ToHex(digest, sizeof(digest));
}

int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + static_cast<int64_t>(doe) - 719468LL;
}

bool ParseIsoUtcMillis(const std::string& iso, int64_t& unix_ms) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millis = 0;
    if (std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d.%3dZ", &year, &month, &day, &hour,
                    &minute, &second, &millis) < 6) {
        return false;
    }
    int64_t days = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    unix_ms = (((days * 24 + hour) * 60 + minute) * 60 + second) * 1000 + millis;
    return true;
}

void AppendLe16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
}

void AppendLe32(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 24) & 0xff));
}

void AppendBe16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

void AppendBe32(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

uint16_t ReadLe16(const char* data) {
    return static_cast<uint16_t>(static_cast<uint8_t>(data[0])) |
           (static_cast<uint16_t>(static_cast<uint8_t>(data[1])) << 8);
}

uint32_t ReadLe32(const char* data) {
    return static_cast<uint32_t>(static_cast<uint8_t>(data[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[3])) << 24);
}

uint16_t ReadBe16(const char* data) {
    return (static_cast<uint16_t>(static_cast<uint8_t>(data[0])) << 8) |
           static_cast<uint16_t>(static_cast<uint8_t>(data[1]));
}

uint32_t ReadBe32(const char* data) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(data[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(data[3]));
}

bool ResamplePcm16Mono(std::vector<int16_t>& pcm, int source_sample_rate, int target_sample_rate) {
    if (source_sample_rate == target_sample_rate) {
        return true;
    }
    if (pcm.empty() || source_sample_rate <= 0 || target_sample_rate <= 0) {
        ESP_LOGE(TAG, "Invalid PCM resample request: samples=%u sourceRate=%d targetRate=%d",
                 (unsigned)pcm.size(), source_sample_rate, target_sample_rate);
        return false;
    }

    esp_ae_rate_cvt_handle_t resampler = nullptr;
    esp_ae_rate_cvt_cfg_t cfg = {
        .src_rate = static_cast<uint32_t>(source_sample_rate),
        .dest_rate = static_cast<uint32_t>(target_sample_rate),
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .complexity = 2,
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
    };
    auto ret = esp_ae_rate_cvt_open(&cfg, &resampler);
    if (resampler == nullptr) {
        ESP_LOGE(TAG, "Failed to create simulated recording resampler, error code: %d", ret);
        return false;
    }

    uint32_t target_samples = 0;
    esp_ae_rate_cvt_get_max_out_sample_num(resampler, pcm.size(), &target_samples);
    std::vector<int16_t> resampled(target_samples);
    uint32_t actual_output = target_samples;
    esp_ae_rate_cvt_process(resampler, reinterpret_cast<esp_ae_sample_t>(pcm.data()), pcm.size(),
                            reinterpret_cast<esp_ae_sample_t>(resampled.data()), &actual_output);
    esp_ae_rate_cvt_close(resampler);
    resampled.resize(actual_output);
    pcm = std::move(resampled);
    ESP_LOGI(TAG, "Resampled simulated recording PCM: sourceRate=%d targetRate=%d samples=%u",
             source_sample_rate, target_sample_rate, (unsigned)pcm.size());
    return true;
}

}  // namespace kids_english_protocol_internal

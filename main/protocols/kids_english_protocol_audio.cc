#include "kids_english_protocol_internal.h"

using namespace kids_english_protocol_internal;

bool KidsEnglishProtocol::ParsePcm16MonoBytes(const std::string& bytes,
                                              std::vector<int16_t>& pcm) const {
    if (bytes.empty() || (bytes.size() % sizeof(int16_t)) != 0) {
        ESP_LOGE(TAG, "Invalid PCM payload size: %u", (unsigned)bytes.size());
        return false;
    }
    pcm.resize(bytes.size() / sizeof(int16_t));
    std::memcpy(pcm.data(), bytes.data(), bytes.size());
    return true;
}

std::string KidsEnglishProtocol::CreateWavFile(const std::vector<int16_t>& pcm,
                                               int sample_rate) const {
    std::string wav;
    uint32_t data_size = pcm.size() * sizeof(int16_t);
    wav.reserve(44 + data_size);
    wav.append("RIFF", 4);
    AppendLe32(wav, 36 + data_size);
    wav.append("WAVE", 4);
    wav.append("fmt ", 4);
    AppendLe32(wav, 16);
    AppendLe16(wav, 1);
    AppendLe16(wav, 1);
    AppendLe32(wav, sample_rate);
    AppendLe32(wav, sample_rate * sizeof(int16_t));
    AppendLe16(wav, sizeof(int16_t));
    AppendLe16(wav, 16);
    wav.append("data", 4);
    AppendLe32(wav, data_size);
    wav.append(reinterpret_cast<const char*>(pcm.data()), data_size);
    return wav;
}

bool KidsEnglishProtocol::ParseWavPcm16Mono(const std::string& wav, std::vector<int16_t>& pcm,
                                            int& sample_rate) const {
    if (wav.size() < 44 || std::memcmp(wav.data(), "RIFF", 4) != 0 ||
        std::memcmp(wav.data() + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Invalid WAV header");
        return false;
    }

    size_t offset = 12;
    bool found_fmt = false;
    bool found_data = false;
    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t data_offset = 0;
    uint32_t data_size = 0;
    sample_rate = 0;

    while (offset + 8 <= wav.size()) {
        const char* chunk = wav.data() + offset;
        uint32_t chunk_size = ReadLe32(chunk + 4);
        offset += 8;
        if (offset + chunk_size > wav.size()) {
            if (std::memcmp(chunk, "data", 4) == 0) {
                ESP_LOGW(TAG, "WAV data chunk size %u exceeds remaining %u; using remaining bytes",
                         (unsigned)chunk_size, (unsigned)(wav.size() - offset));
                chunk_size = wav.size() - offset;
            } else {
                ESP_LOGE(TAG, "Truncated WAV chunk");
                return false;
            }
        }

        if (std::memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
            audio_format = ReadLe16(wav.data() + offset);
            channels = ReadLe16(wav.data() + offset + 2);
            sample_rate = ReadLe32(wav.data() + offset + 4);
            bits_per_sample = ReadLe16(wav.data() + offset + 14);
            found_fmt = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data_offset = offset;
            data_size = chunk_size;
            found_data = true;
        }

        size_t next_offset = offset + chunk_size + (chunk_size & 1);
        if (next_offset <= offset) {
            ESP_LOGE(TAG, "Invalid WAV chunk offset");
            return false;
        }
        offset = next_offset;
    }

    if (!found_fmt || !found_data || audio_format != 1 || channels != 1 || bits_per_sample != 16 ||
        sample_rate <= 0 || (data_size % sizeof(int16_t)) != 0) {
        ESP_LOGE(TAG, "Unsupported WAV format: format=%u channels=%u sample_rate=%d bits=%u",
                 audio_format, channels, sample_rate, bits_per_sample);
        return false;
    }

    pcm.resize(data_size / sizeof(int16_t));
    std::memcpy(pcm.data(), wav.data() + data_offset, data_size);
    return true;
}

bool KidsEnglishProtocol::DownloadWavAudio(const std::string& url, std::vector<int16_t>& pcm,
                                           int& sample_rate, std::string* content_type) {
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create HTTP client for TTS download");
        return false;
    }

    http->SetTimeout(kTtsDownloadTimeoutMs);
    http->SetHeader("Accept", "audio/wav,*/*");
    ESP_LOGI(TAG, "Downloading TTS audio: %s", RedactUrlForLog(url).c_str());
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "TTS download open failed, error=%d", http->GetLastError());
        return false;
    }

    int status = http->GetStatusCode();
    std::string response_content_type = http->GetResponseHeader("Content-Type");
    std::string body;
    bool read_ok = ReadHttpBody(http.get(), body);
    http->Close();
    if (content_type != nullptr) {
        *content_type = response_content_type;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "TTS download failed, status=%d, bytes=%u", status, (unsigned)body.size());
        return false;
    }
    if (!read_ok) {
        ESP_LOGE(TAG, "Failed to read TTS audio body");
        return false;
    }

    if (!ParseWavPcm16Mono(body, pcm, sample_rate)) {
        ESP_LOGE(TAG, "TTS response is not playable WAV, content_type=%s",
                 response_content_type.c_str());
        return false;
    }

    ESP_LOGI(TAG, "Downloaded TTS WAV: content_type=%s sample_rate=%d samples=%u",
             response_content_type.c_str(), sample_rate, (unsigned)pcm.size());
    return true;
}

bool KidsEnglishProtocol::DownloadAndPlayTtsAudio(const std::string& url) {
    std::vector<int16_t> pcm;
    int sample_rate = 0;
    if (!DownloadWavAudio(url, pcm, sample_rate)) {
        return false;
    }

    return PlayPcmAudio(std::move(pcm), sample_rate);
}

bool KidsEnglishProtocol::PlayPcmAudio(std::vector<int16_t>&& pcm, int sample_rate) {
    if (pcm.empty() || sample_rate <= 0) {
        return false;
    }
    AudioService& audio_service = Application::GetInstance().GetAudioService();
    audio_service.PushPcmToPlaybackQueue(std::move(pcm), sample_rate);
    return true;
}

bool KidsEnglishProtocol::ReadHttpBody(Http* http, std::string& body) {
    body.clear();
    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        body = http->ReadAll();
        return !body.empty();
    }

    body.resize(content_length);
    size_t total_read = 0;
    while (total_read < content_length) {
        int read = http->Read(body.data() + total_read, content_length - total_read);
        if (read <= 0) {
            ESP_LOGE(TAG, "HTTP body read failed at %u/%u bytes", (unsigned)total_read,
                     (unsigned)content_length);
            body.resize(total_read);
            return false;
        }
        total_read += read;
    }
    return true;
}

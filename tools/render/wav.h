#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

struct Stereo {
    std::vector<float> left, right;
    float sampleRate = 48000.0f;

    size_t size() const { return left.size(); }
    void resize(size_t n) {
        left.assign(n, 0.0f);
        right.assign(n, 0.0f);
    }
};

// Writes 24-bit PCM stereo.
inline bool writeWav(const std::string& path, const Stereo& audio) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const uint32_t frames = uint32_t(audio.size());
    const uint16_t channels = 2, bits = 24, blockAlign = channels * bits / 8;
    const uint32_t rate = uint32_t(audio.sampleRate), dataBytes = frames * blockAlign;
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(36 + dataBytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(channels);
    u32(rate);
    u32(rate * blockAlign);
    u16(blockAlign);
    u16(bits);
    f.write("data", 4);
    u32(dataBytes);
    std::vector<char> buffer(dataBytes);
    char* p = buffer.data();
    for (uint32_t i = 0; i < frames; ++i) {
        for (const float s : {audio.left[i], audio.right[i]}) {
            const int32_t v = int32_t(std::clamp(s, -1.0f, 1.0f) * 8388607.0f);
            *p++ = char(v & 0xFF);
            *p++ = char((v >> 8) & 0xFF);
            *p++ = char((v >> 16) & 0xFF);
        }
    }
    f.write(buffer.data(), std::streamsize(buffer.size()));
    return bool(f);
}

// Reads PCM 16/24/32-bit or float32 WAV, mono or stereo (extra channels ignored).
inline bool readWav(const std::string& path, Stereo& audio, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return error = "cannot open " + path, false;
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) || std::memcmp(data.data() + 8, "WAVE", 4))
        return error = "not a RIFF/WAVE file", false;

    auto u16 = [&](size_t at) { uint16_t v; std::memcpy(&v, data.data() + at, 2); return v; };
    auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, data.data() + at, 4); return v; };

    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    size_t dataAt = 0, dataSize = 0;
    for (size_t at = 12; at + 8 <= data.size();) {
        const uint32_t chunkSize = u32(at + 4);
        if (!std::memcmp(data.data() + at, "fmt ", 4)) {
            format = u16(at + 8);
            channels = u16(at + 10);
            rate = u32(at + 12);
            bits = u16(at + 22);
            if (format == 0xFFFE && chunkSize >= 40) format = u16(at + 32);  // WAVE_FORMAT_EXTENSIBLE
        } else if (!std::memcmp(data.data() + at, "data", 4)) {
            dataAt = at + 8;
            dataSize = std::min<size_t>(chunkSize, data.size() - dataAt);
        }
        at += 8 + chunkSize + (chunkSize & 1);
    }
    if (!dataAt || !channels) return error = "missing fmt or data chunk", false;
    if (!((format == 1 && (bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32)))
        return error = "unsupported sample format", false;

    const size_t bytes = bits / 8, frames = dataSize / (bytes * channels);
    audio.sampleRate = float(rate);
    audio.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        for (size_t c = 0; c < 2; ++c) {
            const size_t ch = std::min<size_t>(c, channels - 1);
            const char* s = data.data() + dataAt + (i * channels + ch) * bytes;
            float v;
            if (format == 3) {
                std::memcpy(&v, s, 4);
            } else if (bits == 16) {
                int16_t x;
                std::memcpy(&x, s, 2);
                v = float(x) / 32768.0f;
            } else if (bits == 24) {
                const int32_t x = int32_t(uint32_t(uint8_t(s[0])) << 8 | uint32_t(uint8_t(s[1])) << 16 |
                                          uint32_t(uint8_t(s[2])) << 24) >> 8;
                v = float(x) / 8388608.0f;
            } else {
                int32_t x;
                std::memcpy(&x, s, 4);
                v = float(double(x) / 2147483648.0);
            }
            (c ? audio.right : audio.left)[i] = v;
        }
    }
    return true;
}

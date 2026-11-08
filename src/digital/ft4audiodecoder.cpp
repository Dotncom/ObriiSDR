#include "ft4audiodecoder.h"
#include "fftplannermutex.h"
#include <fftw3.h>
extern "C" {
#include "ft8/decode.h"
}
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

void Ft4AudioDecoder::reset() {
    audio.clear(); recent.clear(); samples = 0; sinceAnalysis = 0;
    inputRate = 0; phase = sum = 0.0; count = 0;
}

QStringList Ft4AudioDecoder::process(const QByteArray &pcm, int sampleRate) {
    if (sampleRate < 12000 || sampleRate > 192000) return {};
    if (inputRate != sampleRate) { reset(); inputRate = sampleRate; }
    QStringList output;
    for (int offset = 0; offset + 1 < pcm.size(); offset += 2) {
        const auto *p = reinterpret_cast<const unsigned char *>(pcm.constData() + offset);
        const qint16 value = static_cast<qint16>(p[0] | (unsigned(p[1]) << 8));
        sum += value / 32768.0; ++count;
        phase += 12000.0;
        if (phase < sampleRate) continue;
        phase -= sampleRate;
        audio.push_back(static_cast<float>(sum / count)); sum = 0.0; count = 0;
        ++samples; ++sinceAnalysis;
        if (audio.size() >= 90000 && sinceAnalysis >= 12000) {
            sinceAnalysis = 0;
            output.append(analyze());
            audio.erase(audio.begin(), audio.begin() + 12000);
        }
    }
    return output;
}

QStringList Ft4AudioDecoder::analyze() {
    constexpr int block = 576, hop = 288, nfft = 1152, bins = 145;
    constexpr int blocks = 90000 / block;
    std::vector<unsigned char> magnitudes(blocks * 4 * bins);
    std::array<float, nfft> history{}, input{}, window{};
    std::array<std::array<float, 2>, nfft / 2 + 1> spectrum{};
    fftwf_plan plan;
    {
        std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex());
        plan = fftwf_plan_dft_r2c_1d(nfft, input.data(), reinterpret_cast<fftwf_complex *>(spectrum.data()), FFTW_ESTIMATE);
    }
    if (!plan) return {};
    constexpr double pi = 3.14159265358979323846;
    for (int i = 0; i < nfft; ++i) window[i] = static_cast<float>((1.0 - std::cos(2.0 * pi * i / nfft)) / nfft);
    int out = 0;
    for (int b = 0; b < blocks; ++b) {
        for (int sub = 0; sub < 2; ++sub) {
            std::move(history.begin() + hop, history.end(), history.begin());
            std::copy_n(audio.begin() + b * block + sub * hop, hop, history.end() - hop);
            for (int i = 0; i < nfft; ++i) input[i] = history[i] * window[i];
            fftwf_execute(plan);
            for (int fsub = 0; fsub < 2; ++fsub) {
                for (int bin = 0; bin < bins; ++bin) {
                    const auto &v = spectrum[bin * 2 + fsub];
                    const float db = 10.0f * std::log10(1e-12f + v[0] * v[0] + v[1] * v[1]);
                    magnitudes[out++] = static_cast<unsigned char>((std::clamp)(int(2 * db + 240), 0, 255));
                }
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(fftwPlannerGlobalMutex());
        fftwf_destroy_plan(plan);
    }
    ftx_waterfall_t wf{};
    wf.max_blocks = wf.num_blocks = blocks; wf.num_bins = bins;
    wf.time_osr = wf.freq_osr = 2; wf.block_stride = 4 * bins;
    wf.mag = magnitudes.data(); wf.protocol = FTX_PROTOCOL_FT4;
    std::array<ftx_candidate_t, 40> candidates{};
    const int found = ftx_find_candidates(&wf, int(candidates.size()), candidates.data(), 8);
    for (auto it = recent.begin(); it != recent.end();) {
        if (samples - it.value() > 12000 * 4) it = recent.erase(it); else ++it;
    }
    QStringList result;
    for (int i = 0; i < found; ++i) {
        ftx_message_t message{}; ftx_decode_status_t status{};
        if (!ftx_decode_candidate(&wf, &candidates[i], 30, &message, &status)) continue;
        std::array<char, 128> text{}; ftx_message_offsets_t offsets{};
        if (ftx_message_decode(&message, nullptr, text.data(), &offsets) != FTX_MESSAGE_RC_OK) continue;
        const double hz = (candidates[i].freq_offset + candidates[i].freq_sub * 0.5) / FT4_SYMBOL_PERIOD;
        if (hz < 100.0 || hz > 3000.0) continue;
        const QString decoded = QString::fromLatin1(text.data());
        const QString key = decoded + QString::number(qRound(hz / 25.0));
        if (recent.contains(key)) continue;
        if (recent.size() >= 128) recent.erase(recent.begin());
        recent.insert(key, samples);
        result << QStringLiteral("[FT4 %1 Hz] %2").arg(hz, 0, 'f', 1).arg(decoded);
    }
    return result;
}

#pragma once

// Núcleo de processamento do LimpaVoz (sem dependência do JUCE, para poder ser testado sozinho).
// Cada canal tem sua própria instância de VoiceCleaner.
//
// Fluxo: áudio do host -> (reamostra para 48 kHz) -> RNNoise (frames de 480 amostras)
//        -> mistura com o sinal original atrasado (dry/wet) -> corte de graves -> ganho
//        -> (reamostra de volta para a taxa do host)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include "rnnoise.h"
}

namespace limpavoz
{

constexpr int kFrame = 480;           // tamanho do frame do RNNoise (10 ms a 48 kHz)
constexpr int kWetPrime = 480;        // zeros colocados na fila de saída do RNNoise
constexpr int kLatency48 = 1440;      // latência total do caminho a 48 kHz (medida em tests/test_core.cpp)
constexpr int kChunk = 1024;          // tamanho máximo de bloco processado de uma vez
constexpr int kMaxExpand = 8;         // razão máxima de reamostragem suportada (8x)
constexpr int kScratch = kChunk * kMaxExpand + 64;

// ---------------------------------------------------------------------------------------------
class Biquad
{
public:
    void setLowPass (double sampleRate, double freq, double q = 0.70710678)
    {
        const double w0 = 2.0 * 3.14159265358979323846 * freq / sampleRate;
        const double cw = std::cos (w0), sw = std::sin (w0), alpha = sw / (2.0 * q);
        set ((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    }

    void setHighPass (double sampleRate, double freq, double q = 0.70710678)
    {
        const double w0 = 2.0 * 3.14159265358979323846 * freq / sampleRate;
        const double cw = std::cos (w0), sw = std::sin (w0), alpha = sw / (2.0 * q);
        set ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    }

    void setBypass() { set (1, 0, 0, 1, 0, 0); }
    void reset() { z1 = z2 = 0; }

    float process (float x)
    {
        const double in = x;
        const double out = b0 * in + z1;
        z1 = b1 * in - a1 * out + z2;
        z2 = b2 * in - a2 * out;
        return (float) out;
    }

private:
    void set (double nb0, double nb1, double nb2, double na0, double na1, double na2)
    {
        b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0;
        a1 = na1 / na0; a2 = na2 / na0;
    }

    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
};

// ---------------------------------------------------------------------------------------------
// Reamostrador em fluxo (interpolação cúbica Catmull-Rom). Latência de ~2 amostras.
class StreamResampler
{
public:
    void prepare (double inRate, double outRate)
    {
        step = inRate / outRate;
        buf.clear();
        buf.reserve ((size_t) kChunk + 64);
        buf.assign (4, 0.0f);
        pos = 1.0;
    }

    void reset()
    {
        buf.assign (4, 0.0f);
        pos = 1.0;
    }

    // Retorna a quantidade de amostras escritas em out (no máximo maxOut).
    int process (const float* in, int n, float* out, int maxOut)
    {
        buf.insert (buf.end(), in, in + n);
        int count = 0;

        while ((int) pos + 2 < (int) buf.size() && count < maxOut)
        {
            const int i = (int) pos;
            const float t = (float) (pos - i);
            const float y0 = buf[(size_t) i - 1], y1 = buf[(size_t) i],
                        y2 = buf[(size_t) i + 1], y3 = buf[(size_t) i + 2];

            const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
            const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            const float c = -0.5f * y0 + 0.5f * y2;
            out[count++] = ((a * t + b) * t + c) * t + y1;
            pos += step;
        }

        const int drop = (int) pos - 1;
        if (drop > 0)
        {
            buf.erase (buf.begin(), buf.begin() + drop);
            pos -= drop;
        }
        return count;
    }

private:
    double step = 1.0, pos = 1.0;
    std::vector<float> buf;
};

// ---------------------------------------------------------------------------------------------
class VoiceCleaner
{
public:
    VoiceCleaner() = default;
    ~VoiceCleaner() { destroy(); }
    VoiceCleaner (const VoiceCleaner&) = delete;
    VoiceCleaner& operator= (const VoiceCleaner&) = delete;

    void prepare (double newHostRate)
    {
        hostRate = newHostRate;
        resampling = std::abs (hostRate - 48000.0) > 0.5;

        destroy();
        state = rnnoise_create (nullptr);

        down.prepare (hostRate, 48000.0);
        up.prepare (48000.0, hostRate);

        antiAlias1.setBypass();
        antiAlias2.setBypass();
        if (hostRate > 48000.5)
        {
            antiAlias1.setLowPass (hostRate, 21000.0, 0.54119610);
            antiAlias2.setLowPass (hostRate, 21000.0, 1.30656296);
        }

        lowCut.setBypass();
        lastLowCut = -1.0f;
        reset();
    }

    void reset()
    {
        fi = 0;
        std::fill (frameIn.begin(), frameIn.end(), 0.0f);
        std::fill (wetRing.begin(), wetRing.end(), 0.0f);
        wr = kWetPrime; rd = 0; wetCount = kWetPrime;
        std::fill (dryRing.begin(), dryRing.end(), 0.0f);
        dryPos = 0;
        down.reset(); up.reset();
        antiAlias1.reset(); antiAlias2.reset(); lowCut.reset();

        // fila de saída no taxa do host (folga para a reamostragem variar +-1 amostra)
        outFifo.assign (kOutFifoSize, 0.0f);
        ofRd = 0; ofWr = 0; ofCount = 0;
        for (int i = 0; i < kOutPrime; ++i) pushOut (0.0f);
    }

    // Latência total em amostras da taxa do host (aproximada quando há reamostragem).
    int getLatencySamples() const
    {
        if (! resampling) return kLatency48;
        return (int) std::lround (kLatency48 * hostRate / 48000.0) + kOutPrime + 4;
    }

    // mix: 0 = só o original, 1 = só a voz limpa. lowCutHz <= 20 desliga o filtro.
    void process (const float* in, float* out, int numSamples, float mix, float lowCutHz, float outGain)
    {
        if (state == nullptr) { std::memcpy (out, in, sizeof (float) * (size_t) numSamples); return; }

        updateLowCut (lowCutHz);
        mix = std::min (1.0f, std::max (0.0f, mix));

        int done = 0;
        while (done < numSamples)
        {
            const int n = std::min (kChunk, numSamples - done);
            processChunk (in + done, out + done, n, mix, outGain);
            done += n;
        }
    }

private:
    static constexpr int kOutFifoSize = 1 << 15;
    static constexpr int kOutPrime = 16;

    void destroy()
    {
        if (state != nullptr) { rnnoise_destroy (state); state = nullptr; }
    }

    void updateLowCut (float hz)
    {
        if (std::abs (hz - lastLowCut) < 0.01f) return;
        lastLowCut = hz;
        if (hz <= 20.5f) lowCut.setBypass();
        else lowCut.setHighPass (48000.0, (double) hz);
    }

    void pushOut (float v)
    {
        if (ofCount >= kOutFifoSize) return;
        outFifo[(size_t) ofWr] = v;
        ofWr = (ofWr + 1) & (kOutFifoSize - 1);
        ++ofCount;
    }

    void processChunk (const float* in, float* out, int n, float mix, float outGain)
    {
        const float* in48 = in;
        int n48 = n;

        if (resampling)
        {
            if (hostRate > 48000.5)
            {
                for (int i = 0; i < n; ++i)
                    tmpIn[(size_t) i] = antiAlias2.process (antiAlias1.process (in[i]));
            }
            else
            {
                std::memcpy (tmpIn.data(), in, sizeof (float) * (size_t) n);
            }

            n48 = down.process (tmpIn.data(), n, buf48.data(), kScratch);
            in48 = buf48.data();
        }

        for (int k = 0; k < n48; ++k)
        {
            const float x = in48[k];

            // original atrasado, alinhado com a saída do RNNoise
            const float dry = dryRing[(size_t) dryPos];
            dryRing[(size_t) dryPos] = x;
            if (++dryPos >= kLatency48) dryPos = 0;

            frameIn[(size_t) fi++] = x * 32768.0f;
            if (fi == kFrame)
            {
                rnnoise_process_frame (state, frameOut.data(), frameIn.data());
                for (int i = 0; i < kFrame; ++i)
                {
                    wetRing[(size_t) wr] = frameOut[(size_t) i] * (1.0f / 32768.0f);
                    wr = (wr + 1) & (kWetRingSize - 1);
                }
                wetCount += kFrame;
                fi = 0;
            }

            float wet = 0.0f;
            if (wetCount > 0)
            {
                wet = wetRing[(size_t) rd];
                rd = (rd + 1) & (kWetRingSize - 1);
                --wetCount;
            }

            const float y = lowCut.process (dry * (1.0f - mix) + wet * mix) * outGain;
            mix48[(size_t) k] = y;
        }

        if (! resampling)
        {
            std::memcpy (out, mix48.data(), sizeof (float) * (size_t) n);
            return;
        }

        const int m = up.process (mix48.data(), n48, tmpOut.data(), kScratch);
        for (int i = 0; i < m; ++i) pushOut (tmpOut[(size_t) i]);

        for (int i = 0; i < n; ++i)
        {
            if (ofCount > 0)
            {
                out[i] = outFifo[(size_t) ofRd];
                ofRd = (ofRd + 1) & (kOutFifoSize - 1);
                --ofCount;
            }
            else
            {
                out[i] = 0.0f;
            }
        }
    }

    static constexpr int kWetRingSize = 2048;

    DenoiseState* state = nullptr;
    double hostRate = 48000.0;
    bool resampling = false;

    StreamResampler down, up;
    Biquad antiAlias1, antiAlias2, lowCut;
    float lastLowCut = -1.0f;

    std::array<float, kFrame> frameIn {}, frameOut {};
    int fi = 0;

    std::array<float, kWetRingSize> wetRing {};
    int wr = 0, rd = 0, wetCount = 0;

    std::array<float, kLatency48> dryRing {};
    int dryPos = 0;

    std::array<float, kChunk> tmpIn {};
    std::array<float, kScratch> buf48 {}, mix48 {}, tmpOut {};

    std::vector<float> outFifo;
    int ofRd = 0, ofWr = 0, ofCount = 0;
};

} // namespace limpavoz

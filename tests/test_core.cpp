// Teste do núcleo do LimpaVoz (sem JUCE).
// Compilar (Linux/macOS):
//   gcc -O2 -mavx2 -mfma -c third_party/rnnoise/src/*.c -Ithird_party/rnnoise/include -Ithird_party/rnnoise/src
//   g++ -O2 -std=c++17 -mavx2 -mfma tests/test_core.cpp *.o -Ithird_party/rnnoise/include -ISource -o test_core

#include "VoiceCleaner.h"

#include <cstdio>
#include <cstdlib>
#include <random>

using namespace limpavoz;

// Sinal parecido com voz: harmônicos de 120 Hz com envelope silábico (pausas entre sílabas).
static void makeSignal (std::vector<float>& clean, std::vector<float>& noisy, double sr, double seconds, float noiseLevel)
{
    const int n = (int) (sr * seconds);
    clean.assign ((size_t) n, 0.0f);
    noisy.assign ((size_t) n, 0.0f);
    std::mt19937 rng (1234);
    std::normal_distribution<float> gauss (0.0f, 1.0f);

    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const double env = std::max (0.0, std::sin (2 * 3.14159265358979 * 2.0 * t)); // 2 sílabas/s
        const double f0 = 120.0 + 15.0 * std::sin (2 * 3.14159265358979 * 0.7 * t);
        double s = 0;
        for (int h = 1; h <= 25; ++h)
            s += std::sin (2 * 3.14159265358979 * f0 * h * t) / (double) h;
        clean[(size_t) i] = (float) (0.25 * env * s);
        noisy[(size_t) i] = clean[(size_t) i] + noiseLevel * gauss (rng);
    }
}

static int findLag (const std::vector<float>& a, const std::vector<float>& b, int maxLag, double& bestCorr)
{
    // lag em que b[n] ~ a[n - lag]
    int bestLag = 0;
    bestCorr = -1;
    const int start = (int) a.size() / 4, len = (int) a.size() / 2;
    double ea = 0;
    for (int i = 0; i < len; ++i) ea += (double) a[(size_t) start + i] * a[(size_t) start + i];

    for (int lag = 0; lag <= maxLag; ++lag)
    {
        double c = 0, eb = 0;
        for (int i = 0; i < len; ++i)
        {
            const double x = a[(size_t) (start + i - lag)];
            const double y = b[(size_t) (start + i)];
            c += x * y; eb += y * y;
        }
        const double corr = c / std::sqrt (ea * eb + 1e-20);
        if (corr > bestCorr) { bestCorr = corr; bestLag = lag; }
    }
    return bestLag;
}

static double rms (const std::vector<float>& v, size_t from, size_t to)
{
    double s = 0;
    for (size_t i = from; i < to; ++i) s += (double) v[i] * v[i];
    return std::sqrt (s / (double) (to - from + 1));
}

int main()
{
    int failures = 0;
    const double rates[] = { 48000.0, 44100.0, 96000.0 };

    for (double sr : rates)
    {
        std::vector<float> clean, noisy;
        makeSignal (clean, noisy, sr, 8.0, 0.02f);

        for (int pass = 0; pass < 2; ++pass)
        {
            const float mix = pass == 0 ? 0.0f : 1.0f;
            VoiceCleaner vc;
            vc.prepare (sr);

            std::vector<float> out (noisy.size());
            // blocos de tamanho irregular, como um host de verdade
            const int sizes[] = { 64, 100, 512, 37, 1024, 333, 2048 };
            size_t pos = 0; int si = 0;
            while (pos < noisy.size())
            {
                const int n = (int) std::min<size_t> ((size_t) sizes[si++ % 7], noisy.size() - pos);
                vc.process (noisy.data() + pos, out.data() + pos, n, mix, 20.0f, 1.0f);
                pos += (size_t) n;
            }

            bool finite = true;
            for (float v : out) if (! std::isfinite (v)) finite = false;

            double corr;
            const int lag = findLag (noisy, out, 4000, corr);
            const int reported = vc.getLatencySamples();

            std::printf ("sr=%6.0f mix=%.0f  latência medida=%4d reportada=%4d  correlação=%.4f  finito=%d\n",
                         sr, mix, lag, reported, corr, (int) finite);

            if (! finite) ++failures;
            if (std::abs (lag - reported) > 24) { std::printf ("  FALHA: latência diferente da reportada\n"); ++failures; }

            if (pass == 0 && corr < 0.99) { std::printf ("  FALHA: mix=0 deveria reproduzir o original\n"); ++failures; }

            if (pass == 1)
            {
                // compara o ruído nas pausas (envelope ~0) entre entrada e saída
                double inQuiet = 0, outQuiet = 0; size_t cnt = 0;
                for (size_t i = (size_t) sr; i < noisy.size(); ++i)
                {
                    const double t = (double) i / sr;
                    if (std::sin (2 * 3.14159265358979 * 2.0 * (t - 0.0)) < -0.3)
                    {
                        inQuiet += (double) noisy[i] * noisy[i];
                        outQuiet += (double) out[i] * out[i];
                        ++cnt;
                    }
                }
                const double reduction = 10.0 * std::log10 ((inQuiet + 1e-20) / (outQuiet + 1e-20));
                std::printf ("         redução de ruído nas pausas: %.1f dB\n", reduction);
                if (reduction < 10.0) { std::printf ("  FALHA: redução menor que 10 dB\n"); ++failures; }
                (void) cnt;
            }
        }
    }

    std::printf (failures == 0 ? "\nTODOS OS TESTES PASSARAM\n" : "\n%d FALHA(S)\n", failures);
    return failures == 0 ? 0 : 1;
}

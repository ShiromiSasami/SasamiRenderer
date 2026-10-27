#include "WindWaveSpectrum.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace SasamiEngine::Sample::Wave
{
    namespace
    {
        constexpr float PI = 3.14159265358979f;
        constexpr float GAMMA = 3.3f;
        constexpr int BIN_SAMPLES = 16;
        constexpr size_t DIRECTION_PATTERN_COUNT = 8;
        constexpr float SWELL_DIRECTION_OFFSETS[DIRECTION_PATTERN_COUNT] = { 0.0f, 18.0f, -24.0f, 40.0f, -35.0f, 55.0f, -50.0f, 70.0f };
        constexpr float RIPPLE_DIRECTION_OFFSETS[4] = { 8.0f, -12.0f, 20.0f, -6.0f };

        double JonswapDensity(double w, double wp, double alpha, double g)
        {
            const double sigma = w <= wp ? 0.07 : 0.09;
            const double d = w - wp;
            const double r = std::exp(-(d * d) / (2.0 * sigma * sigma * wp * wp));
            const double ratio = wp / w;
            return alpha * g * g / std::pow(w, 5.0) * std::exp(-1.25 * ratio * ratio * ratio * ratio) *
                   std::pow(static_cast<double>(GAMMA), r);
        }
    }

    std::vector<GerstnerWave> BuildWindWaves(const WindWaveSpectrumSettings& settings, WindWaveSpectrumInfo* info)
    {
        const double g = GerstnerWaveSet::GRAVITY;
        const double U = (std::max)(settings.windSpeed, 0.5f);
        const double F = (std::max)(settings.fetch, 100.0f);
        const double wp = 22.0 * std::cbrt(g * g / (U * F));
        const double alpha = 0.076 * std::pow(U * U / (F * g), 0.22);

        std::vector<GerstnerWave> waves;
        const uint32_t n = settings.swellWaves;
        double sumSquares = 0.0;
        if (n > 0)
        {
            const double wLo = settings.minFrequencyRatio * wp;
            const double wHi = settings.maxFrequencyRatio * wp;
            for (uint32_t i = 0; i < n; ++i)
            {
                const double e0 = wLo * std::pow(wHi / wLo, static_cast<double>(i) / n);
                const double e1 = wLo * std::pow(wHi / wLo, static_cast<double>(i + 1) / n);
                const double w = std::sqrt(e0 * e1);
                const double dw = e1 - e0;

                double energy = 0.0;
                for (int s = 0; s < BIN_SAMPLES; ++s)
                {
                    const double ws = e0 + dw * (s + 0.5) / BIN_SAMPLES;
                    energy += JonswapDensity(ws, wp, alpha, g);
                }
                energy *= dw / BIN_SAMPLES;

                const double a = std::sqrt(2.0 * energy);
                sumSquares += a * a * 0.5;

                GerstnerWave wave;
                wave.wavelength = static_cast<float>(2.0 * PI * g / (w * w));
                wave.amplitude = static_cast<float>(a);
                wave.directionDeg = settings.windDirectionDeg + SWELL_DIRECTION_OFFSETS[i % DIRECTION_PATTERN_COUNT];
                wave.steepness = (std::max)(0.7f - 0.1f * static_cast<float>(i), 0.25f);
                wave.gustResponse = 0.0f;
                waves.push_back(wave);
            }
        }

        const float windScale = static_cast<float>(U / 10.0);
        for (uint32_t j = 0; j < settings.rippleWaves; ++j)
        {
            GerstnerWave wave;
            wave.wavelength = settings.rippleWavelength * std::pow(0.72f, static_cast<float>(j));
            wave.amplitude = settings.rippleAmplitude * windScale * windScale * std::pow(0.7f, static_cast<float>(j));
            wave.directionDeg = settings.windDirectionDeg + RIPPLE_DIRECTION_OFFSETS[j % 4];
            wave.steepness = 0.3f;
            wave.gustResponse = 1.0f;
            waves.push_back(wave);
        }

        if (info)
        {
            info->peakOmega = static_cast<float>(wp);
            info->peakWavelength = static_cast<float>(2.0 * PI * g / (wp * wp));
            info->alpha = static_cast<float>(alpha);
            info->significantHeight = static_cast<float>(4.0 * std::sqrt(sumSquares));
            info->empiricalHeight = static_cast<float>(1.6e-3 * U * std::sqrt(F / g));
        }
        return waves;
    }

    uint32_t RunWindWaveSpectrumSelfTest(std::vector<std::string>* failures, uint32_t* total)
    {
        uint32_t passed = 0;
        uint32_t checks = 0;
        auto check = [&](bool ok, const std::string& what) {
            ++checks;
            if (ok)
            {
                ++passed;
            }
            else if (failures)
            {
                failures->push_back(what);
            }
        };

        const double g = GerstnerWaveSet::GRAVITY;

        // 1-2: default settings: peak wavelength from w_p, in the expected range for U=10, F=5000.
        WindWaveSpectrumSettings base;
        WindWaveSpectrumInfo baseInfo;
        const std::vector<GerstnerWave> baseWaves = BuildWindWaves(base, &baseInfo);
        {
            const double expected = 2.0 * PI * g / (static_cast<double>(baseInfo.peakOmega) * baseInfo.peakOmega);
            check(std::fabs(baseInfo.peakWavelength - expected) <= 1e-4 * expected, "peak wavelength != 2 pi g / wp^2");
            check(baseInfo.peakWavelength > 5.0f && baseInfo.peakWavelength < 15.0f, "peak wavelength outside 5..15 m");
        }

        // 3: bins carry most of the energy of the fetch-limited growth law.
        check(std::fabs(baseInfo.significantHeight - baseInfo.empiricalHeight) <= 0.35f * baseInfo.empiricalHeight,
              "significant height not within 35% of the empirical height");

        // 4-5: stronger wind -> longer and higher waves.
        {
            WindWaveSpectrumSettings stronger = base;
            stronger.windSpeed = 15.0f;
            WindWaveSpectrumInfo strongInfo;
            BuildWindWaves(stronger, &strongInfo);
            check(strongInfo.peakWavelength > baseInfo.peakWavelength, "U=15 peak wavelength not longer than U=10");
            check(strongInfo.significantHeight > baseInfo.significantHeight, "U=15 significant height not larger than U=10");
        }

        // 6: longer fetch -> longer peak wavelength.
        {
            WindWaveSpectrumSettings longer = base;
            longer.fetch = 20000.0f;
            WindWaveSpectrumInfo longInfo;
            BuildWindWaves(longer, &longInfo);
            check(longInfo.peakWavelength > baseInfo.peakWavelength, "fetch 20000 peak wavelength not longer than 5000");
        }

        // 7: directions follow the wind.
        {
            WindWaveSpectrumSettings turned = base;
            turned.windDirectionDeg = 90.0f;
            const std::vector<GerstnerWave> turnedWaves = BuildWindWaves(turned, nullptr);
            check(!turnedWaves.empty() && turnedWaves[0].directionDeg == 90.0f, "first wave direction != wind direction");
        }

        // 8-10: size, gust flags, finite positive amplitudes.
        {
            check(baseWaves.size() == static_cast<size_t>(base.swellWaves) + base.rippleWaves, "wave count != swell + ripples");
            bool flagsOk = true;
            bool amplitudesOk = true;
            for (size_t i = 0; i < baseWaves.size(); ++i)
            {
                const float expectedGust = i < base.swellWaves ? 0.0f : 1.0f;
                flagsOk = flagsOk && baseWaves[i].gustResponse == expectedGust;
                amplitudesOk = amplitudesOk && baseWaves[i].amplitude > 0.0f && std::isfinite(baseWaves[i].amplitude);
            }
            check(flagsOk, "gustResponse not 0 for swell / 1 for ripples");
            check(amplitudesOk, "amplitude not positive and finite");
        }

        if (total)
        {
            *total = checks;
        }
        return passed;
    }
}

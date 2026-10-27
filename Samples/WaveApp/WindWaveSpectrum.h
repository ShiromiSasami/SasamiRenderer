#pragma once

#include "GerstnerWaveSet.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Sample::Wave
{
    struct WindWaveSpectrumSettings
    {
        float windSpeed = 10.0f;        // m/s, U10
        float windDirectionDeg = 0.0f;  // waves travel with the wind
        float fetch = 5000.0f;          // m, distance over which the wind has blown on the water
        uint32_t swellWaves = 4;        // spectrum bins turned into Gerstner waves
        float minFrequencyRatio = 0.85f;   // first bin at 0.85 w_p
        float maxFrequencyRatio = 2.6f;    // last bin at 2.6 w_p (geometric spacing in between)
        // Short, gust-responsive ripples (cat's paws) on top of the swell. Their amplitude follows the
        // local gust factor (GerstnerWave::gustResponse = 1); the swell does not react to gusts.
        uint32_t rippleWaves = 2;
        float rippleWavelength = 2.2f;   // m, longest ripple; the next ones are x0.72 each
        float rippleAmplitude = 0.035f;  // m at windSpeed 10, scales with (U / 10)^2
    };

    struct WindWaveSpectrumInfo
    {
        float peakOmega = 0.0f;            // rad/s
        float peakWavelength = 0.0f;       // m, 2 pi g / w_p^2
        float alpha = 0.0f;                // JONSWAP Phillips constant
        float significantHeight = 0.0f;    // m, 4 sqrt(sum a_i^2 / 2) of the swell bins
        float empiricalHeight = 0.0f;      // m, fetch-limited Hs = 1.6e-3 U sqrt(F / g) (JONSWAP growth law)
    };

    // Fetch-limited JONSWAP spectrum (Hasselmann et al. 1973), parameterised as in Horvath 2015
    // ("Empirical directional wave spectra for computer graphics", DigiPro): w_p = 22 (g^2 / (U F))^(1/3),
    // alpha = 0.076 (U^2 / (F g))^0.22, gamma = 3.3, sigma = 0.07 (w <= w_p) / 0.09.
    // S(w) = alpha g^2 / w^5 exp(-5/4 (w_p / w)^4) gamma^r, r = exp(-(w - w_p)^2 / (2 sigma^2 w_p^2)).
    // Each bin becomes one Gerstner wave with a_i = sqrt(2 S(w_i) dw_i) and lambda = 2 pi g / w_i^2.
    // Directions spread around the wind by a fixed pattern (0, +18, -24, +40, ... degrees); there is
    // no directional spreading function and no random phase. Steepness falls 0.7, 0.6, 0.5, 0.4...
    std::vector<GerstnerWave> BuildWindWaves(const WindWaveSpectrumSettings& settings, WindWaveSpectrumInfo* info);

    // Checks: peak wavelength from w_p; Hs of the bins within 35% of the empirical growth law; stronger
    // wind / longer fetch -> longer, higher waves; directions follow the wind; ripples flagged gustResponse=1.
    uint32_t RunWindWaveSpectrumSelfTest(std::vector<std::string>* failures, uint32_t* total);
}

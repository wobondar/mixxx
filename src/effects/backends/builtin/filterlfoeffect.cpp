#include "effects/backends/builtin/filterlfoeffect.h"

#include <algorithm>
#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/sample.h"

namespace {
//  Depth-swept low-pass: three one-pole LP sections in series feeding three
//  one-pole HP sections inside a positive feedback loop. The LP path is the
//  output; the HP path, scaled by the resonance gain, creates a resonant
//  peak just below the moving corner. Unity DC gain at every resonance,
//  never self-oscillates (max loop gain < 0.75).
//
//  out  = y3 + kResoScale * g * y6
//  loop = x  + g * y6[n-1]
//
//  The corner frequency follows a 129-entry design table indexed by the
//  triangle LFO (0 = open). Design frequencies are clamped to 0.319*fs and
//  prewarped twice - the double prewarp is deliberate voicing, keep it.

//  Main (low-pass) and resonance (high-pass) design frequency tables,
//  index 0 = sweep fully open, 128 = fully closed.
constexpr double kMainFc[129] = {
        23718.55943, 21324.14879, 20217.59439, 19329.50077, 18558.2221,
        17864.32237, 17227.50817, 16635.65252, 16080.7935, 15557.3473,
        15061.18789, 14589.15801, 14138.76234, 13707.97597, 13295.11907,
        12898.78164, 12517.7508, 12150.97906, 11797.54926, 11456.64562,
        11127.54026, 10809.5827, 10502.17451, 10204.78214, 9916.911977,
        9638.108361, 9367.953519, 9106.059246, 8852.06346, 8605.634108,
        8366.454931, 8134.234069, 7908.694648, 7689.575458, 7476.633952,
        7269.641611, 7068.379353, 6872.640324, 6682.230105, 6496.966838,
        6316.669268, 6141.176222, 5970.32497, 5803.96811, 5641.961633,
        5484.165051, 5330.45081, 5180.692538, 5034.774379, 4892.577108,
        4753.99429, 4618.925256, 4487.263775, 4358.920714, 4233.799132,
        4111.81453, 3992.881343, 3876.919615, 3763.852729, 3653.605183,
        3546.106944, 3441.28688, 3339.081398, 3239.427895, 3142.262627,
        3047.529227, 2955.170128, 2865.132904, 2777.363901, 2691.814522,
        2608.434911, 2527.180187, 2448.004177, 2370.864369, 2295.719803,
        2222.530032, 2151.256734, 2081.862873, 2014.312873, 1948.572401,
        1884.60814, 1822.388177, 1761.882172, 1703.060151, 1645.893679,
        1590.355445, 1536.419044, 1484.058772, 1433.250583, 1383.970515,
        1336.196232, 1289.90565, 1245.07809, 1201.693099, 1159.73141,
        1119.174537, 1080.004578, 1042.204392, 1005.757781, 970.648529,
        936.8619124, 904.3835511, 873.1995904, 843.2966947, 814.6626031,
        787.2851812, 761.1529789, 736.2554103, 712.5821847, 690.1234897,
        668.8701338, 648.813523, 629.9455244, 612.2584997, 595.7453374,
        580.3993749, 566.2145056, 553.1850273, 541.3057686, 530.5719935,
        520.9794355, 512.5243138, 505.2032756, 499.0134259, 493.9523255,
        490.0179788, 487.208838, 485.523794, 484.9621866,
};
constexpr double kResoFc[129] = {
        16845.12953, 12823.23914, 11176.66196, 9966.794084, 8996.408433,
        8185.243291, 7490.24968, 6884.809587, 6350.969604, 5875.886016,
        5449.96163, 5065.802777, 4717.565814, 4400.537249, 4110.851333,
        3845.299803, 3601.180923, 3376.202882, 3168.402677, 2976.083103,
        2797.768988, 2632.17211, 2478.154391, 2334.715395, 2200.963822,
        2076.103854, 1959.424189, 1850.28508, 1748.109194, 1652.37651,
        1562.612983, 1478.389128, 1399.312273, 1325.023312, 1255.194539,
        1189.525238, 1127.737865, 1069.576907, 1014.807036, 963.2114151,
        914.5869522, 868.7483792, 825.5212294, 784.7452654, 746.2704626,
        709.9563489, 675.673432, 643.2997911, 612.7225545, 583.8344104,
        556.53651, 530.7364494, 506.3457645, 483.2838428, 461.4729491,
        440.8415953, 421.3218437, 402.850205, 385.3669935, 368.8157734,
        353.1438178, 338.3009465, 324.2405996, 310.918711, 298.2933157,
        286.3255325, 274.9783026, 264.2170725, 254.0088997, 244.3230988,
        235.1304072, 226.403597, 218.1166957, 210.2454427, 202.7669783,
        195.6596726, 188.90322, 182.4784896, 176.3674827, 170.5532544,
        165.019842, 159.7522477, 154.7364033, 149.9590236, 145.4076628,
        141.0706371, 136.9369698, 132.9963396, 129.2391185, 125.6562202,
        122.239184, 118.9800446, 115.8713865, 112.906235, 110.078097,
        107.380909, 104.8090012, 102.3570876, 100.0202569, 97.79389182,
        95.67374655, 93.65585585, 91.73653028, 89.91234026, 88.18013477,
        86.53696923, 84.98012631, 83.50711349, 82.11561717, 80.8035025,
        79.56881112, 78.4097496, 77.32467215, 76.31207381, 75.37058409,
        74.49895494, 73.6960597, 72.96087802, 72.29249702, 71.69010039,
        71.15296535, 70.68045914, 70.27203172, 69.92721403, 69.6456148,
        69.42691738, 69.27087785, 69.17732294, 69.14614945,
};

constexpr double kFcClamp = 0.319;      // keeps the second tan() finite
constexpr float kResoScale = 0.4f;      // HP branch level in the output
constexpr double kRgSlewSeconds = 0.0033333;
constexpr double kDetentLo = 124.0 / 255.0;
constexpr double kDetentHi = 131.0 / 255.0;
constexpr double kDetentGain = 255.0 / 248.0;

//  depth knob 0..100 -> internal v 0..1 (center detent at 0.5)
double convertDepth(double depth) {
    const double r = depth / 100.0;
    if (r <= 0.0) {
        return 0.0;
    }
    if (r >= 1.0) {
        return 1.0;
    }
    if (r >= kDetentLo && r <= kDetentHi) {
        return 0.5;
    }
    return (r < kDetentLo) ? r * kDetentGain
                           : r * kDetentGain - (kDetentGain - 1.0);
}

//  v -> dry level: raised-cosine crossfade, fully wet above v = 0.15625
float dryFor(double v) {
    if (v > 0.15625) {
        return 0.0f;
    }
    const double d = 0.5 + 0.5 * std::sin(M_PI_2 * (1.0 - 12.8 * v));
    return static_cast<float>(std::clamp(d, 0.0, 1.0));
}

//  v -> resonance gain: zero below the knee at v = 0.3984, then rises to
//  0.96. The step at the knee is intentional.
float rgFor(double v) {
    if (v < 0.3984) {
        return 0.0f;
    }
    const double s = 1.0 - v;
    return static_cast<float>(std::min(0.96, 0.96 * (1.0 - s * s)));
}

//  LFO-position resonance taper: mute the resonance near the open end of
//  the sweep (a resonant peak against a wide-open corner reads as whistle).
inline float resoTaper(float lfo) {
    if (lfo < 0.1f) {
        return 1e-4f;
    }
    if (lfo >= 0.2f) {
        return 1.0f;
    }
    const float t = 2.0f - 10.0f * lfo;
    return std::clamp(1.0f - 0.99990f * t * t * t, 1e-4f, 1.0f);
}

float lpCoef(double fc, double fs) {
    const double f = std::min(fc, fs * kFcClamp);
    const double fp = (fs / M_PI) * std::tan(M_PI * f / fs);
    const double w = std::tan(M_PI * fp / fs);
    return static_cast<float>(std::clamp(w / (1.0 + w), 0.0, 1.0));
}

float hpCoef(double fc, double fs) {
    const double f = std::min(fc, fs * kFcClamp);
    const double fp = (fs / M_PI) * std::tan(M_PI * f / fs);
    const double w = std::tan(M_PI * fp / fs);
    return static_cast<float>(std::clamp(1.0 / (1.0 + w), 0.0, 1.0));
}
} // namespace

// static
QString FilterLFOEffect::getId() {
    return "org.mixxx.effects.filterlfo";
}

// static
EffectManifestPointer FilterLFOEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Filter LFO"));
    pManifest->setShortName(QObject::tr("Filter LFO"));
    pManifest->setAuthor("Daedalus");
    pManifest->setVersion("2.0");
    pManifest->setDescription(QObject::tr(
            "A resonant low-pass sweep driven by a beat-anchored triangle LFO"));
    pManifest->setEffectRampsFromDry(true);

    EffectManifestParameterPointer rate = pManifest->addParameter();
    rate->setId("rate");
    rate->setName(QObject::tr("Rate"));
    rate->setShortName(QObject::tr("Rate"));
    rate->setDescription(QObject::tr(
            "Rate of the sweep\n"
            "16 beats - 1/16 beat per cycle if tempo is detected\n"
            "16 s - 1/16 s per cycle if no tempo is detected"));
    rate->setValueScaler(EffectManifestParameter::ValueScaler::Logarithmic);
    rate->setUnitsHint(EffectManifestParameter::UnitsHint::Beats);
    rate->setRange(1.0 / 16, 0.25, 16);

    EffectManifestParameterPointer depth = pManifest->addParameter();
    depth->setId("depth");
    depth->setName(QObject::tr("Depth"));
    depth->setShortName(QObject::tr("Depth"));
    depth->setDescription(QObject::tr(
            "Depth of the sweep: fades the effect in over the lower range, "
            "adds resonance over the upper range"));
    depth->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    depth->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    depth->setRange(0, 50, 100);

    EffectManifestParameterPointer trim = pManifest->addParameter();
    trim->setId("res_trim");
    trim->setName(QObject::tr("Res Trim"));
    trim->setShortName(QObject::tr("Trim"));
    trim->setDescription(QObject::tr(
            "Scales the resonance of the sweep (1 = calibrated)"));
    trim->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    trim->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    trim->setRange(0.0, 1.0, 2.0);

    return pManifest;
}

void FilterLFOEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pRateParameter = parameters.value("rate");
    m_pDepthParameter = parameters.value("depth");
    m_pTrimParameter = parameters.value("res_trim");
}

void FilterLFOEffect::processChannel(
        FilterLFOGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    const double sampleRate = engineParameters.sampleRate();
    const int channelCount = engineParameters.channelCount();
    const SINT samplesTotal = engineParameters.samplesPerBuffer();

    // (Re)build the coefficient tables when the sample rate changes.
    if (pState->tableRate != sampleRate) {
        for (int i = 0; i < FilterLFOGroupState::kTableSize; i++) {
            pState->mainTab[i] = lpCoef(kMainFc[i], sampleRate);
            pState->resoTab[i] = hpCoef(kResoFc[i], sampleRate);
        }
        pState->tableRate = sampleRate;
    }

    const double v = convertDepth(m_pDepthParameter->value());
    const float dry = dryFor(v);
    const float trim = static_cast<float>(m_pTrimParameter->value());
    const float rgNow = std::min(rgFor(v) * trim, 0.999f);

    if (enableState == EffectEnableState::Enabling) {
        pState->clearFilters();
        pState->rg = rgNow;
        pState->rgTarget = rgNow;
        pState->rgStep = 0;
        pState->prevDry = dry;
    } else if (rgNow != pState->rgTarget) {
        // Linear slew to the new resonance gain over a fixed time.
        pState->rgTarget = rgNow;
        pState->rgStep = static_cast<float>(
                (rgNow - pState->rg) / (kRgSlewSeconds * sampleRate));
    }

    const double rate = m_pRateParameter->value();
    const GroupFeatureState& gf = groupFeatures;
    double framePerBeat = 0;
    double framePerPeriod;
    if (gf.beat_length.has_value() && gf.beat_fraction_buffer_end.has_value() &&
            rate > 0) {
        // rate is cycles per beat
        framePerBeat = gf.beat_length->seconds * sampleRate;
        framePerPeriod = framePerBeat / rate;
    } else if (rate > 0) {
        // rate is cycles per second
        framePerPeriod = sampleRate / rate;
    } else {
        framePerPeriod = sampleRate;
    }
    if (framePerPeriod < 1) {
        framePerPeriod = 1;
    }

    double currentFrame = pState->currentFrame;
    if (enableState == EffectEnableState::Enabling) {
        // Anchor the sweep to the beat grid at the moment of engagement:
        // the cycle starts on the previous beat boundary. Multi-beat
        // cycles free-run from there (the grid exposes no bar phase).
        currentFrame = framePerBeat > 0
                ? fmod(*gf.beat_fraction_buffer_end * framePerBeat,
                          framePerPeriod)
                : 0;
    } else if (framePerBeat > 0 && rate >= 1.0) {
        // Sub-beat cycles re-anchor every buffer so rate changes and long
        // sessions can never drift off the grid (same fix as TremoloV2).
        double startFrame = *gf.beat_fraction_buffer_end * framePerBeat -
                engineParameters.framesPerBuffer();
        startFrame = fmod(startFrame, framePerBeat);
        if (startFrame < 0) {
            startFrame += framePerBeat;
        }
        currentFrame = fmod(startFrame, framePerPeriod);
    }

    // Per-buffer linear ramp for the dry/wet crossfade.
    const float dryStart = pState->prevDry;
    const float dryDelta = (dry - dryStart) /
            static_cast<float>(engineParameters.framesPerBuffer());

    float rg = pState->rg;
    const float rgTarget = pState->rgTarget;
    const float rgStep = pState->rgStep;

    int frame = 0;
    for (SINT i = 0; i < samplesTotal; i += channelCount) {
        const double position =
                fmod(currentFrame + frame, framePerPeriod) / framePerPeriod;
        // Triangle, 0 = fully open: descend for the first half-cycle.
        const float lfo = static_cast<float>(
                position < 0.5 ? position * 2.0 : 2.0 - position * 2.0);

        const float scaled = lfo * 128.0f;
        int idx = static_cast<int>(scaled);
        if (idx > 127) {
            idx = 127;
        }
        const float frac = scaled - static_cast<float>(idx);
        // Interpolate in coefficient space, not frequency space.
        const float a = pState->mainTab[idx] +
                (pState->mainTab[idx + 1] - pState->mainTab[idx]) * frac;
        const float b = pState->resoTab[idx] +
                (pState->resoTab[idx + 1] - pState->resoTab[idx]) * frac;
        const float cLP = 2.0f * a - 1.0f;
        const float cHP = 1.0f - 2.0f * b;

        if (rgStep != 0 && rg != rgTarget) {
            rg += rgStep;
            if ((rgStep > 0 && rg > rgTarget) ||
                    (rgStep < 0 && rg < rgTarget)) {
                rg = rgTarget;
            }
        }
        const float g = rg * resoTaper(lfo);
        const float dryNow = dryStart + dryDelta * frame;
        const float wetNow = 1.0f - dryNow;

        for (int c = 0; c < channelCount && c < FilterLFOGroupState::kMaxChannels;
                c++) {
            float* s = pState->state[c];
            const float x = pInput[i + c];
            const float w0 = x + g * s[6];
            const float y1 = a * (w0 + s[0]) - cLP * s[1];
            s[0] = w0;
            const float y2 = a * (y1 + s[1]) - cLP * s[2];
            s[1] = y1;
            const float y3 = a * (y2 + s[2]) - cLP * s[3];
            s[2] = y2;
            const float y4 = b * (y3 - s[3]) - cHP * s[4];
            s[3] = y3;
            const float y5 = b * (y4 - s[4]) - cHP * s[5];
            s[4] = y4;
            const float y6 = b * (y5 - s[5]) - cHP * s[6];
            s[5] = y5;
            s[6] = y6;
            pOutput[i + c] =
                    dryNow * x + wetNow * (y3 + kResoScale * g * y6);
        }
        frame++;
    }

    pState->currentFrame = fmod(currentFrame + frame, framePerPeriod);
    pState->rg = rg;
    pState->prevDry = dry;

    if (enableState == EffectEnableState::Disabling) {
        // Crossfade back to dry over this buffer and reset for re-enable.
        SampleUtil::linearCrossfadeBuffersOut(
                pOutput, pInput, samplesTotal, channelCount);
        pState->clearFilters();
        pState->currentFrame = 0;
    }
}

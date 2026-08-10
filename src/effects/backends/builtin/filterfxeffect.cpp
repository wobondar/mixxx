#include "effects/backends/builtin/filterfxeffect.h"

#include <algorithm>
#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/sample.h"

namespace {
//  Beat-swept resonant filter: three one-pole sections in series feeding
//  three opposite-polarity sections inside a positive feedback loop. The
//  first branch is the output; the second, scaled by the resonance gain,
//  creates a resonant peak near the moving corner. Unity passband gain at
//  every resonance, never self-oscillates (max loop gain < 0.75).
//
//  out  = y3 + kResoScale * g * y6
//  loop = x  + g * y6[n-1]
//
//  The corner frequency follows a 129-entry design table indexed by the
//  triangle LFO (0 = open). Tone switches the sweep between the low-pass
//  voicing (LP main / HP resonance branch) and its high-pass mirror; tone
//  and phase changes are applied inside a brief output mute so the
//  topology never switches audibly. A secondary, faster triangle can
//  wobble one channel's sweep position for stereo movement.
//  Design frequencies are clamped to 0.319*fs and prewarped twice - the
//  double prewarp is deliberate voicing, keep it.

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

//  High-pass-mode design tables: main sections become high-pass and the
//  resonance branch low-pass - the mirror voicing of the tables above.
//  Index 0 = transparent, 128 = maximum effect, same direction. The
//  resonance table is exactly 4x the main table at every index.
constexpr double kHpfMainFc[129] = {
        51.849, 61.537496, 70.461271, 78.710817, 86.371503,
        93.523779, 100.243373, 106.601488, 112.664999, 118.496644,
        124.155207, 129.69571, 135.169591, 140.624883, 146.106393,
        151.655872, 157.312188, 163.111493, 169.087386, 175.271073,
        181.691531, 188.375657, 195.348425, 202.633032, 210.251047,
        218.222555, 226.566295, 235.2998, 244.439529, 254.001002,
        263.998925, 274.447321, 285.359645, 296.748911, 308.627805,
        321.008801, 333.904269, 347.326585, 361.288235, 375.801919,
        390.880646, 406.537834, 422.787401, 439.643854, 457.122379,
        475.238922, 494.010274, 513.454144, 533.589239, 554.435333,
        576.013339, 598.345372, 621.454814, 645.366375, 670.106151,
        695.701674, 722.18197, 749.577601, 777.920714, 807.245084,
        837.586152, 868.981061, 901.468691, 935.08969, 969.8865,
        1005.903382, 1043.186439, 1081.783634, 1121.744803, 1163.121675,
        1205.967872, 1250.338925, 1296.292272, 1343.88726, 1393.185146,
        1444.249088, 1497.144139, 1551.937235, 1608.697181, 1667.494637,
        1728.402093, 1791.493851, 1856.845996, 1924.536367, 1994.644529,
        2067.251734, 2142.440883, 2220.296491, 2300.904636, 2384.352917,
        2470.730403, 2560.127582, 2652.6363, 2748.34971, 2847.362206,
        2949.769359, 3055.667848, 3165.155394, 3278.330683, 3395.293291,
        3516.143603, 3640.982734, 3769.91244, 3903.035031, 4040.453281,
        4182.270334, 4328.589603, 4479.514674, 4635.149202, 4795.596804,
        4960.960948, 5131.344847, 5306.851336, 5487.582762, 5673.640857,
        5865.126616, 6062.140172, 6264.780663, 6473.1461, 6687.333235,
        6907.437416, 7133.552448, 7365.77045, 7604.181706, 7848.874513,
        8099.935028, 8357.447114, 8621.492175, 8892.149,
};
constexpr double kHpfResoFc[129] = {
        207.396, 246.149986, 281.845084, 314.843268, 345.486014,
        374.095117, 400.97349, 426.40595, 450.659997, 473.986575,
        496.620829, 518.782842, 540.678365, 562.499532, 584.42557,
        606.623487, 629.248753, 652.445973, 676.349543, 701.084293,
        726.766125, 753.502629, 781.393699, 810.532127, 841.004189,
        872.890221, 906.265182, 941.1992, 977.758117, 1016.004007,
        1055.995702, 1097.789282, 1141.438578, 1186.995642, 1234.51122,
        1284.035203, 1335.617075, 1389.306338, 1445.15294, 1503.207675,
        1563.522585, 1626.151337, 1691.149603, 1758.575415, 1828.489514,
        1900.955688, 1976.041095, 2053.816576, 2134.356956, 2217.741333,
        2304.053356, 2393.381486, 2485.819255, 2581.465501, 2680.424604,
        2782.806698, 2888.72788, 2998.310403, 3111.682855, 3228.980336,
        3350.344606, 3475.924242, 3605.874764, 3740.35876, 3879.546,
        4023.613529, 4172.745757, 4327.134535, 4486.979213, 4652.486698,
        4823.871488, 5001.355699, 5185.169086, 5375.549041, 5572.740586,
        5776.996354, 5988.576556, 6207.748938, 6434.788724, 6669.978547,
        6913.608372, 7165.975404, 7427.383983, 7698.145469, 7978.578117,
        8269.006935, 8569.763533, 8881.185963, 9203.618542, 9537.411667,
        9882.921613, 10240.51033, 10610.5452, 10993.39884, 11389.44883,
        11799.07743, 12222.67139, 12660.62158, 13113.32273, 13581.17316,
        14064.57441, 14563.93094, 15079.64976, 15612.14012, 16161.81313,
        16729.08134, 17314.35841, 17918.0587, 18540.59681, 19182.38721,
        19843.84379, 20525.37939, 21227.40534, 21950.33105, 22694.56343,
        23460.50646, 24248.56069, 25059.12265, 25892.5844, 26749.33294,
        27629.74966, 28534.20979, 29463.0818, 30416.72682, 31395.49805,
        32399.74011, 33429.78845, 34485.9687, 35568.596,
};

constexpr double kFcClamp = 0.319;      // keeps the second tan() finite
constexpr float kResoScale = 0.4f;      // resonance branch level in the output
constexpr double kRgSlewSeconds = 0.0033333;
constexpr double kGateSeconds = 0.0033333;  // change-gate fade time
constexpr double kSecFloorSeconds = 0.020;  // secondary-LFO period floor
constexpr double kDetentLo = 124.0 / 255.0;
constexpr double kDetentHi = 131.0 / 255.0;
constexpr double kDetentGain = 255.0 / 248.0;
constexpr double kHalfGain = 255.0 / 124.0; // detent-half normalization

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

//  resonance knob 0..1 -> loop gain rg: linear from zero up to the
//  calibrated value at the center detent, then a shallower linear rise
//  to the maximum at full
float rgFromResonance(double r) {
    if (r <= 0.0) {
        return 0.0f;
    }
    if (r >= 1.0) {
        return 0.96f;
    }
    if (r >= kDetentLo && r <= kDetentHi) {
        return 0.72f;
    }
    if (r < kDetentLo) {
        return static_cast<float>(
                0.72 * std::clamp(r * kHalfGain, 0.0, 1.0));
    }
    return static_cast<float>(0.72 +
            0.24 * std::clamp((r - kDetentHi) * kHalfGain, 0.0, 1.0));
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
QString FilterFXEffect::getId() {
    return "org.mixxx.effects.filterfx";
}

// static
EffectManifestPointer FilterFXEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Filter FX"));
    pManifest->setShortName(QObject::tr("Filter FX"));
    pManifest->setAuthor("Daedalus");
    pManifest->setVersion("1.0");
    pManifest->setDescription(QObject::tr(
            "A resonant filter sweep driven by a beat-anchored triangle LFO, "
            "with tone, phase and stereo wobble controls"));
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
            "Dry/wet mix: fades the effect in over the lower range of the "
            "knob, fully wet from there on"));
    depth->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    depth->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    depth->setRange(0, 100, 100);

    EffectManifestParameterPointer resonance = pManifest->addParameter();
    resonance->setId("resonance");
    resonance->setName(QObject::tr("Resonance"));
    resonance->setShortName(QObject::tr("Reso"));
    resonance->setDescription(QObject::tr(
            "Resonance of the sweep: the center detent is the calibrated "
            "default, rising to maximum at full"));
    resonance->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    resonance->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    resonance->setRange(0.0, 0.5, 1.0);

    EffectManifestParameterPointer lfoAmp = pManifest->addParameter();
    lfoAmp->setId("lfo_amp");
    lfoAmp->setName(QObject::tr("LFO Amp"));
    lfoAmp->setShortName(QObject::tr("Amp"));
    lfoAmp->setDescription(QObject::tr(
            "Adds a faster wobble to one channel's sweep: left of center "
            "wobbles the left channel, right of center the right; off at "
            "center"));
    lfoAmp->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    lfoAmp->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    lfoAmp->setRange(0.0, 0.5, 1.0);

    EffectManifestParameterPointer tone = pManifest->addParameter();
    tone->setId("tone");
    tone->setName(QObject::tr("Tone"));
    tone->setShortName(QObject::tr("Tone"));
    tone->setDescription(QObject::tr(
            "Character of the sweep: low-pass when off, high-pass when on"));
    tone->setValueScaler(EffectManifestParameter::ValueScaler::Toggle);
    tone->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    tone->setRange(0, 0, 1);

    EffectManifestParameterPointer phase = pManifest->addParameter();
    phase->setId("phase");
    phase->setName(QObject::tr("Phase"));
    phase->setShortName(QObject::tr("Phase"));
    phase->setDescription(QObject::tr(
            "Inverts the sweep cycle: starts closed and opens instead of "
            "starting open and closing"));
    phase->setValueScaler(EffectManifestParameter::ValueScaler::Toggle);
    phase->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    phase->setRange(0, 0, 1);

    return pManifest;
}

void FilterFXEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pRateParameter = parameters.value("rate");
    m_pDepthParameter = parameters.value("depth");
    m_pResonanceParameter = parameters.value("resonance");
    m_pLfoAmpParameter = parameters.value("lfo_amp");
    m_pToneParameter = parameters.value("tone");
    m_pPhaseParameter = parameters.value("phase");
}

void FilterFXEffect::processChannel(
        FilterFXGroupState* pState,
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
        for (int i = 0; i < FilterFXGroupState::kTableSize; i++) {
            pState->mainTabLp[i] = lpCoef(kMainFc[i], sampleRate);
            pState->resoTabLp[i] = hpCoef(kResoFc[i], sampleRate);
            pState->mainTabHp[i] = hpCoef(kHpfMainFc[i], sampleRate);
            pState->resoTabHp[i] = lpCoef(kHpfResoFc[i], sampleRate);
        }
        pState->tableRate = sampleRate;
    }

    const float dry = dryFor(convertDepth(m_pDepthParameter->value()));
    const float rgNow = std::min(
            rgFromResonance(m_pResonanceParameter->value()), 0.999f);
    const bool toneTarget = m_pToneParameter->toBool();
    const bool phaseTarget = m_pPhaseParameter->toBool();

    if (enableState == EffectEnableState::Enabling) {
        pState->clearFilters();
        pState->rg = rgNow;
        pState->rgTarget = rgNow;
        pState->rgStep = 0;
        pState->prevDry = dry;
        pState->hpfMode = toneTarget;
        pState->phaseInv = phaseTarget;
        pState->gateGain = 1.0f;
        pState->gateClosing = false;
    } else if (rgNow != pState->rgTarget) {
        // Linear slew to the new resonance gain over a fixed time.
        pState->rgTarget = rgNow;
        pState->rgStep = static_cast<float>(
                (rgNow - pState->rg) / (kRgSlewSeconds * sampleRate));
    }

    // A tone or phase switch mutes the output briefly, applies the change
    // at silence (flushing the filters on a tone switch), then fades back.
    if (!pState->gateClosing &&
            (toneTarget != pState->hpfMode ||
                    phaseTarget != pState->phaseInv)) {
        pState->gateClosing = true;
    }
    const float gateStep =
            static_cast<float>(1.0 / (kGateSeconds * sampleRate));

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

    // Secondary LFO: a faster beat-subdivision wobble summed into one
    // channel's sweep position. Off at the center detent, and off when
    // the main cycle is too fast for it to read as a sub-modulation.
    double secFramePeriod = 1;
    float secAmp = 0.0f;
    int secChannel = 0;
    {
        const double r = m_pLfoAmpParameter->value();
        if (r < kDetentLo || r > kDetentHi) {
            const double edgeDist =
                    (r < kDetentLo) ? kDetentLo - r : r - kDetentHi;
            const double c = std::clamp(edgeDist * kHalfGain, 0.0, 1.0);
            double subdiv;
            if (c < 0.2) {
                subdiv = 1.0;
            } else if (c < 0.4) {
                subdiv = 0.5;
            } else if (c < 0.6) {
                subdiv = 0.25;
            } else if (c < 0.8) {
                subdiv = 0.125;
            } else {
                subdiv = 0.0625;
            }
            // fall back to 120 BPM when no tempo is detected
            const double beatFrames =
                    framePerBeat > 0 ? framePerBeat : sampleRate * 0.5;
            secFramePeriod = std::max(
                    beatFrames * subdiv, kSecFloorSeconds * sampleRate);
            secAmp = static_cast<float>(
                    c < 0.6 ? 0.3 : 0.375 * c + 0.075);
            if (framePerPeriod < 2.0 * secFramePeriod) {
                secAmp = 0.0f;
            }
            secChannel = (r < kDetentLo) ? 0 : 1;
        }
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
    float gate = pState->gateGain;

    int frame = 0;
    for (SINT i = 0; i < samplesTotal; i += channelCount) {
        // Change gate: fade to silence, switch at the bottom, fade back.
        if (pState->gateClosing) {
            gate -= gateStep;
            if (gate <= 0.0f) {
                gate = 0.0f;
                if (pState->hpfMode != toneTarget) {
                    pState->hpfMode = toneTarget;
                    pState->clearFilters();
                }
                pState->phaseInv = phaseTarget;
                pState->gateClosing = false;
            }
        } else if (gate < 1.0f) {
            gate = std::min(gate + gateStep, 1.0f);
        }

        const double position =
                fmod(currentFrame + frame, framePerPeriod) / framePerPeriod;
        // Triangle, 0 = fully open: descend for the first half-cycle.
        const float tri = static_cast<float>(
                position < 0.5 ? position * 2.0 : 2.0 - position * 2.0);
        const float lfoMain = pState->phaseInv ? 1.0f - tri : tri;

        float sec = 0.0f;
        if (secAmp != 0.0f) {
            const double sp = fmod(currentFrame + frame, secFramePeriod) /
                    secFramePeriod;
            sec = secAmp * static_cast<float>(
                    sp < 0.5 ? 4.0 * sp - 1.0 : 3.0 - 4.0 * sp);
        }

        if (rgStep != 0 && rg != rgTarget) {
            rg += rgStep;
            if ((rgStep > 0 && rg > rgTarget) ||
                    (rgStep < 0 && rg < rgTarget)) {
                rg = rgTarget;
            }
        }

        const bool hpf = pState->hpfMode;
        const float* mainTab = hpf ? pState->mainTabHp : pState->mainTabLp;
        const float* resoTab = hpf ? pState->resoTabHp : pState->resoTabLp;
        const float dryNow = dryStart + dryDelta * frame;
        const float wetNow = 1.0f - dryNow;

        for (int c = 0; c < channelCount && c < FilterFXGroupState::kMaxChannels;
                c++) {
            const float lfo = std::clamp(
                    lfoMain + (c == secChannel ? sec : 0.0f), 0.0f, 1.0f);
            const float scaled = lfo * 128.0f;
            int idx = static_cast<int>(scaled);
            if (idx > 127) {
                idx = 127;
            }
            const float frac = scaled - static_cast<float>(idx);
            // Interpolate in coefficient space, not frequency space.
            const float a = mainTab[idx] +
                    (mainTab[idx + 1] - mainTab[idx]) * frac;
            const float b = resoTab[idx] +
                    (resoTab[idx + 1] - resoTab[idx]) * frac;
            const float g = rg * resoTaper(lfo);

            float* s = pState->state[c];
            const float x = pInput[i + c];
            const float w0 = x + g * s[6];
            float y3;
            float y6;
            if (!hpf) {
                // low-pass main sections, high-pass resonance branch
                const float cA = 2.0f * a - 1.0f;
                const float cB = 1.0f - 2.0f * b;
                const float y1 = a * (w0 + s[0]) - cA * s[1];
                s[0] = w0;
                const float y2 = a * (y1 + s[1]) - cA * s[2];
                s[1] = y1;
                y3 = a * (y2 + s[2]) - cA * s[3];
                s[2] = y2;
                const float y4 = b * (y3 - s[3]) - cB * s[4];
                s[3] = y3;
                const float y5 = b * (y4 - s[4]) - cB * s[5];
                s[4] = y4;
                y6 = b * (y5 - s[5]) - cB * s[6];
                s[5] = y5;
                s[6] = y6;
            } else {
                // high-pass main sections, low-pass resonance branch
                const float cA = 1.0f - 2.0f * a;
                const float cB = 2.0f * b - 1.0f;
                const float y1 = a * (w0 - s[0]) - cA * s[1];
                s[0] = w0;
                const float y2 = a * (y1 - s[1]) - cA * s[2];
                s[1] = y1;
                y3 = a * (y2 - s[2]) - cA * s[3];
                s[2] = y2;
                const float y4 = b * (y3 + s[3]) - cB * s[4];
                s[3] = y3;
                const float y5 = b * (y4 + s[4]) - cB * s[5];
                s[4] = y4;
                y6 = b * (y5 + s[5]) - cB * s[6];
                s[5] = y5;
                s[6] = y6;
            }
            pOutput[i + c] = gate *
                    (dryNow * x + wetNow * (y3 + kResoScale * g * y6));
        }
        frame++;
    }

    pState->currentFrame = fmod(currentFrame + frame, framePerPeriod);
    pState->rg = rg;
    pState->prevDry = dry;
    pState->gateGain = gate;

    if (enableState == EffectEnableState::Disabling) {
        // Crossfade back to dry over this buffer and reset for re-enable.
        SampleUtil::linearCrossfadeBuffersOut(
                pOutput, pInput, samplesTotal, channelCount);
        pState->clearFilters();
        pState->currentFrame = 0;
    }
}

#include "effects/backends/builtin/panfxeffect.h"

#include <algorithm>
#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/sample.h"

namespace {
//  Beat-locked auto-pan: two triangle LFOs half a cycle apart drive a
//  per-channel VCA on a linear constant-mono-sum pan law. The channel
//  gains run 0 .. 1.4125 (+3.0 dB hard-panned, -3.03 dB at the center
//  crossing) and always sum to 1.4125, so the mono sum never pumps.
//  One LFO period = two rate periods: "rate N" pans hard-left to
//  hard-right in 1/N beats and back in the next 1/N.
//
//  The signal is split into three bands (4th-order Butterworth corners,
//  two cascaded Q = 0.7071 biquads per corner) and the pan is applied
//  only to the enabled bands; disabled bands pass through unpanned.
//  Band routing: LOW CUT removes the pan from the mid band, HI CUT
//  removes it from the high AND low bands (both toggles = no pan at
//  all). Deliberate voicing - keep it.

constexpr double kLowCorner = 284.0;
constexpr double kHighCorner = 4752.0;
constexpr double kBiquadQ = 0.70710678;
constexpr double kAmpMax = 1.4125; // +3.0 dB gain ceiling
constexpr double kBandSlewSeconds = 0.0033333;
//  Asymmetric VCA slew, same values as TremoloV2.
constexpr double kMaxGainIncrementUp = 0.0068;
constexpr double kMaxGainIncrementDown = 0.001;
//  Center-detent geometry shared with FilterFX.
constexpr double kDetentLo = 124.0 / 255.0;
constexpr double kDetentHi = 131.0 / 255.0;
constexpr double kDetentGain = 255.0 / 248.0;
constexpr double kHalfGain = 255.0 / 124.0;
//  Stereo-spread limit of the lfo knob, in LFO cycles per channel.
constexpr double kSpreadMax = 0.185;

//  depth knob 0..100 -> detent-corrected v in 0..1 (0.5 detent).
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

//  The lfo_amp ladder: raw knob position -> per-channel LFO period
//  ratios. Center = both channels at the nominal period; either side
//  speeds ONE channel up in 5 steps down to period/8.
void ampLadder(double c, double* pRatioL, double* pRatioR) {
    double rL = 1.0;
    double rR = 1.0;
    if (c < 0.05) {
        rL = 1.0 / 8.0;
    } else if (c < 0.15) {
        rL = 1.0 / 4.0;
    } else if (c < 0.25) {
        rL = 1.0 / 3.0;
    } else if (c < 0.35) {
        rL = 1.0 / 2.0;
    } else if (c < 0.45) {
        rL = 2.0 / 3.0;
    } else if (c < 0.55) {
        // center: plain pan
    } else if (c < 0.65) {
        rR = 2.0 / 3.0;
    } else if (c < 0.75) {
        rR = 1.0 / 2.0;
    } else if (c < 0.85) {
        rR = 1.0 / 3.0;
    } else if (c < 0.95) {
        rR = 1.0 / 4.0;
    } else {
        rR = 1.0 / 8.0;
    }
    *pRatioL = rL;
    *pRatioR = rR;
}

//  Triangle at phase p (0..1): -1 at the cycle boundary, +1 at the
//  half cycle.
inline double triRaw(double p) {
    return 1.0 - 4.0 * std::fabs(p - 0.5);
}

inline double wrap01(double p) {
    p = std::fmod(p, 1.0);
    return p < 0 ? p + 1.0 : p;
}
} // namespace

// static
QString PanFXEffect::getId() {
    return "org.mixxx.effects.panfx";
}

// static
EffectManifestPointer PanFXEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("PAN FX"));
    pManifest->setShortName(QObject::tr("PAN FX"));
    pManifest->setAuthor("The Mixxx Team + Daedalus");
    pManifest->setVersion("1.0");
    pManifest->setDescription(QObject::tr(
            "Bounces the signal between the left and right channels on the "
            "beat, with a constant mono sum"));

    EffectManifestParameterPointer rate = pManifest->addParameter();
    rate->setId("rate");
    rate->setName(QObject::tr("Rate"));
    rate->setShortName(QObject::tr("Rate"));
    rate->setDescription(QObject::tr(
            "Speed of the pan\n"
            "16 beats - 1/16 beat per side if tempo is detected\n"
            "1/16 Hz - 16 Hz if no tempo is detected"));
    rate->setValueScaler(
            EffectManifestParameter::ValueScaler::Logarithmic);
    rate->setUnitsHint(EffectManifestParameter::UnitsHint::Beats);
    rate->setRange(1.0 / 16, 0.25, 16);

    EffectManifestParameterPointer depth = pManifest->addParameter();
    depth->setId("depth");
    depth->setName(QObject::tr("Depth"));
    depth->setShortName(QObject::tr("Depth"));
    depth->setDescription(QObject::tr(
            "How far the signal moves out of the center\n"
            "Full pan from the knob center up"));
    depth->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    depth->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    depth->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    depth->setRange(0, 50, 100);

    EffectManifestParameterPointer lfoAmp = pManifest->addParameter();
    lfoAmp->setId("lfo_amp");
    lfoAmp->setName(QObject::tr("LFO Amp"));
    lfoAmp->setShortName(QObject::tr("LFO Amp"));
    lfoAmp->setDescription(QObject::tr(
            "Speeds up one channel's pan cycle in steps\n"
            "Left of center: the left channel, down to 1/8 of the period\n"
            "Right of center: the right channel"));
    lfoAmp->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    lfoAmp->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    lfoAmp->setRange(0, 0.5, 1);

    EffectManifestParameterPointer lfo = pManifest->addParameter();
    lfo->setId("lfo");
    lfo->setName(QObject::tr("LFO"));
    lfo->setShortName(QObject::tr("LFO"));
    lfo->setDescription(QObject::tr(
            "Stereo spread of the two pan cycles\n"
            "Center: hard ping-pong (half a cycle apart)\n"
            "Either end: nearly in phase - the pan becomes a tremolo"));
    lfo->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    lfo->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    lfo->setRange(0, 0.5, 1);

    EffectManifestParameterPointer lowCut = pManifest->addParameter();
    lowCut->setId("low_cut");
    lowCut->setName(QObject::tr("Low Cut"));
    lowCut->setShortName(QObject::tr("LowCut"));
    lowCut->setDescription(QObject::tr(
            "Removes the pan from the mid band (284 Hz - 4752 Hz)"));
    lowCut->setValueScaler(EffectManifestParameter::ValueScaler::Toggle);
    lowCut->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    lowCut->setRange(0, 0, 1);

    EffectManifestParameterPointer hiCut = pManifest->addParameter();
    hiCut->setId("hi_cut");
    hiCut->setName(QObject::tr("Hi Cut"));
    hiCut->setShortName(QObject::tr("HiCut"));
    hiCut->setDescription(QObject::tr(
            "Removes the pan from the high band (above 4752 Hz) and the "
            "low band (below 284 Hz)"));
    hiCut->setValueScaler(EffectManifestParameter::ValueScaler::Toggle);
    hiCut->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    hiCut->setRange(0, 0, 1);

    return pManifest;
}

void PanFXEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pRateParameter = parameters.value("rate");
    m_pDepthParameter = parameters.value("depth");
    m_pLfoAmpParameter = parameters.value("lfo_amp");
    m_pLfoParameter = parameters.value("lfo");
    m_pLowCutParameter = parameters.value("low_cut");
    m_pHiCutParameter = parameters.value("hi_cut");
}

namespace {
//  RBJ biquad design, bilinear with inherent prewarp.
void designBiquad(bool highPass,
        double fc,
        double fs,
        float* b0,
        float* b1,
        float* b2,
        float* a1,
        float* a2) {
    const double w0 = 2.0 * M_PI * fc / fs;
    const double cosw = std::cos(w0);
    const double sinw = std::sin(w0);
    const double alpha = sinw / (2.0 * kBiquadQ);
    const double a0 = 1.0 + alpha;
    double rb0, rb1, rb2;
    if (highPass) {
        rb0 = (1.0 + cosw) / 2.0;
        rb1 = -(1.0 + cosw);
        rb2 = rb0;
    } else {
        rb0 = (1.0 - cosw) / 2.0;
        rb1 = 1.0 - cosw;
        rb2 = rb0;
    }
    *b0 = static_cast<float>(rb0 / a0);
    *b1 = static_cast<float>(rb1 / a0);
    *b2 = static_cast<float>(rb2 / a0);
    *a1 = static_cast<float>(-2.0 * cosw / a0);
    *a2 = static_cast<float>((1.0 - alpha) / a0);
}

inline float biquadTick(PanFXGroupState* pState, int c, int b, int coef, float x) {
    const float y = pState->b0[coef] * x + pState->z1[c][b];
    pState->z1[c][b] = pState->b1[coef] * x -
            pState->a1[coef] * y + pState->z2[c][b];
    pState->z2[c][b] = pState->b2[coef] * x - pState->a2[coef] * y;
    return y;
}
} // namespace

void PanFXEffect::processChannel(
        PanFXGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    const double sampleRate = engineParameters.sampleRate();
    const int channelCount = engineParameters.channelCount();
    const SINT samplesTotal = engineParameters.samplesPerBuffer();

    if (pState->tableRate != sampleRate) {
        designBiquad(false, kLowCorner, sampleRate,
                &pState->b0[0], &pState->b1[0], &pState->b2[0],
                &pState->a1[0], &pState->a2[0]);
        designBiquad(true, kLowCorner, sampleRate,
                &pState->b0[1], &pState->b1[1], &pState->b2[1],
                &pState->a1[1], &pState->a2[1]);
        designBiquad(false, kHighCorner, sampleRate,
                &pState->b0[2], &pState->b1[2], &pState->b2[2],
                &pState->a1[2], &pState->a2[2]);
        designBiquad(true, kHighCorner, sampleRate,
                &pState->b0[3], &pState->b1[3], &pState->b2[3],
                &pState->a1[3], &pState->a2[3]);
        pState->tableRate = sampleRate;
    }

    //  Depth law: dry falls and wet rises together, both done at the
    //  knob center - from there the pan runs at full swing.
    const double v = convertDepth(m_pDepthParameter->value());
    const float dry = static_cast<float>(std::max(0.0, 1.0 - 2.0 * v));
    const float wet = static_cast<float>(std::min(1.0, 2.0 * v));

    //  Band gains: 1 = the band is panned, 0 = it passes through.
    float bandTarget[3];
    const bool lowCut = m_pLowCutParameter->toBool();
    const bool hiCut = m_pHiCutParameter->toBool();
    bandTarget[0] = hiCut ? 0.0f : 1.0f;  // high band
    bandTarget[1] = lowCut ? 0.0f : 1.0f; // mid band
    bandTarget[2] = hiCut ? 0.0f : 1.0f;  // low band
    const float bandStep =
            static_cast<float>(1.0 / (kBandSlewSeconds * sampleRate));

    //  Stereo spread offsets from the lfo knob.
    double offL = 0.0;
    double offR = 0.0;
    {
        const double r = m_pLfoParameter->value();
        if (r < kDetentLo || r > kDetentHi) {
            const double edgeDist =
                    (r < kDetentLo) ? kDetentLo - r : r - kDetentHi;
            const double c = std::clamp(edgeDist * kHalfGain, 0.0, 1.0);
            const double s = kSpreadMax * c;
            offL = (r < kDetentLo) ? -s : s;
            offR = -offL;
        }
    }

    double ratioL, ratioR;
    ampLadder(m_pLfoAmpParameter->value(), &ratioL, &ratioR);

    const double rate = m_pRateParameter->value();
    const GroupFeatureState& gf = groupFeatures;
    double framePerBeat = 0;
    double framePerPeriod;
    if (gf.beat_length.has_value() && gf.beat_fraction_buffer_end.has_value() &&
            rate > 0) {
        // rate is periods per beat; the LFO takes two periods per cycle
        framePerBeat = gf.beat_length->seconds * sampleRate;
        framePerPeriod = 2.0 * framePerBeat / rate;
    } else if (rate > 0) {
        framePerPeriod = 2.0 * sampleRate / rate;
    } else {
        framePerPeriod = sampleRate;
    }
    if (framePerPeriod < 1) {
        framePerPeriod = 1;
    }

    double currentFrame = pState->currentFrame;
    if (enableState == EffectEnableState::Enabling) {
        // Anchor the cycle to the beat grid at the moment of engagement:
        // the cycle starts on the previous beat boundary. Multi-beat
        // cycles free-run from there (the grid exposes no bar phase).
        currentFrame = framePerBeat > 0
                ? fmod(*gf.beat_fraction_buffer_end * framePerBeat,
                          framePerPeriod)
                : 0;
        pState->clearFilters();
        pState->gain[0] = 1.0;
        pState->gain[1] = 1.0;
        for (int b = 0; b < 3; b++) {
            pState->bandGain[b] = bandTarget[b];
        }
        pState->prevDry = dry;
        pState->prevWet = wet;
    } else if (framePerBeat > 0 && rate >= 2.0) {
        // Cycles no longer than a beat re-anchor every buffer so rate
        // changes and long sessions can never drift off the grid (same
        // fix as TremoloV2; the full cycle spans 2/rate beats).
        double startFrame = *gf.beat_fraction_buffer_end * framePerBeat -
                engineParameters.framesPerBuffer();
        startFrame = fmod(startFrame, framePerBeat);
        if (startFrame < 0) {
            startFrame += framePerBeat;
        }
        currentFrame = fmod(startFrame, framePerPeriod);
    }

    //  Per-buffer linear ramp for the dry/wet pair.
    const float dryStart = pState->prevDry;
    const float wetStart = pState->prevWet;
    const float dryDelta = (dry - dryStart) /
            static_cast<float>(engineParameters.framesPerBuffer());
    const float wetDelta = (wet - wetStart) /
            static_cast<float>(engineParameters.framesPerBuffer());

    const double framePerL = framePerPeriod * ratioL;
    const double framePerR = framePerPeriod * ratioR;

    int frame = 0;
    for (SINT i = 0; i < samplesTotal; i += channelCount) {
        for (int b = 0; b < 3; b++) {
            const float target = bandTarget[b];
            float g = pState->bandGain[b];
            if (g < target) {
                g = std::min(g + bandStep, target);
            } else if (g > target) {
                g = std::max(g - bandStep, target);
            }
            pState->bandGain[b] = g;
        }

        const double pos = currentFrame + frame;
        //  Left LFO leads at the half cycle (loud) on the cycle
        //  boundary, right at the bottom - half a cycle apart.
        const double phaseL = wrap01(pos / framePerL + 0.5 + offL);
        const double phaseR = wrap01(pos / framePerR + 0.0 + offR);

        const float dryNow = dryStart + dryDelta * frame;
        const float wetNow = wetStart + wetDelta * frame;

        for (int c = 0; c < channelCount && c < PanFXGroupState::kMaxChannels;
                c++) {
            const double raw = triRaw(c == 0 ? phaseL : phaseR);
            const double gainTarget = 0.5 * kAmpMax * (1.0 + raw);

            double gain = pState->gain[c];
            if (gainTarget > gain + kMaxGainIncrementUp) {
                gain += kMaxGainIncrementUp;
            } else if (gainTarget < gain - kMaxGainIncrementDown) {
                gain -= kMaxGainIncrementDown;
            } else {
                gain = gainTarget;
            }
            pState->gain[c] = gain;

            const float x = pInput[i + c];
            //  Three-band split: LOW = LP^2, HIGH = HP^2, MID = the
            //  band between (HP at the low corner into LP at the high).
            const float low = biquadTick(pState, c, 1, 0,
                    biquadTick(pState, c, 0, 0, x));
            const float high = biquadTick(pState, c, 3, 3,
                    biquadTick(pState, c, 2, 3, x));
            const float mid = biquadTick(pState, c, 7, 2,
                    biquadTick(pState, c, 6, 2,
                            biquadTick(pState, c, 5, 1,
                                    biquadTick(pState, c, 4, 1, x))));

            const float g0 = pState->bandGain[0];
            const float g1 = pState->bandGain[1];
            const float g2 = pState->bandGain[2];
            const float fx = g0 * high + g1 * mid + g2 * low;
            const float thru = (1.0f - g0) * high + (1.0f - g1) * mid +
                    (1.0f - g2) * low;
            const float panned = static_cast<CSAMPLE_GAIN>(gain) * fx + thru;

            pOutput[i + c] = dryNow * x + wetNow * panned;
        }
        frame++;
    }

    //  Wrap at TWO periods: every ladder ratio (1/8..2/3) completes a
    //  whole number of its own cycles in that span, so the wrap is
    //  phase-continuous on both channels (2/3 would jump at one period).
    pState->currentFrame = fmod(currentFrame + frame, 2.0 * framePerPeriod);
    pState->prevDry = dry;
    pState->prevWet = wet;
}

#include "effects/backends/builtin/phaserfxeffect.h"

#include <algorithm>
#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/sample.h"

namespace {
//  Beat-swept all-pass phaser: up to 60 first-order all-pass stages per
//  channel inside a signed feedback loop, swept by a beat-locked
//  triangle. The sweep interpolates the 90-degree frequency cubically
//  in the PREWARPED domain between 299.96 Hz and 8058.92 Hz - the
//  bilinear coefficient a = (2 - theta)/(2 + theta) then lands those
//  endpoints exactly at every sample rate. The sweep starts at the TOP
//  on the cycle boundary and reaches the bottom at the half cycle.
//
//  The feedback knob is bipolar by the tone switch: positive feedback
//  (peaks between the notches) with a 0..0.93 ladder, or negative
//  (deeper, shifted notches) with a 0..0.95 ladder that sits near 0.77
//  over most of its left half. A makeup gain rides the feedback so
//  extreme settings do not get louder; the feedback is also windowed
//  down 10% at the bottom of the sweep for low-frequency stability.
//  The dry/wet pair locks at 0.75/0.75 above a quarter of the depth
//  range - a phaser is a parallel blend, never fully wet.
//
//  Stage-count and LFO-inversion changes reallocate/reset the all-pass
//  state, so they apply inside a brief output mute (the same change
//  gate as FilterFX).

constexpr double kFcBottom = 299.9614563;
constexpr double kFcPeak = 8058.9213867;
constexpr double kGateSeconds = 0.0033333;
//  Center-detent geometry shared with FilterFX.
constexpr double kDetentLo = 124.0 / 255.0;
constexpr double kDetentHi = 131.0 / 255.0;
constexpr double kDetentGain = 255.0 / 248.0;
constexpr double kHalfGain = 255.0 / 124.0;

//  knob 0..1 -> detent-corrected value in 0..1 (0.5 at the detent).
double convertCentered(double r) {
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

//  knob 0..1 -> distance across the active half, 0 at the end stop /
//  detent edge that side runs from, 1 at the other end. Left half runs
//  0 at the end stop up to 1 at the detent; right half 0 at the detent.
double halfValue(double r) {
    if (r <= 0.0) {
        return 0.0;
    }
    if (r >= 1.0) {
        return 1.0;
    }
    return (r < kDetentLo)
            ? std::clamp(r * kHalfGain, 0.0, 1.0)
            : std::clamp((r - kDetentHi) * kHalfGain, 0.0, 1.0);
}

//  Feedback ladders. tone off: positive feedback, 0 at the left end
//  stop through +0.2667 at the detent to +0.93 full right. tone on:
//  negative, with a nearly flat -0.77 left half, -0.8 detent, -0.95
//  full right.
float feedbackFor(double r, bool tone) {
    const bool center = (r >= kDetentLo && r <= kDetentHi);
    const double h = halfValue(r);
    double fb;
    if (tone) {
        double m;
        if (center) {
            m = 0.8;
        } else if (r < kDetentLo) {
            m = (h < 0.1) ? 7.7 * h : 0.003333336 * h + 0.7696667;
        } else {
            m = 0.14999998 * h + 0.8;
        }
        fb = -m;
    } else {
        if (center) {
            fb = 0.26666659;
        } else if (r < kDetentLo) {
            fb = 0.26666659 * h;
        } else {
            fb = 0.66333342 * h + 0.26666659;
        }
    }
    return static_cast<float>(fb);
}

//  Feedback-dependent makeup gain, applied to dry AND wet.
float makeupFor(double r, bool tone, float fb) {
    if (tone) {
        const double u = convertCentered(r);
        if (u < 0.1) {
            return static_cast<float>(1.0 - 1.0874909 * u);
        }
        if (u < 0.5) {
            return 0.8912509f;
        }
        return static_cast<float>(1.1515446 - 0.5205872 * u);
    }
    const float a = std::fabs(fb);
    if (a > 0.8f) {
        return 3.6603353f - 3.3254192f * a;
    }
    return 1.0f;
}

//  Stage knob: bipolar stage count. 0 = 6 stages on both channels;
//  negative raises the LEFT channel's count to |v| (floor 6, forced
//  even), positive the right - the other channel stays at 6.
void stagesFor(double v, int* pStagesL, int* pStagesR) {
    int l = 6;
    int r = 6;
    int n = static_cast<int>(std::fabs(v));
    if (n > 6) {
        if (n & 1) {
            n++;
        }
        n = std::min(n, PhaserFXGroupState::kMaxStages);
        if (v < 0) {
            l = n;
        } else {
            r = n;
        }
    }
    *pStagesL = l;
    *pStagesR = r;
}

inline double warp(double fc, double fs) {
    return std::tan(M_PI * fc / fs) * fs / M_PI;
}
} // namespace

// static
QString PhaserFXEffect::getId() {
    return "org.mixxx.effects.phaserfx";
}

// static
EffectManifestPointer PhaserFXEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Phaser FX"));
    pManifest->setShortName(QObject::tr("Phaser FX"));
    pManifest->setAuthor("The Mixxx Team + Daedalus");
    pManifest->setVersion("1.0");
    pManifest->setDescription(QObject::tr(
            "Sweeps a deep all-pass notch comb through the signal on the "
            "beat"));

    EffectManifestParameterPointer rate = pManifest->addParameter();
    rate->setId("rate");
    rate->setName(QObject::tr("Rate"));
    rate->setShortName(QObject::tr("Rate"));
    rate->setDescription(QObject::tr(
            "Speed of the sweep\n"
            "64 beats - 1/16 beat per cycle if tempo is detected\n"
            "1/64 Hz - 16 Hz if no tempo is detected"));
    rate->setValueScaler(
            EffectManifestParameter::ValueScaler::Logarithmic);
    rate->setUnitsHint(EffectManifestParameter::UnitsHint::Beats);
    rate->setRange(1.0 / 64, 0.25, 16);

    EffectManifestParameterPointer depth = pManifest->addParameter();
    depth->setId("depth");
    depth->setName(QObject::tr("Depth"));
    depth->setShortName(QObject::tr("Depth"));
    depth->setDescription(QObject::tr(
            "Strength of the effect\n"
            "The blend locks at its deepest from a quarter of the range up"));
    depth->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    depth->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    depth->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    depth->setRange(0, 50, 100);

    EffectManifestParameterPointer feedback = pManifest->addParameter();
    feedback->setId("feedback");
    feedback->setName(QObject::tr("Feedback"));
    feedback->setShortName(QObject::tr("Feedback"));
    feedback->setDescription(QObject::tr(
            "Resonance of the notch comb\n"
            "Rises clockwise; the Tone switch flips its polarity"));
    feedback->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    feedback->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    feedback->setRange(0, 0.5, 1);

    EffectManifestParameterPointer stage = pManifest->addParameter();
    stage->setId("stages");
    stage->setName(QObject::tr("Stages"));
    stage->setShortName(QObject::tr("Stages"));
    stage->setDescription(QObject::tr(
            "All-pass stages per channel, rounded to even\n"
            "0: 6 stages on both channels\n"
            "Negative: raises the left channel's count to 60\n"
            "Positive: raises the right channel's count"));
    stage->setValueScaler(EffectManifestParameter::ValueScaler::Integral);
    stage->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    stage->setRange(-60, 0, 60);

    EffectManifestParameterPointer tone = pManifest->addParameter();
    tone->setId("tone");
    tone->setName(QObject::tr("Tone"));
    tone->setShortName(QObject::tr("Tone"));
    tone->setDescription(QObject::tr(
            "Flips the feedback polarity: negative feedback deepens the "
            "notches and darkens the sweep"));
    tone->setValueScaler(EffectManifestParameter::ValueScaler::Toggle);
    tone->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    tone->setRange(0, 0, 1);

    EffectManifestParameterPointer phase = pManifest->addParameter();
    phase->setId("phase");
    phase->setName(QObject::tr("Phase"));
    phase->setShortName(QObject::tr("Phase"));
    phase->setDescription(QObject::tr(
            "Inverts the sweep: the cycle starts at the bottom of the "
            "range instead of the top"));
    phase->setValueScaler(EffectManifestParameter::ValueScaler::Toggle);
    phase->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    phase->setRange(0, 0, 1);

    return pManifest;
}

void PhaserFXEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pRateParameter = parameters.value("rate");
    m_pDepthParameter = parameters.value("depth");
    m_pFeedbackParameter = parameters.value("feedback");
    m_pStagesParameter = parameters.value("stages");
    m_pToneParameter = parameters.value("tone");
    m_pPhaseParameter = parameters.value("phase");
}

void PhaserFXEffect::processChannel(
        PhaserFXGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    const double sampleRate = engineParameters.sampleRate();
    const int channelCount = engineParameters.channelCount();
    const SINT samplesTotal = engineParameters.samplesPerBuffer();

    if (pState->warpRate != sampleRate) {
        pState->bottomWarped = warp(kFcBottom, sampleRate);
        pState->peakWarped = warp(kFcPeak, sampleRate);
        pState->warpRate = sampleRate;
    }
    const double bottomW = pState->bottomWarped;
    const double peakW = pState->peakWarped;

    //  Depth law: dry falls slowly while wet rises three times as fast,
    //  meeting at 0.75/0.75 a quarter in - flat from there.
    const double v = convertCentered(m_pDepthParameter->value() / 100.0);
    float dry, wet;
    if (v < 0.25) {
        dry = static_cast<float>(1.0 - v);
        wet = static_cast<float>(3.0 * v);
    } else {
        dry = 0.75f;
        wet = 0.75f;
    }
    const double fbKnob = m_pFeedbackParameter->value();
    const bool tone = m_pToneParameter->toBool();
    const float fb = feedbackFor(fbKnob, tone);
    const float makeup = makeupFor(fbKnob, tone, fb);
    dry *= makeup;
    wet *= makeup;

    int stagesTargetL, stagesTargetR;
    stagesFor(m_pStagesParameter->value(), &stagesTargetL, &stagesTargetR);
    const bool phaseTarget = m_pPhaseParameter->toBool();

    if (enableState == EffectEnableState::Enabling) {
        pState->clearCore();
        pState->stages[0] = stagesTargetL;
        pState->stages[1] = stagesTargetR;
        pState->phaseInv = phaseTarget;
        pState->prevFb = fb;
        pState->prevDry = dry;
        pState->prevWet = wet;
        pState->gateGain = 1.0f;
        pState->gateClosing = false;
    }

    //  A stage or phase switch mutes the output briefly, applies the
    //  change at silence (resetting the all-pass state), then fades back.
    if (!pState->gateClosing &&
            (stagesTargetL != pState->stages[0] ||
                    stagesTargetR != pState->stages[1] ||
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

    //  Per-buffer linear ramps: the feedback glides through polarity
    //  flips (tone) without a gate, the dry/wet pair follows the depth
    //  knob and the makeup gain.
    const float fbStart = pState->prevFb;
    const float fbDelta = (fb - fbStart) /
            static_cast<float>(engineParameters.framesPerBuffer());
    const float dryStart = pState->prevDry;
    const float dryDelta = (dry - dryStart) /
            static_cast<float>(engineParameters.framesPerBuffer());
    const float wetStart = pState->prevWet;
    const float wetDelta = (wet - wetStart) /
            static_cast<float>(engineParameters.framesPerBuffer());

    float gate = pState->gateGain;

    int frame = 0;
    for (SINT i = 0; i < samplesTotal; i += channelCount) {
        // Change gate: fade to silence, switch at the bottom, fade back.
        if (pState->gateClosing) {
            gate -= gateStep;
            if (gate <= 0.0f) {
                gate = 0.0f;
                pState->stages[0] = stagesTargetL;
                pState->stages[1] = stagesTargetR;
                pState->phaseInv = phaseTarget;
                pState->clearCore();
                pState->gateClosing = false;
            }
        } else if (gate < 1.0f) {
            gate = std::min(gate + gateStep, 1.0f);
        }

        const double p =
                fmod(currentFrame + frame, framePerPeriod) / framePerPeriod;
        // Triangle from the top: u = 1 on the cycle boundary, 0 at the
        // half cycle.
        double u = std::fabs(1.0 - 2.0 * p);
        if (pState->phaseInv) {
            u = 1.0 - u;
        }

        // Cubic sweep in the prewarped domain, then the bilinear
        // all-pass coefficient - exact at both endpoints.
        const double fcW = bottomW + (peakW - bottomW) * u * u * u;
        const double theta = 2.0 * M_PI * fcW / sampleRate;
        const float a = static_cast<float>((2.0 - theta) / (2.0 + theta));

        // Low-frequency stability window on the feedback.
        const double om = 1.0 - u;
        const double om2 = om * om;
        const double om4 = om2 * om2;
        const float window = static_cast<float>(1.0 - 0.1 * om4 * om4);
        const float fbNow = (fbStart + fbDelta * frame) * window;

        const float dryNow = dryStart + dryDelta * frame;
        const float wetNow = wetStart + wetDelta * frame;

        for (int c = 0; c < channelCount && c < PhaserFXGroupState::kMaxChannels;
                c++) {
            const float in = pInput[i + c];
            float x = in + fbNow * pState->lastOut[c];
            float* state = pState->apState[c];
            const int stages = pState->stages[c];
            //  First-order all-pass H(z) = (a - z^-1)/(1 - a z^-1),
            //  one state and two multiplies per stage. Pole at z = a,
            //  stable across the whole sweep.
            for (int s = 0; s < stages; s++) {
                const float y = a * x + state[s];
                state[s] = a * y - x;
                x = y;
            }
            pState->lastOut[c] = x;
            pOutput[i + c] = gate * (dryNow * in + wetNow * x);
        }
        frame++;
    }

    pState->currentFrame = fmod(currentFrame + frame, framePerPeriod);
    pState->gateGain = gate;
    pState->prevFb = fb;
    pState->prevDry = dry;
    pState->prevWet = wet;
}

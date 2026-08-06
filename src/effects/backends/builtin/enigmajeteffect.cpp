#include "effects/backends/builtin/enigmajeteffect.h"

#include <algorithm>
#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"

namespace {
//  A four-tap barber-pole feedback flanger. Four delay lines sweep
//  0.1 - 2 ms, phase-staggered a quarter cycle apart, each faded by a
//  squared window around its wrap so the sweep never audibly turns
//  around. Left output = taps at phase 0 and 0.5 subtracted; right =
//  taps at 0.25 and 0.75 - the stereo image IS the phase stagger.
//  Everything below ~69 Hz is split off and passed through clean, so
//  the low end never flanges. High feedback stays bounded because the
//  window also multiplies the feedback path, forcibly emptying each
//  line around its wrap.

//  Band split: 2nd-order at 69.178 Hz, Q = 0.5 (critically damped).
constexpr double kSplitOmega = 434.655735174446; // 2*pi*69.178
//  Gain ramp rate: (target - current) * 300/fs per sample, with snap.
constexpr float kRampRate = 300.0f;
constexpr float kRampSnap = 0.00032f;
//  Fallback beat length when no tempo is available (120 BPM).
constexpr double kFallbackBeatMs = 500.0;

//  depth 0..100 -> dry/wet targets and the feedback endpoints F0/F1.
//  Knee at the knob center: below it the effect crossfades in with
//  rising feedback; above it the mix locks at 1:1 and only feedback
//  keeps climbing (0.7->0.9 at fb-knob center, 0.9->0.99 at max).
struct DepthLaw {
    float dry;
    float wet;
    float f0;
    float f1;
};

DepthLaw depthLaw(double depth) {
    const int L = static_cast<int>(depth / 100.0 * 1023.9);
    DepthLaw out;
    if (L <= 9) {
        out.wet = 0.0f;
        out.dry = 1.0f;
        out.f0 = 0.0f;
        out.f1 = 0.0f;
    } else if (L <= 511) {
        out.wet = 0.00099601597f * (L - 10);
        out.dry = 1.0f - out.wet;
        out.f0 = 0.0013944224f * (L - 10);
        out.f1 = 0.0017928288f * (L - 10);
    } else {
        out.wet = 0.5f;
        out.dry = 0.5f;
        out.f0 = 0.7f + 0.000390625f * (L - 512);
        out.f1 = 0.9f + 0.000175781f * (L - 512);
    }
    return out;
}

//  Modulation knob (0..1, center = off): a triangle vibrato on the
//  delay time, +-12.5% at full depth. Left of center: 2 -> 33 Hz;
//  right: 2 -> 49 Hz; both sides fade the depth in from the dead zone.
struct ModLaw {
    float depth;
    double phaseInc; // per sample; triangle wraps at 2
};

ModLaw modLaw(double knob, double fs) {
    const int m = static_cast<int>(std::clamp(knob, 0.0, 1.0) * 255.0);
    ModLaw out{0.0f, 0.0};
    double v;
    double a;
    if (m <= 125) {
        v = m;
        a = 4.0 - 0.0317460336 * m;
    } else if (m <= 130) {
        return out; // dead zone
    } else {
        v = m - 130;
        a = 0.0158730168 * v;
    }
    out.depth = 0.25f * static_cast<float>(std::min(a, 1.0));
    out.phaseInc = (0.0060468642 * v * v + 4.0) / fs;
    return out;
}
} // namespace

// static
QString EnigmaJetEffect::getId() {
    return "org.mixxx.effects.enigmajet";
}

// static
EffectManifestPointer EnigmaJetEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Enigma Jet"));
    pManifest->setShortName(QObject::tr("Enigma"));
    pManifest->setAuthor("Daedalus");
    pManifest->setVersion("1.0");
    pManifest->setDescription(QObject::tr(
            "An endlessly rising (or falling) barber-pole flanger"));
    pManifest->setEffectRampsFromDry(true);

    EffectManifestParameterPointer beats = pManifest->addParameter();
    beats->setId("beats");
    beats->setName(QObject::tr("Beats"));
    beats->setShortName(QObject::tr("Beats"));
    beats->setDescription(QObject::tr(
            "Sweep cycle length in beats; negative values fall instead "
            "of rise"));
    beats->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    beats->setUnitsHint(EffectManifestParameter::UnitsHint::Beats);
    beats->setRange(-64, 4, 64);

    EffectManifestParameterPointer depth = pManifest->addParameter();
    depth->setId("depth");
    depth->setName(QObject::tr("Depth"));
    depth->setShortName(QObject::tr("Depth"));
    depth->setDescription(QObject::tr(
            "Fades the effect in over the lower half, raises the "
            "feedback over the upper half"));
    depth->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    depth->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    depth->setRange(0, 50, 100);

    EffectManifestParameterPointer feedback = pManifest->addParameter();
    feedback->setId("feedback");
    feedback->setName(QObject::tr("Feedback"));
    feedback->setShortName(QObject::tr("Feedback"));
    feedback->setDescription(QObject::tr(
            "Comb feedback; center is the recommended value"));
    feedback->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    feedback->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    feedback->setRange(0.0, 0.5, 1.0);

    EffectManifestParameterPointer mod = pManifest->addParameter();
    mod->setId("mod");
    mod->setName(QObject::tr("Mod"));
    mod->setShortName(QObject::tr("Mod"));
    mod->setDescription(QObject::tr(
            "Delay-time vibrato: off at center, slow-deep to the left, "
            "fast to the right"));
    mod->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    mod->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    mod->setRange(0.0, 0.5, 1.0);

    EffectManifestParameterPointer freeze = pManifest->addParameter();
    freeze->setId("freeze");
    freeze->setName(QObject::tr("Freeze"));
    freeze->setShortName(QObject::tr("Freeze"));
    freeze->setDescription(QObject::tr(
            "Halts the sweep: a static resonant comb, audio keeps "
            "flowing"));
    freeze->setValueScaler(EffectManifestParameter::ValueScaler::Toggle);
    freeze->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    freeze->setRange(0, 0, 1);

    return pManifest;
}

void EnigmaJetEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pBeatsParameter = parameters.value("beats");
    m_pDepthParameter = parameters.value("depth");
    m_pFeedbackParameter = parameters.value("feedback");
    m_pModParameter = parameters.value("mod");
    m_pFreezeParameter = parameters.value("freeze");
}

void EnigmaJetEffect::processChannel(
        EnigmaJetGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    const double fs = engineParameters.sampleRate();
    const int channelCount = engineParameters.channelCount();
    const SINT samplesTotal = engineParameters.samplesPerBuffer();

    if (enableState == EffectEnableState::Enabling) {
        pState->clear();
    }

    const DepthLaw law = depthLaw(m_pDepthParameter->value());
    //  Feedback knob: two linear segments hinged at center, where the
    //  feedback is exactly F0. The top half only buys F1 - F0 extra.
    const double n = std::clamp(m_pFeedbackParameter->value(), 0.0, 1.0);
    const float fb = (n < 0.5)
            ? static_cast<float>(2.0 * n) * law.f0
            : law.f0 + static_cast<float>(2.0 * n - 1.0) * (law.f1 - law.f0);

    //  Signed beat length -> signed sweep cycle; sign = direction.
    const double beats = m_pBeatsParameter->value();
    const double beatMs = groupFeatures.beat_length.has_value()
            ? groupFeatures.beat_length->seconds * 1000.0
            : kFallbackBeatMs;
    double cycleMs = beats * beatMs;
    if (cycleMs == 0.0) {
        cycleMs = 1.0;
    }
    const double cycleSamples = cycleMs * 0.001 * fs;
    double inc = -1.0 / cycleSamples;
    if (m_pFreezeParameter->toBool()) {
        inc = 0.0;
    }
    const bool rising = cycleSamples > 0;

    const ModLaw mod = modLaw(m_pModParameter->value(), fs);

    //  Band-split coefficients (bilinear, both bands from one biquad).
    const double t = kSplitOmega / fs;
    const double g = 1.0 / (4.0 + 4.0 * t + t * t);
    const float b0 = static_cast<float>(4.0 * g);
    const float b1 = static_cast<float>(-8.0 * g);
    const float b2 = static_cast<float>(4.0 * g);
    const float a1 = static_cast<float>((2.0 * t * t - 8.0) * g);
    const float a2 = static_cast<float>((4.0 - 4.0 * t + t * t) * g);
    const float lowScale = static_cast<float>(1.0 + a1 + a2);

    const float rampInc = kRampRate / static_cast<float>(fs);
    const int minDelay = pState->minDelay;
    const int range = pState->range;
    const int bufLen = pState->bufLen;

    double phase = pState->phase;
    double modPhase = pState->modPhase;
    float dryGain = pState->dryGain;
    float wetGain = pState->wetGain;
    int wr = pState->writeIdx;

    for (SINT i = 0; i < samplesTotal; i += channelCount) {
        //  Gain ramps toward targets.
        dryGain += (law.dry - dryGain) * rampInc;
        if (std::fabs(law.dry - dryGain) < kRampSnap) {
            dryGain = law.dry;
        }
        wetGain += (law.wet - wetGain) * rampInc;
        if (std::fabs(law.wet - wetGain) < kRampSnap) {
            wetGain = law.wet;
        }

        //  Modulation triangle in [0,1], multiplier 1 +- depth/2.
        float modMul = 1.0f;
        if (mod.depth > 0.0f) {
            modPhase += mod.phaseInc;
            if (modPhase >= 2.0) {
                modPhase -= 2.0;
            }
            const float tri = static_cast<float>(
                    modPhase < 1.0 ? modPhase : 2.0 - modPhase);
            modMul = 1.0f + mod.depth * (tri - 0.5f);
        }

        //  Band split, both channels.
        float high[2];
        float low[2];
        for (int c = 0; c < 2; c++) {
            const float x = pInput[i + std::min(c, channelCount - 1)];
            const float w0 = x - a1 * pState->w1[c] - a2 * pState->w2[c];
            low[c] = lowScale * w0;
            high[c] = b0 * w0 + b1 * pState->w1[c] + b2 * pState->w2[c];
            pState->w2[c] = pState->w1[c];
            pState->w1[c] = w0;
        }

        //  Four phase-staggered windowed taps.
        float tap[4];
        for (int k = 0; k < 4; k++) {
            double pk = phase + k * 0.25;
            pk -= std::floor(pk);
            float w;
            if (pk < 0.1) {
                const float u = static_cast<float>(10.0 * pk);
                w = u * u;
            } else if (pk > 0.9) {
                const float u = static_cast<float>(10.0 - 10.0 * pk);
                w = u * u;
            } else {
                w = 1.0f;
            }
            double d = (minDelay + pk * range) * modMul;
            d = std::clamp(d, static_cast<double>(minDelay),
                    static_cast<double>(minDelay + range));
            const int di = static_cast<int>(d);
            const float frac = static_cast<float>(d - di);
            int r0 = wr - di;
            if (r0 < 0) {
                r0 += bufLen;
            }
            int r1 = r0 - 1;
            if (r1 < 0) {
                r1 += bufLen;
            }
            const std::vector<float>& ln = pState->line[k];
            const float y = ln[r0] + (ln[r1] - ln[r0]) * frac;
            tap[k] = w * y;
            //  Lines 0/2 are fed from the left high band, 1/3 from the
            //  right; feedback sign alternates per pair. The window in
            //  the feedback path is what keeps fb 0.99 bounded.
            const float xin = high[(k % 2 == 0) ? 0 : 1];
            const float fbSign = (k == 0 || k == 2) ? 1.0f : -1.0f;
            pState->line[k][wr] = xin + fbSign * fb * tap[k];
        }
        wr++;
        if (wr >= bufLen) {
            wr = 0;
        }

        const float wetL = tap[0] - tap[2];
        const float wetR = tap[1] - tap[3];

        pOutput[i] = pInput[i] * dryGain + (low[0] + wetL) * wetGain;
        if (channelCount > 1) {
            pOutput[i + 1] =
                    pInput[i + 1] * dryGain + (low[1] + wetR) * wetGain;
        }

        //  Advance the sweep; wrap direction depends on the sign.
        phase += inc;
        if (rising) {
            if (phase < 0.0) {
                phase += 1.0;
            }
        } else {
            if (phase >= 1.0) {
                phase -= 1.0;
            }
        }
    }

    pState->phase = phase;
    pState->modPhase = modPhase;
    pState->dryGain = dryGain;
    pState->wetGain = wetGain;
    pState->writeIdx = wr;

    if (enableState == EffectEnableState::Disabling) {
        pState->clear();
    }
}

#include "effects/backends/builtin/mobiussaweffect.h"

#include <algorithm>
#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"

namespace {
//  An endless-glissando (Shepard) saw generator. Four partials two
//  octaves apart, base 55 Hz, each stepping one octave per BEAT period
//  through an eight-stage cycle (stage 7 is silent) under a squared
//  trapezoid window. Purely additive: it ignores its input and adds
//  the tone on top - it sounds with the deck stopped. A fixed ~93 ms
//  two-stage echo builds the runaway tail and creates the entire
//  stereo image (taps 12.3 ms apart).
//
//  Deliberate character kept from the reference: all four partials
//  change stage on the same sample, so the summed window wobbles by
//  ~0.8 dB at half the stage rate. Do not "fix" it.

constexpr double kBaseHz = 55.0;
//  Quadratic minimax fit of 2^p (exact at p = 0, 0.5, 1).
constexpr double kExpA = 0.6568542719; // 4*sqrt(2) - 5
constexpr double kExpB = 0.3431457579; // 2*(3 - 2*sqrt(2))
constexpr float kSawLevel = 1.4f;
constexpr float kEchoTap = 0.3f;
constexpr float kOutScale = 0.1f;
constexpr float kGainScale = 0.211f;
constexpr float kRampRate = 300.0f;
constexpr float kRampSnap = 0.00032f;
constexpr double kFallbackBeatMs = 500.0;
//  Fastest stage clamp: one wrap per sub-block of fs*6.666667e-4.
constexpr double kSubBlockSeconds = 6.666667e-4;

float toneGainFor(double depth) {
    const int L = static_cast<int>(depth / 100.0 * 1023.9);
    double v;
    if (L <= 9) {
        v = 0.0;
    } else if (L <= 511) {
        v = (L - 10) / 502.0;
    } else if (L <= 1012) {
        v = 1.0 + 1.3372804e-5 * (L - 512) * (L - 512);
    } else {
        v = 4.37;
    }
    return kGainScale * static_cast<float>(v);
}

//  Same vibrato idiom as Enigma Jet, bit-identical rate constant; the
//  triangle multiplies the oscillator frequency (+-12.5% at full).
struct ModLaw {
    float depth;
    double phaseInc;
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
        return out;
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
QString MobiusSawEffect::getId() {
    return "org.mixxx.effects.mobiussaw";
}

// static
EffectManifestPointer MobiusSawEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Mobius Saw"));
    pManifest->setShortName(QObject::tr("Mobius"));
    pManifest->setAuthor("Daedalus");
    pManifest->setVersion("1.0");
    pManifest->setDescription(QObject::tr(
            "An endlessly rising (or falling) saw-tone generator with a "
            "runaway echo, added on top of the input"));

    EffectManifestParameterPointer beats = pManifest->addParameter();
    beats->setId("beats");
    beats->setName(QObject::tr("Beats"));
    beats->setShortName(QObject::tr("Beats"));
    beats->setDescription(QObject::tr(
            "Beats per octave of climb; negative values fall instead of "
            "rise"));
    beats->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    beats->setUnitsHint(EffectManifestParameter::UnitsHint::Beats);
    beats->setRange(-64, 4, 64);

    EffectManifestParameterPointer depth = pManifest->addParameter();
    depth->setId("depth");
    depth->setName(QObject::tr("Depth"));
    depth->setShortName(QObject::tr("Depth"));
    depth->setDescription(QObject::tr(
            "Tone level: linear over the lower half of the knob, "
            "quadratic over the upper half"));
    depth->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    depth->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    depth->setRange(0, 50, 100);

    EffectManifestParameterPointer echo = pManifest->addParameter();
    echo->setId("echo");
    echo->setName(QObject::tr("Echo Level"));
    echo->setShortName(QObject::tr("Echo"));
    echo->setDescription(QObject::tr(
            "Echo send (lower half) and runaway feedback (upper half)"));
    echo->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    echo->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    echo->setRange(0.0, 0.5, 1.0);

    EffectManifestParameterPointer mod = pManifest->addParameter();
    mod->setId("mod");
    mod->setName(QObject::tr("Mod"));
    mod->setShortName(QObject::tr("Mod"));
    mod->setDescription(QObject::tr(
            "Pitch vibrato: off at center, slow-deep to the left, fast "
            "to the right"));
    mod->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    mod->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    mod->setRange(0.0, 0.5, 1.0);

    EffectManifestParameterPointer freeze = pManifest->addParameter();
    freeze->setId("freeze");
    freeze->setName(QObject::tr("Freeze"));
    freeze->setShortName(QObject::tr("Freeze"));
    freeze->setDescription(QObject::tr(
            "Holds the glissando: the tone drones at the frozen pitch, "
            "vibrato and echo keep running"));
    freeze->setValueScaler(EffectManifestParameter::ValueScaler::Toggle);
    freeze->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    freeze->setRange(0, 0, 1);

    return pManifest;
}

void MobiusSawEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pBeatsParameter = parameters.value("beats");
    m_pDepthParameter = parameters.value("depth");
    m_pEchoParameter = parameters.value("echo");
    m_pModParameter = parameters.value("mod");
    m_pFreezeParameter = parameters.value("freeze");
}

void MobiusSawEffect::processChannel(
        MobiusSawGroupState* pState,
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

    const float gainTarget = toneGainFor(m_pDepthParameter->value());

    //  Echo knob: send rises over the whole travel, feedback only over
    //  the top half (0.72 -> 0.96). The runaway build-up at high
    //  settings is the intended character.
    const double e = std::clamp(m_pEchoParameter->value(), 0.0, 1.0);
    const double x = 2.0 * e;
    const float send = static_cast<float>(x * x);
    double c;
    if (x < 0.9) {
        c = 0.9;
    } else if (x <= 1.0) {
        c = x;
    } else {
        c = 0.4 * e + 0.8;
    }
    const float fbe = static_cast<float>(0.8 * c);

    //  Signed beats -> master phase step; one BEAT period = one octave.
    const double beats = m_pBeatsParameter->value();
    const double beatMs = groupFeatures.beat_length.has_value()
            ? groupFeatures.beat_length->seconds * 1000.0
            : kFallbackBeatMs;
    double sec = beats * beatMs * 0.001;
    if (sec == 0.0) {
        sec = 0.001;
    }
    const double subBlock = fs * kSubBlockSeconds;
    const double stageSamples = sec * fs;
    double step = (stageSamples > 0)
            ? 1.0 / std::max(stageSamples, subBlock)
            : 1.0 / std::min(stageSamples, -subBlock);
    if (m_pFreezeParameter->toBool()) {
        step = 0.0;
    }
    const bool rising = step >= 0.0;

    const ModLaw mod = modLaw(m_pModParameter->value(), fs);
    const float rampInc = kRampRate / static_cast<float>(fs);

    double p = pState->masterPhase;
    double modPhase = pState->modPhase;
    float toneGain = pState->toneGain;

    for (SINT i = 0; i < samplesTotal; i += channelCount) {
        toneGain += (gainTarget - toneGain) * rampInc;
        if (std::fabs(gainTarget - toneGain) < kRampSnap) {
            toneGain = gainTarget;
        }

        float vib = 1.0f;
        if (mod.depth > 0.0f) {
            modPhase += mod.phaseInc;
            if (modPhase >= 2.0) {
                modPhase -= 2.0;
            }
            const float tri = static_cast<float>(
                    modPhase < 1.0 ? modPhase : 2.0 - modPhase);
            vib = 1.0f + mod.depth * (tri - 0.5f);
        }

        //  Base increment: 55 Hz times a quadratic fit of 2^p.
        const double incBase = (1.0 / fs) * vib * kBaseHz *
                (1.0 + kExpA * p + kExpB * p * p);

        //  Window ramp position: mirrors for the falling direction.
        const double q = rising ? p : 1.0 - p;

        float tone = 0.0f;
        for (int k = 0; k < 4; k++) {
            const int s = pState->stage[k];
            if (s == 7) {
                continue; // silent stage
            }
            float win;
            if (s == 0) {
                win = static_cast<float>(q);
            } else if (s == 6) {
                win = static_cast<float>(1.0 - q);
            } else {
                win = 1.0f;
            }
            const double inc = incBase * static_cast<double>(1 << s);
            double ph = pState->oscPhase[k] + inc;
            float v;
            if (ph >= 1.0) {
                ph -= 1.0;
                //  One-sample linear transition across the wrap.
                v = static_cast<float>(ph / inc - 0.5);
            } else {
                v = static_cast<float>(0.5 - ph);
            }
            pState->oscPhase[k] = ph;
            tone += win * win * kSawLevel * v;
        }

        //  Advance the glissando; a wrap = every partial steps a stage.
        p += step;
        bool wrapped = false;
        if (step > 0 && p >= 1.0) {
            p -= 1.0;
            wrapped = true;
        } else if (step < 0 && p < 0.0) {
            p += 1.0;
            wrapped = true;
        }
        if (wrapped) {
            for (int k = 0; k < 4; k++) {
                int s = pState->stage[k];
                s = (s + (rising ? 1 : -1) + 8) % 8;
                pState->stage[k] = s;
            }
        }

        //  Fixed two-stage echo; the taps ARE the stereo image.
        const float t = toneGain * tone;
        const float a = pState->bufA[pState->wa];
        const float b = pState->bufB[pState->wb];
        pState->bufA[pState->wa] = send * t + fbe * b;
        pState->bufB[pState->wb] = a;
        pState->wa++;
        if (pState->wa >= pState->lenA) {
            pState->wa = 0;
        }
        pState->wb++;
        if (pState->wb >= pState->lenB) {
            pState->wb = 0;
        }

        pOutput[i] = pInput[i] + kOutScale * (t + kEchoTap * a);
        if (channelCount > 1) {
            pOutput[i + 1] = pInput[i + 1] + kOutScale * (t + kEchoTap * b);
        }
    }

    pState->masterPhase = p;
    pState->modPhase = modPhase;
    pState->toneGain = toneGain;

    if (enableState == EffectEnableState::Disabling) {
        pState->clear();
    }
}

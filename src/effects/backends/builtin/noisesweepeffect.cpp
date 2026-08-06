#include "effects/backends/builtin/noisesweepeffect.h"

#include <algorithm>
#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"

namespace {
//  A swept-noise generator: white noise -> optional decimator crush ->
//  resonant 2nd-order band-pass (220 Hz - 9 kHz, one knob drives both
//  the center frequency and a loudness-compensating gain tilt) ->
//  half-beat echo. Purely additive on top of the input - it sounds
//  with the deck stopped. FREEZE is a real capture: the echo input
//  closes, feedback goes to exactly 1.0 and the output tap switches
//  0.35 -> 1.0, so the last half-beat of noise loops forever.

constexpr float kBpQ = 0.7f;
constexpr float kEchoTapNormal = 0.35f;
constexpr double kEchoMinMs = 10.0;
constexpr double kEchoMaxMs = 4000.0;
constexpr double kFallbackBeatMs = 500.0;
//  Gate / gain ramp time (linear).
constexpr double kGateSeconds = 0.0033333;
//  Crush: fixed-rate sample-and-hold plus a 0.5 gain trim.
constexpr double kCrushHoldHz = 4982.0;
//  Coefficient update interval.
constexpr SINT kCoefUpdateFrames = 16;

//  FREQ knob (raw 0..255) -> band-pass center frequency in Hz.
double bpCenterHz(double r) {
    const double i = r - 4.0;
    if (i < 0) {
        return 220.0;
    }
    return 220.0 * std::exp2(5.354361534 * std::pow(i / 251.0, 0.66));
}

//  FREQ knob -> noise gain: peaks at 0.8 a fifth of the way up, then
//  falls ~20 dB to the top. An equal-loudness tilt - without it the
//  high sweep would be brutally louder than the low one.
float noiseGainFor(int w) {
    if (w < 4) {
        return 0.0f;
    }
    if (w <= 50) {
        const double d = 51.0 - w;
        return static_cast<float>(0.8 - 0.00036215479 * d * d);
    }
    if (w <= 101) {
        return static_cast<float>(0.53812295 + 0.0051348442 * (102.0 - w));
    }
    const double a = 0.0019480520 * (w - 101);
    double s = 1.0 - a;
    s = s - a * s;
    s = s * s * s * s;
    return static_cast<float>(s * 0.0055210432 * (w - 3));
}
} // namespace

// static
QString NoiseSweepEffect::getId() {
    return "org.mixxx.effects.noisesweep";
}

// static
EffectManifestPointer NoiseSweepEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Noise Sweep"));
    pManifest->setShortName(QObject::tr("N.Sweep"));
    pManifest->setAuthor("Daedalus");
    pManifest->setVersion("1.0");
    pManifest->setDescription(QObject::tr(
            "A band-passed noise generator with a half-beat echo, added "
            "on top of the input"));

    EffectManifestParameterPointer freq = pManifest->addParameter();
    freq->setId("freq");
    freq->setName(QObject::tr("Freq"));
    freq->setShortName(QObject::tr("Freq"));
    freq->setDescription(QObject::tr(
            "Noise band center, 220 Hz - 9 kHz, with a matching loudness "
            "tilt"));
    freq->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    freq->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    freq->setRange(0, 50, 100);

    EffectManifestParameterPointer level = pManifest->addParameter();
    level->setId("level");
    level->setName(QObject::tr("Level"));
    level->setShortName(QObject::tr("Level"));
    level->setDescription(QObject::tr("Noise volume"));
    level->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    level->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    level->setRange(0.0, 0.5, 1.0);

    EffectManifestParameterPointer mod = pManifest->addParameter();
    mod->setId("mod");
    mod->setName(QObject::tr("Mod"));
    mod->setShortName(QObject::tr("Mod"));
    mod->setDescription(QObject::tr(
            "Free-running wobble of the noise band: depth fades in over "
            "the first sixth, then rate 5 - 50 Hz"));
    mod->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    mod->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    mod->setRange(0, 50, 100);

    EffectManifestParameterPointer crush = pManifest->addParameter();
    crush->setId("crush");
    crush->setName(QObject::tr("Crush"));
    crush->setShortName(QObject::tr("Crush"));
    crush->setDescription(QObject::tr(
            "Decimates the noise before the band-pass (which tames the "
            "aliasing)"));
    crush->setValueScaler(EffectManifestParameter::ValueScaler::Toggle);
    crush->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    crush->setRange(0, 0, 1);

    EffectManifestParameterPointer freeze = pManifest->addParameter();
    freeze->setId("freeze");
    freeze->setName(QObject::tr("Freeze"));
    freeze->setShortName(QObject::tr("Freeze"));
    freeze->setDescription(QObject::tr(
            "Loops the last half-beat of noise forever (ignored until "
            "the echo has filled once)"));
    freeze->setValueScaler(EffectManifestParameter::ValueScaler::Toggle);
    freeze->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    freeze->setRange(0, 0, 1);

    return pManifest;
}

void NoiseSweepEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pFreqParameter = parameters.value("freq");
    m_pLevelParameter = parameters.value("level");
    m_pModParameter = parameters.value("mod");
    m_pCrushParameter = parameters.value("crush");
    m_pFreezeParameter = parameters.value("freeze");
}

void NoiseSweepEffect::processChannel(
        NoiseSweepGroupState* pState,
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

    const double freqRaw =
            std::clamp(m_pFreqParameter->value(), 0.0, 100.0) * 2.55;
    const bool crush = m_pCrushParameter->toBool();
    const float gainTarget =
            noiseGainFor(static_cast<int>(freqRaw)) *
            static_cast<float>(std::clamp(m_pLevelParameter->value(), 0.0, 1.0)) *
            (crush ? 0.5f : 1.0f);

    //  MOD: unipolar - the first sixth of travel fades the depth in,
    //  the rest is a pure rate control, 5 Hz -> 50 Hz. Free-running by
    //  design (the reference does not beat-lock it).
    const double m =
            std::clamp(m_pModParameter->value(), 0.0, 100.0) * 2.55;
    const double mn = m / 255.0;
    double modDepth;
    if (mn < 0.015625) {
        modDepth = 0.0;
    } else if (mn < 0.15625) {
        modDepth = 0.027886713 * m - 0.111111;
    } else {
        modDepth = 1.0;
    }
    const double lfoPeriodMs = 200.0 - 0.70588235 * m;
    const double modInc = 2.0 / (lfoPeriodMs * 0.001 * fs);
    const double topTaper = (freqRaw > 230) ? 0.04 * (255.0 - freqRaw) : 1.0;

    //  Echo: always half a beat, no user control over the time.
    const double beatMs = groupFeatures.beat_length.has_value()
            ? groupFeatures.beat_length->seconds * 1000.0
            : kFallbackBeatMs;
    const double echoMs = std::clamp(beatMs * 0.5, kEchoMinMs, kEchoMaxMs);
    int echoSamples = static_cast<int>(echoMs * 0.001 * fs);
    if (echoSamples >= pState->echoLen) {
        echoSamples = pState->echoLen - 1;
    }

    //  FREEZE only engages once the echo has filled at least once.
    const bool frozen = m_pFreezeParameter->toBool() &&
            pState->samplesSinceEnable >= echoSamples;
    const float fbTarget = frozen ? 1.0f : 0.5f;
    const float gateInTarget = frozen ? 0.0f : 1.0f;
    const float gateNormTarget = frozen ? 0.0f : 1.0f;
    const float gateFrozTarget = frozen ? 1.0f : 0.0f;
    const float gateStep = static_cast<float>(1.0 / (kGateSeconds * fs));
    const float rampInc = static_cast<float>(300.0 / fs);

    const double holdPeriod = fs / kCrushHoldHz;

    auto ramp = [gateStep](float current, float target) {
        if (current < target) {
            return std::min(current + gateStep, target);
        }
        return std::max(current - gateStep, target);
    };

    float bpB0 = 0, bpA1 = 0, bpA2 = 0;
    SINT sinceCoef = kCoefUpdateFrames; // force compute on first frame

    for (SINT i = 0; i < samplesTotal; i += channelCount) {
        //  Free-running triangle in [-1, +1].
        pState->modPhase += modInc;
        if (pState->modPhase >= 2.0) {
            pState->modPhase -= 2.0;
        }
        const double tri = (pState->modPhase < 1.0)
                ? pState->modPhase * 2.0 - 1.0
                : 3.0 - pState->modPhase * 2.0;

        if (++sinceCoef >= kCoefUpdateFrames) {
            sinceCoef = 0;
            const double offset = std::trunc(modDepth * topTaper * 65.0 * tri);
            const double r = std::clamp(freqRaw + offset, 0.0, 255.0);
            double fc = bpCenterHz(r);
            fc = std::clamp(fc, 5.0, 0.45 * fs);
            //  RBJ constant-skirt band-pass.
            const double w0 = 2.0 * M_PI * fc / fs;
            const double alpha = std::sin(w0) / (2.0 * kBpQ);
            const double a0 = 1.0 + alpha;
            bpB0 = static_cast<float>(alpha / a0);
            bpA1 = static_cast<float>(-2.0 * std::cos(w0) / a0);
            bpA2 = static_cast<float>((1.0 - alpha) / a0);
        }

        pState->oscGain += (gainTarget - pState->oscGain) * rampInc;
        pState->gateEchoIn = ramp(pState->gateEchoIn, gateInTarget);
        pState->gateNormal = ramp(pState->gateNormal, gateNormTarget);
        pState->gateFrozen = ramp(pState->gateFrozen, gateFrozTarget);
        pState->feedback = fbTarget; // switched, matching the reference

        //  Crush decimator clock (shared across channels).
        bool holdSample = false;
        pState->holdCounter += 1.0;
        if (pState->holdCounter >= holdPeriod) {
            pState->holdCounter -= holdPeriod;
            holdSample = true;
        }

        int readPos = pState->echoWrite - echoSamples;
        if (readPos < 0) {
            readPos += pState->echoLen;
        }

        for (int c = 0; c < 2; c++) {
            float noise = pState->dist(pState->gen) * pState->oscGain;
            if (crush) {
                if (holdSample) {
                    pState->holdValue[c] = noise;
                }
                noise = pState->holdValue[c];
            }
            //  Band-pass (biquad, direct form 1).
            const float y = bpB0 * (noise - pState->bpX2[c]) -
                    bpA1 * pState->bpY1[c] - bpA2 * pState->bpY2[c];
            pState->bpX2[c] = pState->bpX1[c];
            pState->bpX1[c] = noise;
            pState->bpY2[c] = pState->bpY1[c];
            pState->bpY1[c] = y;

            const float echoOut = pState->echoBuf[c][readPos];
            pState->echoBuf[c][pState->echoWrite] =
                    y * pState->gateEchoIn + pState->feedback * echoOut;

            const float add = y +
                    echoOut * (kEchoTapNormal * pState->gateNormal +
                                      pState->gateFrozen);
            if (c < channelCount) {
                pOutput[i + c] = pInput[i + c] + add;
            }
        }
        pState->echoWrite++;
        if (pState->echoWrite >= pState->echoLen) {
            pState->echoWrite = 0;
        }
        pState->samplesSinceEnable++;
    }

    if (enableState == EffectEnableState::Disabling) {
        pState->clear();
    }
}

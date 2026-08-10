#include "effects/backends/builtin/colorfiltereffect.h"

#include <algorithm>
#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/sample.h"

namespace {
//  A bipolar color filter: a low-pass sweep left of center, transparent
//  at the center detent, a high-pass sweep right of center. The two
//  biquads run in PARALLEL with the dry path and the three are summed
//  with position-dependent gains, so the center dead zone is truly empty
//  and the entries into it are short crossfades.
//
//  Both knobs are quantized to 8-bit codes and slewed one code at a time
//  on a fixed clock, so a full sweep always takes a minimum time - the
//  faintly stepped feel is part of the character. Cutoff laws are cubic
//  in the code. The second knob sets damping (1/Q); a color-derived
//  damping floor stops the high-pass booming when parked near its
//  bottom, and a gain taper limits the resonant peak at high resonance.
//  The biquads are bilinear with NO prewarping and the parked low-pass
//  sits above Nyquist - both are deliberate voicing, keep them.

constexpr double kCodeSeconds = 0.0003333; // slew time per 8-bit code step

using Coef = ColorFilterGroupState::Coef;

//  Bilinear 2nd-order sections with makeup gain; q is 1/Q (damping).
Coef makeLpf2(double fc, double q, double gain, double fs) {
    const double w = 2.0 * M_PI * fc / fs;
    const double d = 4.0 + 2.0 * w * q + w * w;
    Coef c;
    c.b0 = gain * w * w / d;
    c.b1 = 2.0 * c.b0;
    c.b2 = c.b0;
    c.a1 = (2.0 * w * w - 8.0) / d;
    c.a2 = (4.0 - 2.0 * w * q + w * w) / d;
    return c;
}

Coef makeHpf2(double fc, double q, double gain, double fs) {
    const double w = 2.0 * M_PI * fc / fs;
    const double d = 4.0 + 2.0 * w * q + w * w;
    Coef c;
    c.b0 = 4.0 * gain / d;
    c.b1 = -2.0 * c.b0;
    c.b2 = c.b0;
    c.a1 = (2.0 * w * w - 8.0) / d;
    c.a2 = (4.0 - 2.0 * w * q + w * w) / d;
    return c;
}

inline double filterSample(const Coef& c, double x, double* s1, double* s2) {
    const double y = c.b0 * x + *s1;
    *s1 = c.b1 * x - c.a1 * y + *s2;
    *s2 = c.b2 * x - c.a2 * y;
    return y;
}

//  Rebuild both biquads from the current codes. Cutoffs are cubic in the
//  color code; damping comes from the param code with the color-derived
//  floor; the makeup gain is unity for damping >= 0.5 and trims the
//  passband below that so the resonant peak stays bounded.
void updateCoef(ColorFilterGroupState* pState, double fs) {
    const int c = pState->colorCode;
    const int p = pState->paramCode;

    double lpfFc;
    double hpfFc;
    double colorDamping = 0.0;
    if (c <= 125) {
        const double u = c / 126.0;
        lpfFc = 25000.0 * u * u * u + 100.0;
        hpfFc = 20.0; // parked
    } else if (c <= 130) {
        lpfFc = 24608.75;
        hpfFc = 20.0;
    } else {
        const double u = (c - 130) / 126.0;
        hpfFc = 8200.0 * u * u * u + 20.0;
        lpfFc = 24608.75; // parked
        if (hpfFc < 249.9) {
            const double t = 1.0 - 35.65217209 * u * u * u;
            colorDamping = std::min(2.0, 4.0 * t * t * t + 0.03);
        }
    }

    double damping;
    if (p <= 127) {
        const double t = 1.0 - p / 128.0;
        damping = 3.5 * t * t + 0.5;
    } else {
        damping = 0.5 - 0.003662109375 * (p - 128);
    }
    damping = std::max(damping, colorDamping);

    const double g = (damping < 0.5) ? 2.0 * damping : 1.0;
    const double w = std::abs(c - (c > 127 ? 192 : 64)) / 128.0;
    const double gain = (w + 0.5) * g + (0.5 - w);

    pState->lpf = makeLpf2(lpfFc, damping, gain, fs);
    pState->hpf = makeHpf2(hpfFc, damping, gain, fs);
    pState->coefRate = fs;
}

//  Color code -> the three mix-gain targets. The small step on the left
//  edge of the dead zone is authentic - keep it.
void gainTargets(int c, float* lpf, float* dry, float* hpf) {
    if (c <= 109) {
        *lpf = 1.0f;
        *dry = 0.0f;
        *hpf = 0.0f;
    } else if (c <= 125) {
        *dry = (c - 110) / 16.0f;
        *lpf = 1.0f - *dry;
        *hpf = 0.0f;
    } else if (c <= 130) {
        *lpf = 0.0f;
        *dry = 1.0f;
        *hpf = 0.0f;
    } else if (c <= 146) {
        *hpf = (c - 130) / 16.0f;
        *dry = 1.0f - *hpf;
        *lpf = 0.0f;
    } else {
        *lpf = 0.0f;
        *dry = 0.0f;
        *hpf = 1.0f;
    }
}
} // namespace

// static
QString ColorFilterEffect::getId() {
    return "org.mixxx.effects.colorfilter";
}

// static
EffectManifestPointer ColorFilterEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Filter CFX"));
    pManifest->setShortName(QObject::tr("Filter CFX"));
    pManifest->setAuthor("Daedalus");
    pManifest->setVersion("1.0");
    pManifest->setDescription(QObject::tr(
            "A bipolar color filter: low-pass sweep left of center, "
            "transparent at center, high-pass sweep right of center"));
    pManifest->setEffectRampsFromDry(true);
    pManifest->setMetaknobDefault(0.5);

    EffectManifestParameterPointer color = pManifest->addParameter();
    color->setId("color");
    color->setName(QObject::tr("Color"));
    color->setShortName(QObject::tr("Color"));
    color->setDescription(QObject::tr(
            "Left of center: low-pass sweeps down; center: transparent; "
            "right of center: high-pass sweeps up"));
    color->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    color->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    color->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    color->setNeutralPointOnScale(0.5);
    color->setRange(0.0, 0.5, 1.0);

    // this is the "Param" knob from research
    EffectManifestParameterPointer param = pManifest->addParameter();
    param->setId("param");
    param->setName(QObject::tr("Resonance"));
    param->setShortName(QObject::tr("Resonance"));
    param->setDescription(QObject::tr(
            "Resonance of the filter: damped at the low end, sharp at the "
            "top; the center detent is the calibrated default"));
    param->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    param->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    param->setRange(0.0, 0.5, 1.0);

    return pManifest;
}

void ColorFilterEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pColorParameter = parameters.value("color");
    m_pParamParameter = parameters.value("param");
}

void ColorFilterEffect::processChannel(
        ColorFilterGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    Q_UNUSED(groupFeatures);
    const double fs = engineParameters.sampleRate();
    const int channelCount = engineParameters.channelCount();
    const SINT samplesTotal = engineParameters.samplesPerBuffer();

    const int colorTarget = static_cast<int>(std::lround(
            std::clamp(m_pColorParameter->value(), 0.0, 1.0) * 255.0));
    const int paramTarget = static_cast<int>(std::lround(
            std::clamp(m_pParamParameter->value(), 0.0, 1.0) * 255.0));

    const double framesPerCode = kCodeSeconds * fs;

    if (enableState == EffectEnableState::Enabling) {
        // Jump straight to the requested codes: no slew, no gain ramp.
        pState->clear();
        pState->colorCode = colorTarget;
        pState->paramCode = paramTarget;
        pState->framesUntilStep = framesPerCode;
        gainTargets(pState->colorCode,
                &pState->lpfGain,
                &pState->dryGain,
                &pState->hpfGain);
        updateCoef(pState, fs);
    } else if (pState->coefRate != fs) {
        updateCoef(pState, fs);
    }

    // Per-sample limit on how fast the mix gains may move - a few samples
    // at typical buffer sizes, a de-click rather than a fade.
    const float gainStep =
            100.0f / static_cast<float>(engineParameters.framesPerBuffer());

    float lpfTarget;
    float dryTarget;
    float hpfTarget;
    gainTargets(pState->colorCode, &lpfTarget, &dryTarget, &hpfTarget);

    float lpfGain = pState->lpfGain;
    float dryGain = pState->dryGain;
    float hpfGain = pState->hpfGain;

    const auto ramp = [gainStep](float g, float target) {
        if (g < target) {
            return std::min(g + gainStep, target);
        }
        return std::max(g - gainStep, target);
    };

    for (SINT i = 0; i < samplesTotal; i += channelCount) {
        // Step the 8-bit codes one at a time on the fixed clock and
        // rebuild the coefficients at every step.
        pState->framesUntilStep -= 1.0;
        if (pState->framesUntilStep <= 0.0) {
            pState->framesUntilStep += framesPerCode;
            bool changed = false;
            if (pState->colorCode != colorTarget) {
                pState->colorCode +=
                        (colorTarget > pState->colorCode) ? 1 : -1;
                changed = true;
            }
            if (pState->paramCode != paramTarget) {
                pState->paramCode +=
                        (paramTarget > pState->paramCode) ? 1 : -1;
                changed = true;
            }
            if (changed) {
                updateCoef(pState, fs);
                gainTargets(pState->colorCode,
                        &lpfTarget,
                        &dryTarget,
                        &hpfTarget);
            }
        }

        lpfGain = ramp(lpfGain, lpfTarget);
        dryGain = ramp(dryGain, dryTarget);
        hpfGain = ramp(hpfGain, hpfTarget);

        for (int ch = 0; ch < channelCount && ch < 2; ch++) {
            const double x = pInput[i + ch];
            // Both biquads always run so their memory stays warm across
            // mix-gain changes.
            const double lp = filterSample(
                    pState->lpf, x, &pState->s1[0][ch], &pState->s2[0][ch]);
            const double hp = filterSample(
                    pState->hpf, x, &pState->s1[1][ch], &pState->s2[1][ch]);
            pOutput[i + ch] = static_cast<float>(
                    dryGain * x + lpfGain * lp + hpfGain * hp);
        }
    }

    pState->lpfGain = lpfGain;
    pState->dryGain = dryGain;
    pState->hpfGain = hpfGain;

    if (enableState == EffectEnableState::Disabling) {
        SampleUtil::linearCrossfadeBuffersOut(
                pOutput, pInput, samplesTotal, channelCount);
        pState->clear();
    }
}

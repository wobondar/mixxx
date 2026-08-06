#include "effects/backends/builtin/sweepeffect.h"

#include <algorithm>
#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"

namespace {
//  A bipolar sweep whose TOPOLOGY changes at the center detent:
//  left of center the two biquads run in PARALLEL and are summed - a
//  widening notch that ends as a near-total kill leaving only sub-bass
//  and air; right of center they run in SERIES - a narrowing band-pass.
//  The center is transparent (fully dry), seven codes wide. There is
//  deliberately no prewarping, no Nyquist clamp and no makeup gain -
//  all three are part of the voicing.

//  Sweep knob crossfade: fully dry at the detent, fully wet once a
//  quarter into either half.
constexpr double kDetentLoC = 124;
constexpr double kDetentHiC = 131;

struct Coef {
    double b0, b1, b2, a1, a2;
};

//  Bilinear 2nd-order sections; q is 1/Q.
Coef makeLpf2(double fc, double q, double fs) {
    const double w = 2.0 * M_PI * fc / fs;
    const double d = 4.0 + 2.0 * w * q + w * w;
    Coef c;
    c.b0 = w * w / d;
    c.b1 = 2.0 * c.b0;
    c.b2 = c.b0;
    c.a1 = (2.0 * w * w - 8.0) / d;
    c.a2 = (4.0 - 2.0 * w * q + w * w) / d;
    return c;
}

Coef makeHpf2(double fc, double q, double fs) {
    const double w = 2.0 * M_PI * fc / fs;
    const double d = 4.0 + 2.0 * w * q + w * w;
    Coef c;
    c.b0 = 4.0 / d;
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
} // namespace

// static
QString SweepEffect::getId() {
    return "org.mixxx.effects.sweep";
}

// static
EffectManifestPointer SweepEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Sweep"));
    pManifest->setShortName(QObject::tr("Sweep"));
    pManifest->setAuthor("Daedalus");
    pManifest->setVersion("1.0");
    pManifest->setDescription(QObject::tr(
            "A bipolar sweep: a widening notch to the left of center, a "
            "narrowing band-pass to the right"));
    pManifest->setEffectRampsFromDry(true);
    pManifest->setMetaknobDefault(0.5);

    EffectManifestParameterPointer sweep = pManifest->addParameter();
    sweep->setId("sweep");
    sweep->setName(QObject::tr("Sweep"));
    sweep->setShortName(QObject::tr("Sweep"));
    sweep->setDescription(QObject::tr(
            "Left of center: widening notch to a near-total kill; "
            "center: transparent; right: narrowing band-pass"));
    sweep->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    sweep->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    sweep->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    sweep->setNeutralPointOnScale(0.5);
    sweep->setRange(0.0, 0.5, 1.0);

    EffectManifestParameterPointer center = pManifest->addParameter();
    center->setId("center");
    center->setName(QObject::tr("Center"));
    center->setShortName(QObject::tr("Center"));
    center->setDescription(QObject::tr(
            "Center frequency of the sweep, 60 Hz - 20 kHz (hinged at "
            "1 kHz mid-knob)"));
    center->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    center->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    center->setRange(0.0, 0.5, 1.0);

    return pManifest;
}

void SweepEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pSweepParameter = parameters.value("sweep");
    m_pCenterParameter = parameters.value("center");
}

void SweepEffect::processChannel(
        SweepGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    Q_UNUSED(groupFeatures);
    const double fs = engineParameters.sampleRate();
    const int channelCount = engineParameters.channelCount();
    const SINT samplesTotal = engineParameters.samplesPerBuffer();

    if (enableState == EffectEnableState::Enabling) {
        pState->clear();
    }

    const int c = static_cast<int>(
            std::clamp(m_pSweepParameter->value(), 0.0, 1.0) * 255.0);
    const int p = static_cast<int>(
            std::clamp(m_pCenterParameter->value(), 0.0, 1.0) * 255.0);

    //  Center frequency: two linear segments hinged at 1 kHz mid-knob.
    const double f = (p < 128) ? 60.0 + 7.4015748 * p
                               : 1000.0 + 149.60630 * (p - 128);

    //  Corner frequencies and damping from the sweep position.
    double fHpf;
    double fLpf;
    double invQ;
    if (c <= 123) {
        const double e = c / 123.0;
        fHpf = 20000.0 * std::pow(f / 20000.0, e);
        fLpf = 60.0 * std::pow(f / 60.0, e);
        invQ = 1.0;
    } else if (c <= 130) {
        fHpf = f / 17.4;
        fLpf = 17.4 * f;
        invQ = 1.00794578438414;
    } else {
        const double d = 256.0 - c;
        const double r = 1.4 + 0.0010078105 * d * d;
        fHpf = f / r;
        fLpf = f * r;
        invQ = (c <= 161) ? 0.70794578438414 + 0.009375 * (162 - c)
                          : 0.70794578438414;
    }

    //  Topology, with a filter clear on every crossing (click-free).
    const bool parallel = c < 127;
    if (parallel != pState->parallel) {
        pState->clear();
        pState->parallel = parallel;
    }

    //  Dry/wet: fully dry at the detent, fully wet a quarter into
    //  either half - the outer three quarters only change the filter.
    double xNorm;
    if (c <= 123) {
        xNorm = 1.0 - c / 246.0;
    } else if (c >= 132) {
        xNorm = 0.5 + (c - 132) / 246.0;
    } else {
        xNorm = 0.5;
    }
    const double t = std::abs(2.0 * xNorm - 1.0);
    const double dryRoot = std::max(0.0, 1.0 - 4.0 * t);
    const float dry = static_cast<float>(dryRoot * dryRoot);
    const float wet = 1.0f - dry;

    const Coef lpf = makeLpf2(fLpf, invQ, fs);
    const Coef hpf = makeHpf2(fHpf, invQ, fs);

    for (SINT i = 0; i < samplesTotal; i += channelCount) {
        for (int ch = 0; ch < channelCount && ch < 2; ch++) {
            const double x = pInput[i + ch];
            double y;
            if (parallel) {
                y = filterSample(lpf, x, &pState->s1[0][ch], &pState->s2[0][ch]) +
                        filterSample(hpf, x, &pState->s1[1][ch], &pState->s2[1][ch]);
            } else {
                y = filterSample(lpf, x, &pState->s1[0][ch], &pState->s2[0][ch]);
                y = filterSample(hpf, y, &pState->s1[1][ch], &pState->s2[1][ch]);
            }
            pOutput[i + ch] = static_cast<float>(
                    dry * static_cast<double>(x) + wet * y);
        }
    }

    if (enableState == EffectEnableState::Disabling) {
        pState->clear();
    }
}

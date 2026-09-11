#include "waveform/renderers/allshader/waveformrendererrgb.h"

#include <algorithm>
#include <cmath>

#include "rendergraph/material/rgbmaterial.h"
#include "rendergraph/vertexupdaters/rgbvertexupdater.h"
#include "track/track.h"
#include "util/colorcomponents.h"
#include "util/math.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "waveform/waveform.h"

using namespace rendergraph;

namespace allshader {

WaveformRendererRGB::WaveformRendererRGB(WaveformWidgetRenderer* waveformWidget,
        ::WaveformRendererAbstract::PositionSource type,
        WaveformRendererSignalBase::Options options)
        : WaveformRendererSignalBase(waveformWidget),
          m_isSlipRenderer(type == ::WaveformRendererAbstract::Slip),
          m_options(options) {
    initForRectangles<RGBMaterial>(0);
    setUsePreprocess(true);
}

void WaveformRendererRGB::setAxesColor(const QColor& axesColor) {
    getRgbF(axesColor, &m_axesColor_r, &m_axesColor_g, &m_axesColor_b, &m_axesColor_a);
}

void WaveformRendererRGB::setLowColor(const QColor& lowColor) {
    getRgbF(lowColor, &m_rgbLowColor_r, &m_rgbLowColor_g, &m_rgbLowColor_b);
}

void WaveformRendererRGB::setMidColor(const QColor& midColor) {
    getRgbF(midColor, &m_rgbMidColor_r, &m_rgbMidColor_g, &m_rgbMidColor_b);
}

void WaveformRendererRGB::setHighColor(const QColor& highColor) {
    getRgbF(highColor, &m_rgbHighColor_r, &m_rgbHighColor_g, &m_rgbHighColor_b);
}

void WaveformRendererRGB::onSetup(const QDomNode&) {
}

void WaveformRendererRGB::preprocess() {
    if (!preprocessInner()) {
        if (geometry().vertexCount() != 0) {
            geometry().allocate(0);
            markDirtyGeometry();
        }
    }
}

bool WaveformRendererRGB::preprocessInner() {
    TrackPointer pTrack = m_waveformRenderer->getTrackInfo();

    if (!pTrack || (m_isSlipRenderer && !m_waveformRenderer->isSlipActive())) {
        return false;
    }

    auto positionType = m_isSlipRenderer ? ::WaveformRendererAbstract::Slip
                                         : ::WaveformRendererAbstract::Play;

    ConstWaveformPointer waveform = pTrack->getWaveform();
    if (waveform.isNull()) {
        return false;
    }

    const int dataSize = waveform->getDataSize();
    if (dataSize <= 1) {
        return false;
    }

    const WaveformData* data = waveform->data();
    if (data == nullptr) {
        return false;
    }
#ifdef __STEM__
    auto stemInfo = pTrack->getStemInfo();
    // If this track is a stem track, skip the rendering
    if (!stemInfo.isEmpty() && waveform->hasStem()) {
        return false;
    }
#endif
#ifdef __LIVE_STEMS__
    const LiveStems live = liveStems();
    const double audioVisualRatio = waveform->getAudioVisualRatio();
#endif

    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();
    const int length = static_cast<int>(m_waveformRenderer->getLength());
    const int pixelLength = static_cast<int>(m_waveformRenderer->getLength() * devicePixelRatio);
    const float invDevicePixelRatio = 1.f / devicePixelRatio;
    const float halfPixelSize = 0.5f / devicePixelRatio;

    // See waveformrenderersimple.cpp for a detailed explanation of the frame and index calculation
    const int visualFramesSize = dataSize / 2;
    const double firstVisualFrame =
            m_waveformRenderer->getFirstDisplayedPosition(positionType) * visualFramesSize;
    const double lastVisualFrame =
            m_waveformRenderer->getLastDisplayedPosition(positionType) * visualFramesSize;

    // Represents the # of visual frames per horizontal pixel.
    const double visualIncrementPerPixel =
            (lastVisualFrame - firstVisualFrame) / static_cast<double>(pixelLength);

    // Fixes a sporadic crash caused by a division by zero on waveform initialization
    if (visualIncrementPerPixel == 0.0) {
        return false;
    }

    // Per-band gain from the EQ knobs.
    float allGain = 1.0f;
    float lowGain = 1.0f;
    float midGain = 1.0f;
    float highGain = 1.0f;
    getGains(&allGain, &lowGain, &midGain, &highGain);

    const float breadth = static_cast<float>(m_waveformRenderer->getBreadth());
    const float halfBreadth = breadth / 2.0f;

    const float heightFactorAbs = allGain * halfBreadth / m_maxValue;
    const float heightFactor[2] = {-heightFactorAbs, heightFactorAbs};
    const bool splitLeftRight = m_options & WaveformRendererSignalBase::Option::SplitStereoSignal;

    const float low_r = static_cast<float>(m_rgbLowColor_r);
    const float mid_r = static_cast<float>(m_rgbMidColor_r);
    const float high_r = static_cast<float>(m_rgbHighColor_r);
    const float low_g = static_cast<float>(m_rgbLowColor_g);
    const float mid_g = static_cast<float>(m_rgbMidColor_g);
    const float high_g = static_cast<float>(m_rgbHighColor_g);
    const float low_b = static_cast<float>(m_rgbLowColor_b);
    const float mid_b = static_cast<float>(m_rgbMidColor_b);
    const float high_b = static_cast<float>(m_rgbHighColor_b);

    // Effective visual frame for x
    double xVisualFrame = qRound(firstVisualFrame / visualIncrementPerPixel) *
            visualIncrementPerPixel;

    const int numVerticesPerLine = 6; // 2 triangles

    const int reserved = numVerticesPerLine *
            // Slip renderer only render a single channel, so the vertices count doesn't change
            ((splitLeftRight && !m_isSlipRenderer ? pixelLength * 2 : pixelLength) + 1);

    geometry().setDrawingMode(Geometry::DrawingMode::Triangles);
    geometry().allocate(reserved);
    markDirtyGeometry();

    RGBVertexUpdater vertexUpdater{geometry().vertexDataAs<Geometry::RGBColoredPoint2D>()};
    vertexUpdater.addRectangle({0.f,
                                       halfBreadth - 0.5f},
            {static_cast<float>(length),
                    m_isSlipRenderer ? halfBreadth : halfBreadth + 0.5f},
            {static_cast<float>(m_axesColor_r),
                    static_cast<float>(m_axesColor_g),
                    static_cast<float>(m_axesColor_b)});

    const double maxSamplingRange = visualIncrementPerPixel / 2.0;

    for (int pos = 0; pos < pixelLength; ++pos) {
        const int visualFrameStart = std::lround(xVisualFrame - maxSamplingRange);
        const int visualFrameStop = std::lround(xVisualFrame + maxSamplingRange);

        const int visualIndexStart = std::max(visualFrameStart * 2, 0);
        const int visualIndexStop =
                std::min(std::max(visualFrameStop, visualFrameStart + 1) * 2, dataSize - 1);

        const float fpos = static_cast<float>(pos) * invDevicePixelRatio;
#ifdef __LIVE_STEMS__
        float liveHeight = 1.0f;
        float liveColor = 1.0f;
        live.columnFactors(static_cast<SINT>(xVisualFrame * audioVisualRatio),
                &liveHeight,
                &liveColor);
#endif

        // Per band: max over the frames under this pixel drives the height,
        // mean over the same frames drives the colour. Using the max for
        // colour too would mix band peaks from different frames into a hue
        // the audio never had.
        uchar u8maxLow[2]{};
        uchar u8maxMid[2]{};
        uchar u8maxHigh[2]{};
        float sumLow[2]{};
        float sumMid[2]{};
        float sumHigh[2]{};
        float sumAll[2]{};
        int frameCount[2]{};
        // - Per channel
        uchar u8maxAllChn[2]{};
        for (int chn = 0; chn < 2; chn++) {
            // In case we don't render individual color per channel, we use only
            // the first field of the arrays to perform signal max
            int signalChn = splitLeftRight ? chn : 0;
            // data is interleaved left / right
            for (int i = visualIndexStart + chn; i < visualIndexStop + chn; i += 2) {
                const WaveformData& waveformData = data[i];

                u8maxLow[signalChn] = math_max(u8maxLow[signalChn], waveformData.filtered.low);
                u8maxMid[signalChn] = math_max(u8maxMid[signalChn], waveformData.filtered.mid);
                u8maxHigh[signalChn] = math_max(u8maxHigh[signalChn], waveformData.filtered.high);
                u8maxAllChn[signalChn] = math_max(
                        u8maxAllChn[signalChn], waveformData.filtered.all);
                sumLow[signalChn] += waveformData.filtered.low;
                sumMid[signalChn] += waveformData.filtered.mid;
                sumHigh[signalChn] += waveformData.filtered.high;
                sumAll[signalChn] += waveformData.filtered.all;
                frameCount[signalChn]++;
            }
        }
        float maxAllChn[2]{static_cast<float>(u8maxAllChn[0]), static_cast<float>(u8maxAllChn[1])};
        if (m_heightCurve != 1.0f) {
            for (float& v : maxAllChn) {
                v = std::pow(v / m_maxValue, m_heightCurve) * m_maxValue;
            }
        }

        // In case we don't render individual color per channel, all the
        // signal information is in the first field of each array. If
        // this is the split render, we only render the left channel
        // anyway.
        for (int chn = 0;
                chn < (splitLeftRight && !m_isSlipRenderer ? 2 : 1);
                chn++) {
            // Cast to float
            float maxLowU = static_cast<float>(u8maxLow[chn]);
            float maxMidU = static_cast<float>(u8maxMid[chn]);
            float maxHighU = static_cast<float>(u8maxHigh[chn]);

            // Apply the gains
            float maxLow = maxLowU * lowGain;
            float maxMid = maxMidU * midGain;
            float maxHigh = maxHighU * highGain;

            float allUnscaled = maxLowU + maxMidU + maxHighU;
            float eqGain = 1.0f;
            if (allUnscaled > 0.0f) {
                if (m_proportionalColor) {
                    // The band visual gains are colour weights here, so the
                    // height only follows the EQ knobs and kills.
                    constexpr float kMinGain = 0.01f;
                    eqGain = (maxLowU * lowGain / std::max(m_lowVisualGain, kMinGain) +
                                     maxMidU * midGain / std::max(m_midVisualGain, kMinGain) +
                                     maxHighU * highGain / std::max(m_highVisualGain, kMinGain)) /
                            allUnscaled;
                } else {
                    eqGain = (maxLow + maxMid + maxHigh) / allUnscaled;
                }
            }

            // Colour weights: gained band means as a 0..1 fraction of full
            // scale, raised to the band contrast.
            const float invCount = frameCount[chn] > 0 ? 1.0f / frameCount[chn] : 0.0f;
            float wLow = sumLow[chn] * invCount * lowGain / m_maxValue;
            float wMid = sumMid[chn] * invCount * midGain / m_maxValue;
            float wHigh = sumHigh[chn] * invCount * highGain / m_maxValue;
            if (m_proportionalColor) {
                // Each band as a share of the column's total level, so the
                // hue follows the balance and not the loudness.
                const float meanAll = sumAll[chn] * invCount / m_maxValue;
                const float invAll = meanAll > 0.0f ? 1.0f / meanAll : 0.0f;
                wLow *= invAll;
                wMid *= invAll;
                wHigh *= invAll;
            }
            if (m_bandContrast != 1.0f) {
                wLow = std::pow(wLow, m_bandContrast);
                wMid = std::pow(wMid, m_bandContrast);
                wHigh = std::pow(wHigh, m_bandContrast);
            }
            if (m_proportionalColor) {
                wLow = std::min(1.0f, wLow * m_colorGain);
                wMid = std::min(1.0f, wMid * m_colorGain);
                wHigh = std::min(1.0f, wHigh * m_colorGain);
            }
            float red = wLow * low_r + wMid * mid_r + wHigh * high_r;
            float green = wLow * low_g + wMid * mid_g + wHigh * high_g;
            float blue = wLow * low_b + wMid * mid_b + wHigh * high_b;

            if (m_proportionalColor) {
                red = std::min(1.0f, red);
                green = std::min(1.0f, green);
                blue = std::min(1.0f, blue);
            } else {
                // Normalize the color components using the maximum of the three
                const float maxComponent = math_max3(red, green, blue);
                if (maxComponent == 0.f) {
                    // Avoid division by 0
                    red = 0.f;
                    green = 0.f;
                    blue = 0.f;
                } else {
                    const float normFactor = 1.f / maxComponent;
                    red *= normFactor;
                    green *= normFactor;
                    blue *= normFactor;
                }
            }
#ifdef __LIVE_STEMS__
            red *= liveColor;
            green *= liveColor;
            blue *= liveColor;
            maxAllChn[chn] *= liveHeight;
#endif

            // Lines are thin rectangles
            if (!splitLeftRight) {
                vertexUpdater.addRectangle(
                        {fpos - halfPixelSize,
                                halfBreadth -
                                        heightFactorAbs * eqGain *
                                                maxAllChn[chn]},
                        {fpos + halfPixelSize,
                                m_isSlipRenderer ? halfBreadth
                                                 : halfBreadth +
                                                heightFactorAbs * eqGain *
                                                        maxAllChn[chn]},
                        {red, green, blue});
            } else {
                // note: heightFactor is the same for left and right,
                // but negative for left (chn 0) and positive for right (chn 1)
                vertexUpdater.addRectangle({fpos - halfPixelSize,
                                                   halfBreadth},
                        {fpos + halfPixelSize,
                                halfBreadth + heightFactor[chn] * eqGain * maxAllChn[chn]},
                        {red,
                                green,
                                blue});
            }
        }

        xVisualFrame += visualIncrementPerPixel;
    }

    DEBUG_ASSERT(reserved == vertexUpdater.index());

    markDirtyMaterial();

    return true;
}

} // namespace allshader

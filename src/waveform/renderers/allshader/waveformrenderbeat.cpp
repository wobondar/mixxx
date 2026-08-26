#include "waveform/renderers/allshader/waveformrenderbeat.h"

#include <QDomNode>

#include "engine/engine.h"
#include "moc_waveformrenderbeat.cpp"
#include "rendergraph/geometry.h"
#include "rendergraph/material/rgbamaterial.h"
#include "rendergraph/vertexupdaters/rgbavertexupdater.h"
#include "skin/legacy/skincontext.h"
#include "track/track.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "waveform/waveform.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/wskincolor.h"

using namespace rendergraph;

namespace {
// Marker geometry for the accented (every 4th) beats, rekordbox-style:
// a small triangle at the top and bottom edge, pointing into the waveform.
constexpr float kAccentTriangleHalfWidth = 3.5f;
constexpr float kAccentTriangleHeight = 6.f;
} // namespace

namespace allshader {

WaveformRenderBeat::WaveformRenderBeat(WaveformWidgetRenderer* waveformWidget,
        ::WaveformRendererAbstract::PositionSource type)
        : ::WaveformRendererAbstract(waveformWidget),
          m_isSlipRenderer(type == ::WaveformRendererAbstract::Slip) {
    initForRectangles<RGBAMaterial>(0);
    setUsePreprocess(true);
}

void WaveformRenderBeat::setup(const QDomNode& node, const SkinContext& skinContext) {
    m_color = QColor(skinContext.selectString(node, QStringLiteral("BeatColor")));
    m_color = WSkinColor::getCorrectColor(m_color).toRgb();
    // Optional skin color for the every-4th-beat markers; defaults to
    // rekordbox-ish red so existing skins need no change.
    QColor accentColor(skinContext.selectString(
            node, QStringLiteral("BeatAccentColor")));
    if (!accentColor.isValid()) {
        accentColor = QColor(QStringLiteral("#FF3B30"));
    }
    m_accentColor = WSkinColor::getCorrectColor(accentColor).toRgb();
}

void WaveformRenderBeat::draw(QPainter* painter, QPaintEvent* event) {
    Q_UNUSED(painter);
    Q_UNUSED(event);
    DEBUG_ASSERT(false);
}

void WaveformRenderBeat::preprocess() {
    if (!preprocessInner()) {
        geometry().allocate(0);
        markDirtyGeometry();
    }
}

bool WaveformRenderBeat::preprocessInner() {
    const TrackPointer trackInfo = m_waveformRenderer->getTrackInfo();

    if (!trackInfo || (m_isSlipRenderer && !m_waveformRenderer->isSlipActive())) {
        return false;
    }

    const bool isStemTrack = trackInfo && trackInfo->hasStem() &&
            trackInfo->getWaveform() && trackInfo->getWaveform()->hasStem();
    const bool splitStemTracks = isStemTrack &&
            WaveformWidgetFactory::instance()->isStemSplitTracks();

    auto positionType = m_isSlipRenderer ? ::WaveformRendererAbstract::Slip
                                         : ::WaveformRendererAbstract::Play;

    mixxx::BeatsPointer trackBeats = trackInfo->getBeats();
    if (!trackBeats) {
        return false;
    }

    int alpha = m_waveformRenderer->getBeatGridAlpha();
    if (alpha == 0) {
        return false;
    }

    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();

    m_color.setAlphaF(alpha / 100.0f);

    const double trackSamples = m_waveformRenderer->getTrackSamples();
    if (trackSamples <= 0.0) {
        return false;
    }

    const double firstDisplayedPosition =
            m_waveformRenderer->getFirstDisplayedPosition(positionType);
    const double lastDisplayedPosition =
            m_waveformRenderer->getLastDisplayedPosition(positionType);

    const auto startPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            firstDisplayedPosition * trackSamples);
    const auto endPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            lastDisplayedPosition * trackSamples);

    if (!startPosition.isValid() || !endPosition.isValid()) {
        return false;
    }

    const float rendererBreadth = m_waveformRenderer->getBreadth();

    const int numVerticesPerLine = 6;     // 2 triangles
    const int numVerticesPerAccent = 6;   // 2 marker triangles

    // The bar phase is counted from the grid's anchor beat. For grids
    // imported from rekordbox the anchor is the first downbeat, so
    // (index % 4 == 0) marks true downbeats; for Mixxx-analyzed grids it
    // is a stable every-4th-beat indication with arbitrary phase.
    const auto anchorIt = trackBeats->iteratorFrom(trackBeats->anchorPosition());

    // Count the number of beats in the range to reserve space in the m_vertices vector.
    // Note that we could also use
    //   int numBearsInRange = trackBeats->numBeatsInRange(startPosition, endPosition);
    // for this, but there have been reports of that method failing with a DEBUG_ASSERT.
    int numBeatsInRange = 0;
    int numAccentsInRange = 0;
    const auto itFirstInRange = trackBeats->iteratorFrom(startPosition);
    int beatIndex = static_cast<int>(itFirstInRange - anchorIt);
    for (auto it = itFirstInRange;
            it != trackBeats->cend() && *it <= endPosition;
            ++it, ++beatIndex) {
        numBeatsInRange++;
        if (((beatIndex % 4) + 4) % 4 == 0) {
            numAccentsInRange++;
        }
    }

    const int numBoxesPerBeat = (m_isSlipRenderer && splitStemTracks)
            ? mixxx::kMaxSupportedStems
            : 1;
    const int reserved = numBeatsInRange * numVerticesPerLine * numBoxesPerBeat +
            numAccentsInRange * numVerticesPerAccent;
    geometry().allocate(reserved);

    RGBAVertexUpdater vertexUpdater{
            geometry().vertexDataAs<Geometry::RGBAColoredPoint2D>()};

    const QVector4D lineRgba{static_cast<float>(m_color.redF()),
            static_cast<float>(m_color.greenF()),
            static_cast<float>(m_color.blueF()),
            static_cast<float>(m_color.alphaF())};
    const QVector4D accentRgba{static_cast<float>(m_accentColor.redF()),
            static_cast<float>(m_accentColor.greenF()),
            static_cast<float>(m_accentColor.blueF()),
            static_cast<float>(m_color.alphaF())};

    const float breadth = m_isSlipRenderer ? rendererBreadth / 2 : rendererBreadth;
    const float boxBreadth = splitStemTracks
            ? rendererBreadth / static_cast<float>(mixxx::kMaxSupportedStems)
            : rendererBreadth;

    beatIndex = static_cast<int>(itFirstInRange - anchorIt);
    for (auto it = itFirstInRange;
            it != trackBeats->cend() && *it <= endPosition;
            ++it, ++beatIndex) {
        double beatPosition = it->toEngineSamplePos();
        double xBeatPoint =
                m_waveformRenderer->transformSamplePositionInRendererWorld(
                        beatPosition, positionType);

        xBeatPoint = qRound(xBeatPoint * devicePixelRatio) / devicePixelRatio;

        const float x1 = static_cast<float>(xBeatPoint);
        const float x2 = x1 + 1.f;

        if (m_isSlipRenderer && splitStemTracks) {
            for (int stemIdx = 0; stemIdx < mixxx::kMaxSupportedStems; ++stemIdx) {
                const float posy1 = stemIdx * boxBreadth;
                const float posy2 = posy1 + boxBreadth / 2.f;
                vertexUpdater.addRectangle({x1, posy1}, {x2, posy2}, lineRgba);
            }
        } else {
            vertexUpdater.addRectangle({x1, 0.f}, {x2, breadth}, lineRgba);
        }

        if (((beatIndex % 4) + 4) % 4 == 0) {
            const float xMid = x1 + 0.5f;
            vertexUpdater.addTriangle({xMid - kAccentTriangleHalfWidth, 0.f},
                    {xMid + kAccentTriangleHalfWidth, 0.f},
                    {xMid, kAccentTriangleHeight},
                    accentRgba);
            vertexUpdater.addTriangle({xMid - kAccentTriangleHalfWidth, breadth},
                    {xMid + kAccentTriangleHalfWidth, breadth},
                    {xMid, breadth - kAccentTriangleHeight},
                    accentRgba);
        }
    }
    markDirtyGeometry();

    DEBUG_ASSERT(reserved == vertexUpdater.index());

    markDirtyMaterial();

    return true;
}

} // namespace allshader

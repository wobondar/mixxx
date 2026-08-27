#include "waveform/renderers/allshader/waveformrendermark.h"

#include <QPainterPath>

#include "moc_waveformrendermark.cpp"
#include "rendergraph/context.h"
#include "rendergraph/geometry.h"
#include "rendergraph/geometrynode.h"
#include "rendergraph/material/rgbamaterial.h"
#include "rendergraph/material/texturematerial.h"
#include "rendergraph/texture.h"
#include "rendergraph/vertexupdaters/rgbavertexupdater.h"
#include "rendergraph/vertexupdaters/texturedvertexupdater.h"
#include "track/track.h"
#include "util/colorcomponents.h"
#include "util/roundtopixel.h"
#include "waveform/renderers/allshader/digitsrenderer.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "waveform/waveformwidgetfactory.h"

using namespace rendergraph;

// On the use of QPainter:
//
// The renderers in this folder are optimized to use GLSL shaders and refrain
// from using QPainter on the QOpenGLWindow, which causes degredated performance.
//
// This renderer does use QPainter (indirectly, in WaveformMark::generateImage), but
// only to draw on a QImage. This is only done once when needed and the images are
// then used as textures to be drawn with a GLSL shader.

namespace {

// Each mark's texture holds both its line and its label box. The two are
// drawn as separate quads of that one texture, so that all lines can render
// below all label boxes and no line ever crosses a box.
enum class MarkPart {
    Line,
    Label,
};

class WaveformMarkNode : public rendergraph::GeometryNode {
  public:
    WaveformMark* m_pOwner{};

    WaveformMarkNode(WaveformMark* pOwner,
            rendergraph::Context* pContext,
            const QImage& image,
            MarkPart part)
            : m_pOwner(pOwner),
              m_part(part) {
        initForRectangles<TextureMaterial>(1);
        updateTexture(pContext, image);
    }
    void updateTexture(rendergraph::Context* pContext, const QImage& image) {
        dynamic_cast<TextureMaterial&>(material())
                .setTexture(std::make_unique<Texture>(pContext, image));
        m_textureWidth = image.width();
        m_textureHeight = image.height();
    }
    // Draw subRect (in logical image coordinates) of the texture, placed so
    // that the image origin lands at x, y.
    void update(float x, float y, float devicePixelRatio, const QRectF& subRect) {
#ifdef MIXXX_DEBUG_ASSERTIONS_ENABLED
        const float epsilon = 1e-6f;
        auto roundToPixel = createFunctionRoundToPixel(devicePixelRatio);
        DEBUG_ASSERT(std::abs(x - roundToPixel(x)) < epsilon);
        DEBUG_ASSERT(std::abs(y - roundToPixel(y)) < epsilon);
#endif
        const float w = m_textureWidth / devicePixelRatio;
        const float h = m_textureHeight / devicePixelRatio;
        const QRectF rect = subRect.intersected(QRectF(0, 0, w, h));
        TexturedVertexUpdater vertexUpdater{
                geometry().vertexDataAs<Geometry::TexturedPoint2D>()};
        vertexUpdater.addRectangle(
                {x + static_cast<float>(rect.left()),
                        y + static_cast<float>(rect.top())},
                {x + static_cast<float>(rect.right()),
                        y + static_cast<float>(rect.bottom())},
                {static_cast<float>(rect.left() / w),
                        static_cast<float>(rect.top() / h)},
                {static_cast<float>(rect.right() / w),
                        static_cast<float>(rect.bottom() / h)});
    }
    float textureWidth() const {
        return m_textureWidth;
    }
    float textureHeight() const {
        return m_textureHeight;
    }
    MarkPart part() const {
        return m_part;
    }

  public:
    float m_textureWidth{};
    float m_textureHeight{};
    MarkPart m_part;
};

class WaveformMarkNodeGraphics : public WaveformMark::Graphics {
  public:
    WaveformMarkNodeGraphics(WaveformMark* pOwner,
            rendergraph::Context* pContext,
            const QImage& image)
            : m_pOwner(pOwner),
              m_pLineNode(std::make_unique<WaveformMarkNode>(
                      pOwner, pContext, image, MarkPart::Line)),
              m_pLabelNode(std::make_unique<WaveformMarkNode>(
                      pOwner, pContext, image, MarkPart::Label)) {
    }
    void updateTexture(rendergraph::Context* pContext, const QImage& image) {
        lineNode()->updateTexture(pContext, image);
        labelNode()->updateTexture(pContext, image);
    }
    void update(float x, float y, float devicePixelRatio) {
        const float w = textureWidth() / devicePixelRatio;
        const float h = textureHeight() / devicePixelRatio;
        // A custom pixmap is a single hand-drawn image: draw it whole on the
        // label layer and leave the line layer empty.
        const bool wholeImageLabel = !m_pOwner->m_pixmapPath.isEmpty();
        const QRectF lineRect = wholeImageLabel
                ? QRectF()
                : QRectF(m_pOwner->m_linePosition - 2.f, 0, 4.f, h);
        // Grown by a pixel to cover the border's antialiasing fringe.
        const QRectF labelRect = wholeImageLabel
                ? QRectF(0, 0, w, h)
                : m_pOwner->m_label.area().adjusted(-1, -1, 1, 1);
        lineNode()->update(x, y, devicePixelRatio, lineRect);
        labelNode()->update(x, y, devicePixelRatio, labelRect);
    }
    bool hasLabel() const {
        return !m_pOwner->m_label.area().isEmpty() ||
                !m_pOwner->m_pixmapPath.isEmpty();
    }
    float textureWidth() const {
        return lineNode()->textureWidth();
    }
    float textureHeight() const {
        return lineNode()->textureHeight();
    }
    void attachNode(std::unique_ptr<rendergraph::BaseNode> pNode) {
        auto* pMarkNode = static_cast<WaveformMarkNode*>(pNode.get());
        if (pMarkNode->part() == MarkPart::Line) {
            DEBUG_ASSERT(!m_pLineNode);
            m_pLineNode = std::move(pNode);
        } else {
            DEBUG_ASSERT(!m_pLabelNode);
            m_pLabelNode = std::move(pNode);
        }
    }
    std::unique_ptr<rendergraph::BaseNode> detachLineNode() {
        return std::move(m_pLineNode);
    }
    std::unique_ptr<rendergraph::BaseNode> detachLabelNode() {
        return std::move(m_pLabelNode);
    }

  private:
    WaveformMarkNode* lineNode() const {
        DEBUG_ASSERT(m_pLineNode);
        return static_cast<WaveformMarkNode*>(m_pLineNode.get());
    }
    WaveformMarkNode* labelNode() const {
        DEBUG_ASSERT(m_pLabelNode);
        return static_cast<WaveformMarkNode*>(m_pLabelNode.get());
    }

    WaveformMark* m_pOwner{};
    std::unique_ptr<rendergraph::BaseNode> m_pLineNode;
    std::unique_ptr<rendergraph::BaseNode> m_pLabelNode;
};

constexpr float kPlayPosWidth{11.f};
constexpr float kPlayPosOffset{-(kPlayPosWidth - 1.f) / 2.f};

QString timeSecToString(double timeSec) {
    int hundredths = std::lround(timeSec * 100.0);
    int seconds = hundredths / 100;
    hundredths -= seconds * 100;
    int minutes = seconds / 60;
    seconds -= minutes * 60;

    return QString::asprintf("%d:%02d.%02d", minutes, seconds, hundredths);
}

} // namespace

// Both allshader::WaveformRenderMark and the non-GL ::WaveformRenderMark derive
// from WaveformRenderMarkBase. The base-class takes care of updating the marks
// when needed and flagging them when their image needs to be updated (resizing,
// cue changes, position changes)
//
// While in the case of ::WaveformRenderMark those images can be updated immediately,
// in the case of allshader::WaveformRenderMark we need to do that when we have an
// openGL context, as we create new textures.
//
// The boolean argument for the WaveformRenderMarkBase constructor indicates
// that updateMarkImages should not be called immediately.

allshader::WaveformRenderMark::WaveformRenderMark(
        WaveformWidgetRenderer* waveformWidget,
        ::WaveformRendererAbstract::PositionSource type)
        : ::WaveformRenderMarkBase(waveformWidget, false),
          m_beatsUntilMark(0),
          m_timeUntilMark(0.0),
          m_currentBeatPosition(0.0),
          m_nextBeatPosition(0.0),
          m_isSlipRenderer(type == ::WaveformRendererAbstract::Slip),
          m_playPosHeight(0.f),
          m_playPosDevicePixelRatio(0.f),
          m_untilMarkShowBeats{false},
          m_untilMarkShowTime(false),
          m_untilMarkAlign(Qt::AlignVCenter),
          m_untilMarkTextSize(0),
          m_untilMarkTextHeightLimit(0.0) {
    {
        auto pNode = std::make_unique<Node>();
        m_pRangeNodesParent = pNode.get();
        appendChildNode(std::move(pNode));
    }

    {
        auto pNode = std::make_unique<Node>();
        m_pMarkLinesParent = pNode.get();
        appendChildNode(std::move(pNode));
    }

    {
        auto pNode = std::make_unique<Node>();
        m_pMarkNodesParent = pNode.get();
        appendChildNode(std::move(pNode));
    }

    {
        auto pNode = std::make_unique<DigitsRenderNode>();
        m_pDigitsRenderNode = pNode.get();
        appendChildNode(std::move(pNode));
    }

    {
        auto pNode = std::make_unique<GeometryNode>();
        m_pPlayPosNode = pNode.get();
        m_pPlayPosNode->initForRectangles<TextureMaterial>(1);
        appendChildNode(std::move(pNode));
    }

    auto* pWaveformWidgetFactory = WaveformWidgetFactory::instance();
    connect(pWaveformWidgetFactory,
            &WaveformWidgetFactory::untilMarkShowBeatsChanged,
            this,
            &WaveformRenderMark::setUntilMarkShowBeats);
    connect(pWaveformWidgetFactory,
            &WaveformWidgetFactory::untilMarkShowTimeChanged,
            this,
            &WaveformRenderMark::setUntilMarkShowTime);
    connect(pWaveformWidgetFactory,
            &WaveformWidgetFactory::untilMarkAlignChanged,
            this,
            &WaveformRenderMark::setUntilMarkAlign);
    connect(pWaveformWidgetFactory,
            &WaveformWidgetFactory::untilMarkTextPointSizeChanged,
            this,
            &WaveformRenderMark::setUntilMarkTextSize);
    connect(pWaveformWidgetFactory,
            &WaveformWidgetFactory::untilMarkTextHeightLimitChanged,
            this,
            &WaveformRenderMark::setUntilMarkTextHeightLimit);
}

void allshader::WaveformRenderMark::draw(QPainter*, QPaintEvent*) {
    DEBUG_ASSERT(false);
}

void allshader::WaveformRenderMark::setup(const QDomNode& node, const SkinContext& context) {
    ::WaveformRenderMarkBase::setup(node, context);
    auto* pWaveformWidgetFactory = WaveformWidgetFactory::instance();

    m_untilMarkShowBeats = pWaveformWidgetFactory->getUntilMarkShowBeats();
    m_untilMarkShowTime = pWaveformWidgetFactory->getUntilMarkShowTime();
    m_untilMarkAlign = pWaveformWidgetFactory->getUntilMarkAlign();

    m_untilMarkTextSize =
            pWaveformWidgetFactory->getUntilMarkTextPointSize();
    m_untilMarkTextHeightLimit =
            pWaveformWidgetFactory
                    ->getUntilMarkTextHeightLimit(); // proportion of waveform
                                                     // height

    m_playMarkerForegroundColor = m_waveformRenderer->getWaveformSignalColors()->getPlayPosColor();
    m_playMarkerBackgroundColor = m_waveformRenderer->getWaveformSignalColors()->getBgColor();
}

bool allshader::WaveformRenderMark::init() {
    m_pTimeRemainingControl = std::make_unique<ControlProxy>(
            m_waveformRenderer->getGroup(), "time_remaining");
    ::WaveformRenderMarkBase::init();
    return true;
}

void allshader::WaveformRenderMark::updateRangeNode(GeometryNode* pNode,
        const QRectF& rect,
        QColor color) {
    // draw a gradient towards transparency at the upper and lower 25% of the waveform view

    const float qh = static_cast<float>(std::floor(rect.height() * 0.25));
    const float posx1 = static_cast<float>(rect.x());
    const float posx2 = static_cast<float>(rect.x() + rect.width());
    const float posy1 = static_cast<float>(rect.y());
    const float posy2 = static_cast<float>(rect.y()) + qh;
    const float posy3 = static_cast<float>(rect.y() + rect.height()) - qh;
    const float posy4 = static_cast<float>(rect.y() + rect.height());

    float r, g, b, a;

    getRgbF(color, &r, &g, &b, &a);

    RGBAVertexUpdater vertexUpdater{pNode->geometry().vertexDataAs<Geometry::RGBAColoredPoint2D>()};
    vertexUpdater.addRectangleVGradient(
            {posx1, posy1}, {posx2, posy2}, {r, g, b, a}, {r, g, b, 0.f});
    vertexUpdater.addRectangleVGradient(
            {posx1, posy4}, {posx2, posy3}, {r, g, b, a}, {r, g, b, 0.f});
}

bool allshader::WaveformRenderMark::isSubtreeBlocked() const {
    return m_isSlipRenderer && !m_waveformRenderer->isSlipActive();
}

void allshader::WaveformRenderMark::update() {
    if (isSubtreeBlocked()) {
        return;
    }

    // For each WaveformMark we create a GeometryNode with Texture
    // (in updateMarkImage). Of these GeometryNodes, we append the
    // the ones that need to be shown on screen as children to
    // m_pMarkNodesParent (transferring ownership).
    //
    // At the beginning of a new frame, we remove all the child nodes
    // from m_pMarkNodesParent and store each with their mark
    // (transferring ownership). Later in this function we move the
    // visible nodes back to m_pMarkNodesParent children.
    for (auto* pParent : {m_pMarkLinesParent, m_pMarkNodesParent}) {
        while (auto* pChild = pParent->firstChild()) {
            auto pNode = pParent->detachChildNode(pChild);
            WaveformMarkNode* pWaveformMarkNode = static_cast<WaveformMarkNode*>(pNode.get());
            // Determine its WaveformMark
            auto* pMark = pWaveformMarkNode->m_pOwner;
            auto* pGraphics = static_cast<WaveformMarkNodeGraphics*>(pMark->m_pGraphics.get());
            // Store the node with the WaveformMark
            pGraphics->attachNode(std::move(pNode));
        }
    }

    auto positionType = m_isSlipRenderer ? ::WaveformRendererAbstract::Slip
                                         : ::WaveformRendererAbstract::Play;
    bool slipActive = m_waveformRenderer->isSlipActive();

    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();
    QList<WaveformWidgetRenderer::WaveformMarkOnScreen> marksOnScreen;

    auto roundToPixel = createFunctionRoundToPixel(devicePixelRatio);

    for (const auto& pMark : std::as_const(m_marks)) {
        pMark->setBreadth(slipActive ? m_waveformRenderer->getBreadth() / 2
                                     : m_waveformRenderer->getBreadth());
    }

    updatePlayPosMarkTexture(m_waveformRenderer->getContext());

    // Generate initial node or update its texture if needed for each of
    // the WaveformMarks (in which case updateMarkImage is called)
    // (Will create textures so requires OpenGL context)
    updateMarkImages();

    const double playPosition = m_waveformRenderer->getTruePosSample(positionType);
    double nextMarkPosition = std::numeric_limits<double>::max();

    GeometryNode* pRangeChild = static_cast<GeometryNode*>(m_pRangeNodesParent->firstChild());

    for (const auto& pMark : std::as_const(m_marks)) {
        if (!pMark->isValid()) {
            continue;
        }

        const double samplePosition = pMark->getSamplePosition();

        if (samplePosition == Cue::kNoPosition) {
            continue;
        }

        auto* pMarkGraphics = pMark->m_pGraphics.get();
        auto* pMarkNodeGraphics = static_cast<WaveformMarkNodeGraphics*>(pMarkGraphics);
        if (!pMarkGraphics) { // is this even possible?
            continue;
        }

        const float currentMarkPos = static_cast<float>(
                m_waveformRenderer->transformSamplePositionInRendererWorld(
                        samplePosition, positionType));
        if (pMark->isShowUntilNext() &&
                samplePosition >= playPosition + 1.0 &&
                samplePosition < nextMarkPosition) {
            nextMarkPosition = samplePosition;
        }
        const double sampleEndPosition = pMark->getSampleEndPosition();

        const float markWidth = pMarkNodeGraphics->textureWidth() / devicePixelRatio;
        const float drawOffset = currentMarkPos + pMark->getOffset();

        bool visible = false;
        // Check if the current point needs to be displayed.
        if (drawOffset > -markWidth &&
                drawOffset < m_waveformRenderer->getLength()) {
            pMarkNodeGraphics->update(
                    roundToPixel(drawOffset),
                    !m_isSlipRenderer && slipActive
                            ? roundToPixel(m_waveformRenderer->getBreadth() / 2.f)
                            : 0,
                    devicePixelRatio);

            // transfer back to the parents' children, for rendering: lines
            // under one parent, label boxes under a later sibling, so no
            // mark's line can cross another mark's box.
            m_pMarkLinesParent->appendChildNode(pMarkNodeGraphics->detachLineNode());
            if (pMarkNodeGraphics->hasLabel()) {
                m_pMarkNodesParent->appendChildNode(pMarkNodeGraphics->detachLabelNode());
            }

            visible = true;
        }

        // Check if the range needs to be displayed.
        if (samplePosition != sampleEndPosition && sampleEndPosition != Cue::kNoPosition) {
            DEBUG_ASSERT(samplePosition < sampleEndPosition);
            const float currentMarkEndPos = static_cast<float>(
                    m_waveformRenderer->transformSamplePositionInRendererWorld(
                            sampleEndPosition, positionType));
            if (visible || currentMarkEndPos > 0.f) {
                QColor color = pMark->fillColor();
                color.setAlphaF(0.4f);

                // Reuse, or create new when needed
                if (!pRangeChild) {
                    auto pNode = std::make_unique<GeometryNode>();
                    pNode->initForRectangles<RGBAMaterial>(2);
                    pRangeChild = pNode.get();
                    m_pRangeNodesParent->appendChildNode(std::move(pNode));
                }

                updateRangeNode(pRangeChild,
                        QRectF(QPointF(roundToPixel(currentMarkPos), 0.f),
                                QPointF(roundToPixel(currentMarkEndPos),
                                        roundToPixel(m_waveformRenderer->getBreadth()))),
                        color);

                visible = true;
                pRangeChild = static_cast<GeometryNode*>(pRangeChild->nextSibling());
            }
        }

        if (visible) {
            marksOnScreen.append(
                    WaveformWidgetRenderer::WaveformMarkOnScreen{
                            pMark, static_cast<int>(drawOffset)});
        }
    }

    // Remove unused nodes
    while (pRangeChild) {
        auto* pNextChild = static_cast<GeometryNode*>(pRangeChild->nextSibling());
        auto pNode = m_pRangeNodesParent->detachChildNode(pRangeChild);
        pRangeChild = pNextChild;
    }

    m_waveformRenderer->setMarkPositions(marksOnScreen);

    const float playMarkerPos = static_cast<float>(m_waveformRenderer->getPlayMarkerPosition() *
            m_waveformRenderer->getLength());
    {
        const float drawOffset = roundToPixel(playMarkerPos + kPlayPosOffset);
        TexturedVertexUpdater vertexUpdater{
                m_pPlayPosNode->geometry()
                        .vertexDataAs<Geometry::TexturedPoint2D>()};
        vertexUpdater.addRectangle({drawOffset, 0.f},
                {drawOffset + kPlayPosWidth, static_cast<float>(m_waveformRenderer->getBreadth())},
                {0.f, 0.f},
                {1.f, 1.f});
    }

    if (m_untilMarkShowBeats || m_untilMarkShowTime) {
        updateUntilMark(playPosition, nextMarkPosition);
        updateDigitsNodeForUntilMark(roundToPixel(playMarkerPos + 20.f));
    } else {
        m_pDigitsRenderNode->clear();
    }
}

void allshader::WaveformRenderMark::updateDigitsNodeForUntilMark(float x) {
    const auto untilMarkMaxHeightForText = getMaxHeightForText(m_untilMarkTextHeightLimit);

    m_pDigitsRenderNode->updateTexture(m_waveformRenderer->getContext(),
            m_untilMarkTextSize,
            untilMarkMaxHeightForText,
            m_waveformRenderer->getDevicePixelRatio());

    if (m_timeUntilMark == 0.0) {
        m_pDigitsRenderNode->clear();
        return;
    }
    const float ch = m_pDigitsRenderNode->height();

    float y = m_untilMarkAlign == Qt::AlignTop ? 0.f
            : m_untilMarkAlign == Qt::AlignBottom
            ? m_waveformRenderer->getBreadth() - ch
            : m_waveformRenderer->getBreadth() / 2.f;

    bool multiLine = m_untilMarkShowBeats && m_untilMarkShowTime &&
            ch * 2.f < untilMarkMaxHeightForText;

    if (multiLine) {
        if (m_untilMarkAlign != Qt::AlignTop) {
            y -= ch;
        }
    } else {
        if (m_untilMarkAlign != Qt::AlignTop && m_untilMarkAlign != Qt::AlignBottom) {
            // center
            y -= ch / 2.f;
        }
    }

    m_pDigitsRenderNode->update(
            x,
            y,
            multiLine,
            m_untilMarkShowBeats ? QString::number(m_beatsUntilMark) : QString{},
            m_untilMarkShowTime ? timeSecToString(m_timeUntilMark) : QString{});
}

// Generate the texture used to draw the play position marker.
// Note that in the legacy waveform widgets this is drawn directly
// in the WaveformWidgetRenderer itself. Doing it here is cleaner.
void allshader::WaveformRenderMark::updatePlayPosMarkTexture(rendergraph::Context* pContext) {
    float imgWidth;
    float imgHeight;

    const float height = m_waveformRenderer->getBreadth();
    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();

    if (height == m_playPosHeight && devicePixelRatio == m_playPosDevicePixelRatio) {
        return;
    }
    m_playPosHeight = height;
    m_playPosDevicePixelRatio = devicePixelRatio;

    const float lineX = 5.5f;

    imgWidth = kPlayPosWidth;
    imgHeight = height;

    const QSize size{static_cast<int>(std::lround(imgWidth * devicePixelRatio)),
            static_cast<int>(std::lround(imgHeight * devicePixelRatio))};

    if (size.width() <= 0 || size.height() <= 0) {
        return;
    }

    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    VERIFY_OR_DEBUG_ASSERT(!image.isNull()) {
        return;
    }
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(QColor(0, 0, 0, 0).rgba());

    // See comment on use of QPainter at top of file
    QPainter painter;

    painter.begin(&image);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.setWorldMatrixEnabled(false);

    // draw dim outlines to increase playpos/waveform contrast
    painter.setPen(m_playMarkerBackgroundColor);
    painter.setOpacity(0.5);
    // lines next to playpos
    // Note: don't draw lines where they would overlap the triangles,
    // otherwise both translucent strokes add up to a darker tone.
    painter.drawLine(QLineF(lineX + 1.f, 4.f, lineX + 1.f, imgHeight));
    painter.drawLine(QLineF(lineX - 1.f, 4.f, lineX - 1.f, imgHeight));

    // triangle at top edge
    // Increase line/waveform contrast
    painter.setOpacity(0.8);
    {
        QPointF baseL = QPointF(lineX - 5.f, 0.f);
        QPointF baseR = QPointF(lineX + 5.f, 0.f);
        QPointF tip = QPointF(lineX, 5.f);
        drawTriangle(&painter, m_playMarkerBackgroundColor, baseL, baseR, tip);
    }
    // draw colored play position indicators
    painter.setPen(m_playMarkerForegroundColor);
    painter.setOpacity(1.0);
    // play position line
    painter.drawLine(QLineF(lineX, 0.f, lineX, imgHeight));
    // triangle at top edge
    {
        QPointF baseL = QPointF(lineX - 4.f, 0.f);
        QPointF baseR = QPointF(lineX + 4.f, 0.f);
        QPointF tip = QPointF(lineX, 4.f);
        drawTriangle(&painter, m_playMarkerForegroundColor, baseL, baseR, tip);
    }
    painter.end();

    dynamic_cast<TextureMaterial&>(m_pPlayPosNode->material())
            .setTexture(std::make_unique<Texture>(pContext, image));
}

void allshader::WaveformRenderMark::drawTriangle(QPainter* painter,
        const QBrush& fillColor,
        QPointF baseL,
        QPointF baseR,
        QPointF tip) {
    QPainterPath triangle;
    painter->setPen(Qt::NoPen);
    triangle.moveTo(baseL);
    triangle.lineTo(tip);
    triangle.lineTo(baseR);
    triangle.closeSubpath();
    painter->fillPath(triangle, fillColor);
}

void allshader::WaveformRenderMark::updateMarkImage(WaveformMarkPointer pMark) {
    if (!pMark->m_pGraphics) {
        pMark->m_pGraphics =
                std::make_unique<WaveformMarkNodeGraphics>(pMark.get(),
                        m_waveformRenderer->getContext(),
                        pMark->generateImage(
                                m_waveformRenderer->getDevicePixelRatio()));
    } else {
        auto* pGraphics = static_cast<WaveformMarkNodeGraphics*>(pMark->m_pGraphics.get());
        pGraphics->updateTexture(m_waveformRenderer->getContext(),
                pMark->generateImage(
                        m_waveformRenderer->getDevicePixelRatio()));
    }
}

void allshader::WaveformRenderMark::updateUntilMark(
        double playPosition, double nextMarkPosition) {
    m_beatsUntilMark = 0;
    m_timeUntilMark = 0.0;
    if (nextMarkPosition == std::numeric_limits<double>::max()) {
        return;
    }

    TrackPointer trackInfo = m_waveformRenderer->getTrackInfo();

    if (!trackInfo) {
        return;
    }

    const double endPosition = m_waveformRenderer->getTrackSamples();
    const double remainingTime = m_pTimeRemainingControl->get();

    mixxx::BeatsPointer trackBeats = trackInfo->getBeats();
    if (!trackBeats) {
        return;
    }

    auto itA = trackBeats->iteratorFrom(
            mixxx::audio::FramePos::fromEngineSamplePos(playPosition));
    auto itB = trackBeats->iteratorFrom(
            mixxx::audio::FramePos::fromEngineSamplePos(nextMarkPosition));

    // itB is the beat at or after the nextMarkPosition.
    if (itB->toEngineSamplePos() > nextMarkPosition) {
        // if itB is after nextMarkPosition, the previous beat might be closer
        // and it the one we are interested in
        if (nextMarkPosition - (itB - 1)->toEngineSamplePos() <
                itB->toEngineSamplePos() - nextMarkPosition) {
            itB--;
        }
    }

    if (std::abs(itA->toEngineSamplePos() - playPosition) < 1) {
        m_currentBeatPosition = itA->toEngineSamplePos();
        m_beatsUntilMark = std::distance(itA, itB);
        itA++;
        m_nextBeatPosition = itA->toEngineSamplePos();
    } else {
        m_nextBeatPosition = itA->toEngineSamplePos();
        itA--;
        m_currentBeatPosition = itA->toEngineSamplePos();
        m_beatsUntilMark = std::distance(itA, itB);
    }
    // As endPosition - playPosition corresponds with remainingTime,
    // we calculate the proportional part of nextMarkPosition - playPosition
    m_timeUntilMark = std::max(0.0,
            remainingTime * (nextMarkPosition - playPosition) /
                    (endPosition - playPosition));
}

float allshader::WaveformRenderMark::getMaxHeightForText(float proportion) const {
    return std::roundf(m_waveformRenderer->getBreadth() * proportion);
}

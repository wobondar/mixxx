#include "waveformmarkset.h"

#include <QtDebug>
#include <algorithm>
#include <set>
#include <vector>

#include "util/defs.h"

WaveformMarkSet::WaveformMarkSet() {
}

WaveformMarkSet::~WaveformMarkSet() {
    clear();
}

void WaveformMarkSet::setup(const QString& group, const QDomNode& node,
                            const SkinContext& context,
                            const WaveformSignalColors& signalColors) {
    // + 3 for cue_point, loop_start_position and loop_end_position
    m_marks.reserve(kMaxNumberOfHotcues + 3);
    // Note: m_hotCueMarks does not support reserving space

    std::set<QString> controlItemSet;
    bool hasDefaultMark = false;

    QDomNode child = node.firstChild();
    QDomNode defaultChild;
    int priority = 0;
    while (!child.isNull()) {
        if (child.nodeName() == "DefaultMark") {
            m_pDefaultMark = WaveformMarkPointer::create(
                    group, child, context, --priority, signalColors);
            hasDefaultMark = true;
            defaultChild = child;
        } else if (child.nodeName() == "Mark") {
            auto pMark = WaveformMarkPointer::create(
                    group, child, context, --priority, signalColors);
            if (pMark->isValid()) {
                // guarantee uniqueness even if there is a misdesigned skin
                QString item = pMark->getItem();
                if (!controlItemSet.insert(item).second) {
                    qWarning() << "WaveformRenderMark::setup - redefinition of" << item;
                } else  {
                    addMark(pMark);
                    if (pMark->getHotCue() >= 0) {
                        m_hotCueMarks.insert(pMark->getHotCue(), pMark);
                    }
                }
            }
        }
        child = child.nextSibling();
    }

    // check if there is a default mark and compare declared
    // and to create all missing hot_cues
    if (hasDefaultMark) {
        for (int i = 0; i < kMaxNumberOfHotcues; ++i) {
            if (m_hotCueMarks.value(i).isNull()) {
                // qDebug() << "WaveformRenderMark::setup - Automatic mark" << hotCueControlItem;
                auto pMark = WaveformMarkPointer::create(
                        group, defaultChild, context, i, signalColors, i);
                m_marks.push_front(pMark);
                m_hotCueMarks.insert(pMark->getHotCue(), pMark);
            }
        }
    }
}

void WaveformMarkSet::setDefault(const QString& group,
        const DefaultMarkerStyle& model,
        const WaveformSignalColors& signalColors) {
    m_pDefaultMark = WaveformMarkPointer::create(

            group,
            model.positionControl,
            model.visibilityControl,
            model.textColor,
            model.markAlign,
            model.text,
            model.pixmapPath,
            model.iconPath,
            model.color,
            0,
            Cue::kNoHotCue,
            signalColors);
    for (int i = 0; i < kMaxNumberOfHotcues; ++i) {
        if (m_hotCueMarks.value(i).isNull()) {
            auto pMark = WaveformMarkPointer::create(

                    group,
                    model.positionControl,
                    model.visibilityControl,
                    model.textColor,
                    model.markAlign,
                    model.text,
                    model.pixmapPath,
                    model.iconPath,
                    model.color,
                    i,
                    i,
                    signalColors);
            m_marks.push_front(pMark);
            m_hotCueMarks.insert(pMark->getHotCue(), pMark);
        }
    }
}

WaveformMarkPointer WaveformMarkSet::getHotCueMark(int hotCue) const {
    return m_hotCueMarks.value(hotCue);
}

WaveformMarkPointer WaveformMarkSet::getDefaultMark() const {
    return m_pDefaultMark;
}

void WaveformMarkSet::setBreadth(float breadth) {
    for (auto& pMark : m_marks) {
        pMark->setBreadth(breadth);
    }
}

void WaveformMarkSet::update() {
    std::map<WaveformMarkSortKey, WaveformMarkPointer> map;
    for (const auto& pMark : std::as_const(m_marks)) {
        if (pMark->isValid() && pMark->isVisible()) {
            double samplePosition = pMark->getSamplePosition();
            if (samplePosition != Cue::kNoPosition) {
                // Create a stable key for sorting, because the WaveformMark's samplePosition is a
                // ControlObject which can change at any time by other threads. Such a change causes
                // another updateCues() call, rebuilding map.
                auto key = WaveformMarkSortKey(samplePosition, pMark->getPriority());
                map.emplace(key, pMark);
            }
        }
    }

    m_marksToRender.clear();
    m_marksToRender.reserve(static_cast<QList<WaveformMarkPointer>::size_type>(map.size()));
    std::transform(map.begin(),
            map.end(),
            std::back_inserter(m_marksToRender),
            [](auto const& pair) { return pair.second; });

    // Marks this close ride the same line on screen: a cue quantized to the
    // engine's beatgrid and the same beat imported from another program
    // differ by a handful of samples, never by a musical distance.
    constexpr double kStackToleranceSamples = 500.0;

    // Avoid overlapping marks by stacking each group of near-coincident marks
    // per vertical lane. Levels follow mark priority, not sub-sample position,
    // so the stack order is stable: loop marks at the edge, then the cue,
    // then hotcues.
    auto groupBegin = m_marksToRender.begin();
    while (groupBegin != m_marksToRender.end()) {
        const double anchorPosition = (*groupBegin)->getSamplePosition();
        auto groupEnd = groupBegin + 1;
        while (groupEnd != m_marksToRender.end() &&
                (*groupEnd)->getSamplePosition() <=
                        anchorPosition + kStackToleranceSamples) {
            ++groupEnd;
        }
        std::map<Qt::Alignment, std::vector<WaveformMarkPointer>> lanes;
        for (auto it = groupBegin; it != groupEnd; ++it) {
            lanes[(*it)->m_align & Qt::AlignVertical_Mask].push_back(*it);
        }
        for (auto& [align, members] : lanes) {
            std::sort(members.begin(),
                    members.end(),
                    [](const WaveformMarkPointer& pA, const WaveformMarkPointer& pB) {
                        return pA->getPriority() < pB->getPriority();
                    });
            int level = 0;
            for (auto& pMark : members) {
                pMark->setLevel(level++);
            }
        }
        groupBegin = groupEnd;
    }
}

WaveformMarkPointer WaveformMarkSet::findHoveredMark(
        QPoint pos, Qt::Orientation orientation) const {
    // Non-hotcue marks (intro/outro cues, main cue, loop in/out) are sorted
    // before hotcues in m_marksToRender so if there is a hotcue in the same
    // location, the hotcue gets rendered on top. When right clicking, the
    // the hotcue rendered on top must be assigned to m_pHoveredMark to show
    // the CueMenuPopup. To accomplish this, m_marksToRender is iterated in
    // reverse and the loop breaks as soon as m_pHoveredMark is set.
    for (auto it = m_marksToRender.crbegin(); it != m_marksToRender.crend(); ++it) {
        const WaveformMarkPointer& pMark = *it;
        if (pMark->contains(pos, orientation)) {
            return pMark;
        }
    }
    for (auto it = m_marksToRender.crbegin(); it != m_marksToRender.crend(); ++it) {
        const WaveformMarkPointer& pMark = *it;
        if (pMark->lineHovered(pos, orientation)) {
            return pMark;
        }
    }
    return nullptr;
}

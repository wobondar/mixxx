#pragma once

#include <QColor>
#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "preferences/usersettings.h"
#include "sources/audiosource.h"
#include "stems/spleeterprocessor.h"
#include "stems/stemtrack.h"
#include "track/track_decl.h"
#include "util/types.h"

namespace mixxx {

/// Stems presented to the user for a stems mode. Model stems are folded
/// into these through `fold` (model stem name to presented index).
struct StemPresentation {
    int numStems = 0;
    QStringList names;
    QList<QColor> colors;
    std::map<QString, int> fold;
    QString modelSubdir;
};

/// Background separation of loaded tracks into stems. One worker thread
/// shares the model sessions between decks and interleaves their splits,
/// following each deck's playhead: the region under the playhead is done
/// first, then everything after it, then the head of the track.
///
/// Preferences ([LiveStems]): mode (0 disables, otherwise the stem count),
/// bins, threads, model_dir (defaults to <settings>/stemmodels).
class StemEstimator : public QObject {
    Q_OBJECT
  public:
    static StemEstimator* instance() {
        return s_pInstance;
    }
    static void initialize(UserSettingsPointer pConfig);
    static void shutdown();

    ~StemEstimator() override;

    bool isEnabled() const {
        return m_pProcessor && m_pProcessor->isValid();
    }
    const StemPresentation& presentation() const {
        return m_presentation;
    }

    /// Registers a loaded track for separation and returns the buffer the
    /// deck mixes from. Null when disabled or the track cannot be separated.
    /// Main thread.
    StemTrackPointer requestTrack(const QString& group, TrackPointer pTrack, SINT numFrames);
    /// Cancels the deck's job and drops its buffer. Main thread.
    void releaseTrack(const QString& group);

  private:
    struct Job {
        TrackPointer pTrack;
        StemTrackPointer pStemTrack;
        AudioSourcePointer pAudioSource;
        SpleeterProcessor::Run run;
        std::vector<int> fold;
        bool failed = false;
        std::atomic<bool> cancelled{false};
    };

    explicit StemEstimator(UserSettingsPointer pConfig);
    void workerLoop();
    /// Processes one split of the job; returns false when the job has
    /// nothing left to do.
    bool step(Job* pJob);
    bool openSource(Job* pJob);
    void readMix(Job* pJob, SINT frame, SINT numFrames, float* out);

    static StemEstimator* s_pInstance;

    std::unique_ptr<SpleeterProcessor> m_pProcessor;
    StemPresentation m_presentation;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::map<QString, std::shared_ptr<Job>> m_jobs;
    std::atomic<bool> m_stop;
    std::thread m_thread;
};

} // namespace mixxx

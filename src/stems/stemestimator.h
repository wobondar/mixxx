#pragma once

#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <algorithm>
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

/// Configuration shared by the estimator and its preferences page.
namespace stemconfig {
constexpr const char* kGroup = "[LiveStems]";
constexpr int kDefaultMode = 0;
constexpr int kDefaultBins = 1536;
constexpr int kDefaultThreads = 1;
constexpr bool kDefaultCache = true;
constexpr int kDefaultCacheMaxMb = 50 * 1024;
constexpr int kDefaultPlayheadRegions = 1;
constexpr const char* kCacheSuffix = ".stems";

inline QString defaultModelDirectory(const UserSettingsPointer& pConfig) {
    return QDir(pConfig->getSettingsPath()).filePath(QStringLiteral("stemmodels"));
}
inline QString modelDirectory(const UserSettingsPointer& pConfig) {
    return pConfig->getValue(ConfigKey(kGroup, "model_dir"), defaultModelDirectory(pConfig));
}
inline QString cacheDirectory(const UserSettingsPointer& pConfig) {
    return QDir(pConfig->getSettingsPath()).filePath(QStringLiteral("stemcache"));
}
} // namespace stemconfig

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
/// Finished separations are kept on disk under <settings>/stemcache, keyed
/// by the file's content hash, model and bin count, and evicted least
/// recently used once the directory exceeds the configured size.
///
/// Preferences ([LiveStems]): mode (0 disables, otherwise the stem count),
/// deckN, bins, threads, model_dir (defaults to <settings>/stemmodels),
/// cache, cache_max_mb, playhead_regions.
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
    /// Separation is opt-in per deck ([LiveStems] deckN). Deck and cache
    /// preferences are read per job, so they apply to the next track loaded
    /// without a restart.
    bool isDeckEnabled(const QString& group) const;

    /// Regions from the playhead onward that must be done before a deck
    /// reports its stems usable at the playhead. The preferences page sets
    /// it without a restart; the engine thread reads it every buffer.
    int playheadRegions() const {
        return m_playheadRegions.load(std::memory_order_relaxed);
    }
    void setPlayheadRegions(int regions) {
        m_playheadRegions.store(std::max(1, regions), std::memory_order_relaxed);
    }

    /// Registers a loaded track for separation and returns the buffer the
    /// deck mixes from. Null when disabled or the track cannot be separated.
    /// Main thread.
    StemTrackPointer requestTrack(
            const QString& group, TrackPointer pTrack, SINT numFrames, int sampleRate);
    /// Cancels the deck's job and drops its buffer. Main thread.
    void releaseTrack(const QString& group);
    /// The buffer a deck currently mixes from, for readers outside the
    /// engine. Null between releaseTrack and the next request.
    StemTrackPointer trackForGroup(const QString& group);

  private:
    struct Job {
        TrackPointer pTrack;
        StemTrackPointer pStemTrack;
        AudioSourcePointer pAudioSource;
        SpleeterProcessor::Run run;
        std::vector<int> fold;
        QString cachePath;
        bool cacheChecked = false;
        bool failed = false;
        std::atomic<bool> cancelled{false};
        // Wall clock since the first split and the time spent inside the
        // model for this job alone; the two differ while decks interleave.
        QElapsedTimer wall;
        qint64 busyMs = 0;
    };

    explicit StemEstimator(UserSettingsPointer pConfig);
    void workerLoop();
    /// Processes one split of the job; returns false when the job has
    /// nothing left to do.
    bool step(Job* pJob);
    bool openSource(Job* pJob);
    void readMix(Job* pJob, SINT frame, SINT numFrames, float* out);
    QString cachePathFor(const Job& job) const;
    bool loadFromCache(Job* pJob);
    void saveToCache(const Job& job);
    void evictCache(const QString& keep);

    static StemEstimator* s_pInstance;

    UserSettingsPointer m_pConfig;
    std::unique_ptr<SpleeterProcessor> m_pProcessor;
    StemPresentation m_presentation;
    int m_bins = 0;
    std::atomic<int> m_playheadRegions;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::map<QString, std::shared_ptr<Job>> m_jobs;
    // Jobs leave m_jobs when they finish; a deck's buffer outlives its job.
    std::map<QString, StemTrackPointer> m_deckTracks;
    std::atomic<bool> m_stop;
    std::thread m_thread;
};

} // namespace mixxx

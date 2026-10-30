#pragma once

#include "library_model.h"
#include "segments/segment_store.h"
#include "editor/range_stream.h"
#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QPointer>
#include <QVideoSink>
#include <array>

namespace introvert::ui {

class EditorModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *player READ player CONSTANT)
    Q_PROPERTY(double position READ position NOTIFY changed)
    Q_PROPERTY(double duration READ duration NOTIFY changed)
    Q_PROPERTY(double frameRate READ frameRate NOTIFY changed)
    Q_PROPERTY(bool playing READ playing NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool editingAllowed READ editingAllowed NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString episodeTitle READ episodeTitle NOTIFY changed)
    Q_PROPERTY(QVariantMap segmentValues READ segmentValues NOTIFY loaded)
    Q_PROPERTY(QVariantMap preferences READ preferences NOTIFY changed)
    Q_PROPERTY(QVariantList levels READ levels NOTIFY changed)
    Q_PROPERTY(bool hasPrevious READ hasPrevious NOTIFY changed)
    Q_PROPERTY(bool hasNext READ hasNext NOTIFY changed)
    Q_PROPERTY(bool muted READ muted NOTIFY changed)
    Q_PROPERTY(QVariantList subtitleTracks READ subtitleTracks NOTIFY subtitlesChanged)
    Q_PROPERTY(int activeSubtitleTrack READ activeSubtitleTrack NOTIFY subtitlesChanged)
    Q_PROPERTY(QString episodeId READ episodeId NOTIFY changed)
    Q_PROPERTY(QVariantList seasons READ seasons NOTIFY catalogChanged)
    Q_PROPERTY(QVariantList episodes READ episodes NOTIFY catalogChanged)
    Q_PROPERTY(int season READ season NOTIFY catalogChanged)
public:
    EditorModel(LibraryModel &library, segments::SegmentStore store, QObject *parent = nullptr);
    ~EditorModel() override;
    QObject *player() { return &m_player; }
    double position() const { return m_player.position() / 1000.; }
    double duration() const { return m_player.duration() / 1000.; }
    double frameRate() const;
    bool playing() const { return m_player.playbackState() == QMediaPlayer::PlayingState; }
    bool ready() const { return m_ready && !m_priming; }
    bool busy() const { return m_busy || m_priming; }
    bool editingAllowed() const { return m_context && !m_library.m_editLocks.blocked(m_context->detail.show,m_context->episode.seasonNumber.value_or(-1)); }
    QString message() const { return m_message; }
    QString episodeTitle() const;
    QVariantMap segmentValues() const;
    QVariantMap preferences() const { return m_preferences; }
    QVariantList levels() const { return {m_levels[0], m_levels[1]}; }
    bool hasPrevious() const { return neighbor(-1).has_value(); }
    bool hasNext() const { return neighbor(1).has_value(); }
    bool muted() const { return m_audio.isMuted(); }
    QVariantList subtitleTracks() const;
    int activeSubtitleTrack() const { return m_activeSubtitleTrack; }
    QString episodeId() const { return m_context ? m_context->episode.identity.itemId : QString(); }
    QVariantList seasons() const { return m_seasons; }
    QVariantList episodes() const { return m_episodes; }
    int season() const { return m_season; }
    Q_INVOKABLE void openEpisode(const QString &id);
    Q_INVOKABLE void save(const QVariantList &rows);
    Q_INVOKABLE void navigate(int direction);
    Q_INVOKABLE void selectSeason(int number);
    Q_INVOKABLE void attachVideoSink(QObject *sink);
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void togglePlayback();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void toggleMuted();
    Q_INVOKABLE void setActiveSubtitleTrack(int index);
    Q_INVOKABLE void setPreference(const QString &key, int value);
    Q_INVOKABLE void unload();
    void close();
private:
    enum class Kind { Load, Save };
    struct Result {
        segments::SegmentValues values;
        std::unique_ptr<editor::RangeStream> stream;
        QUrl file;
        QString message;
        bool edited = false;
    };
    using Work = std::function<Result(const providers::Cancellation &)>;
    std::optional<providers::Episode> neighbor(int direction) const;
    void open(const providers::Episode &episode);
    void submit(Kind kind, Work work);
    void finished(Kind kind, quint64 generation, const std::shared_ptr<Result> &result, const QString &error);
    void status(QMediaPlayer::MediaStatus value);
    void firstFrame(const QVideoFrame &frame);
    void bindSink(QVideoSink *sink);
    void updateSubtitlePresentation();
    void resetMedia();
    void libraryChanged();
    void refreshCatalog();
    LibraryModel &m_library;
    segments::SegmentStore m_store;
    QAudioOutput m_audio;
    QAudioBufferOutput m_buffer;
    QVideoSink m_videoSink;
    QMediaPlayer m_player;
    QPointer<QVideoSink> m_outputSink;
    QMetaObject::Connection m_sinkConnection, m_subtitleConnection, m_sinkDestroyedConnection;
    QThreadPool m_pool;
    providers::Cancellation m_cancel = std::make_shared<std::atomic_bool>(false);
    std::optional<EpisodeContext> m_context;
    segments::SegmentValues m_baseline;
    QVariantMap m_preferences;
    QVariantList m_seasons, m_episodes;
    int m_season = -1;
    int m_publishedSeason = -1;
    int m_activeSubtitleTrack = -1;
    std::array<double, 2> m_levels{0, 0};
    std::unique_ptr<editor::RangeStream> m_stream;
    std::optional<double> m_pendingSeek;
    quint64 m_generation = 0;
    bool m_busy = false, m_ready = false, m_closed = false, m_priming = false, m_primeActive = false;
    QString m_message;
signals:
    void changed();
    void errorOccurred(const QString &message);
    void loaded();
    void saved();
    void catalogChanged();
    void subtitlesChanged();
};

} // namespace introvert::ui

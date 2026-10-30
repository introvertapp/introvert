#include "editor_model.h"
#include "editor/audio_levels.h"
#include "providers/access_retry.h"
#include <QDir>
#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QMediaMetaData>
#include <QRunnable>
#include <QPointer>
#include <QVideoFrame>
#include <cmath>
#include <algorithm>
#include <limits>

namespace introvert::ui {
using namespace providers;
using namespace segments;
namespace {
qint64 milliseconds(double seconds)
{
    const auto value = seconds * 1000;
    if (!std::isfinite(value) || value >= double(std::numeric_limits<qint64>::max())) return std::numeric_limits<qint64>::max();
    const auto floor = std::floor(value);
    return qint64(floor + (value - floor > .5 || (value - floor == .5 && std::fmod(floor, 2) != 0) ? 1 : 0));
}

std::optional<double> number(const QVariant &value)
{
    const auto type = value.metaType().id();
    if (type != QMetaType::Int && type != QMetaType::LongLong && type != QMetaType::UInt
        && type != QMetaType::ULongLong && type != QMetaType::Double && type != QMetaType::Float && type != QMetaType::Bool) return {};
    const auto result = value.toDouble();
    return std::isfinite(result) ? std::optional<double>(result) : std::nullopt;
}

double playbackRate(int index)
{
    const std::array<double, 3> rates{1., .5, 1. / 3};
    return index >= 0 && index < int(rates.size()) ? rates[index] : 1.;
}
}

EditorModel::EditorModel(LibraryModel &library, SegmentStore store, QObject *parent)
    : QObject(parent), m_library(library), m_store(std::move(store)), m_audio(this), m_buffer(this), m_videoSink(this), m_player(this),
      m_preferences(m_store.preferences())
{
    m_pool.setMaxThreadCount(1);
    m_player.setAudioOutput(&m_audio); m_player.setAudioBufferOutput(&m_buffer);
    m_player.setVideoSink(&m_videoSink);
    connect(&m_buffer, &QAudioBufferOutput::audioBufferReceived, this, [this](const QAudioBuffer &buffer) {
        if (!m_closed && playing() && !muted() && buffer.isValid()) { m_levels = editor::audioLevels(buffer, m_levels); emit changed(); }
    });
    connect(&m_player, &QMediaPlayer::positionChanged, this, &EditorModel::changed);
    connect(&m_player, &QMediaPlayer::durationChanged, this, &EditorModel::changed);
    connect(&m_player, &QMediaPlayer::tracksChanged, this, &EditorModel::subtitlesChanged);
    connect(&m_player, &QMediaPlayer::activeTracksChanged, this, &EditorModel::subtitlesChanged);
    connect(&m_player, &QMediaPlayer::playbackStateChanged, this, [this] {
        if (!playing()) m_levels = {0, 0};
        emit changed();
    });
    connect(&m_player, &QMediaPlayer::mediaStatusChanged, this, &EditorModel::status);
    connect(&m_player, &QMediaPlayer::errorOccurred, this, [this] {
        if (m_closed) return;
        m_priming = false; m_ready = false; m_primeActive = false;
        m_message = "Cannot decode episode media with Qt Multimedia."; emit changed();
    });
    connect(&library, &LibraryModel::changed, this, &EditorModel::libraryChanged);
}

EditorModel::~EditorModel() { close(); }

double EditorModel::frameRate() const
{
    bool ok = false;
    const auto rate = m_player.metaData().value(QMediaMetaData::VideoFrameRate).toDouble(&ok);
    return ok && std::isfinite(rate) && rate > 1 ? rate : 24.;
}

QString EditorModel::episodeTitle() const
{
    if (!m_context) return "No episode loaded";
    const auto &episode = m_context->episode;
    return "Episode " + (episode.episodeNumber ? QString::number(*episode.episodeNumber) : QString("None"))
        + " — " + episode.title.value_or(QString());
}

QVariantList EditorModel::subtitleTracks() const
{
    QVariantList result;
    for (const auto &track : m_player.subtitleTracks()) {
        auto label = track.stringValue(QMediaMetaData::Title).trimmed();
        const auto language = track.stringValue(QMediaMetaData::Language).trimmed();
        if (!language.isEmpty() && language != label)
            label = label.isEmpty() ? language : label + " · " + language;
        result.append(label.isEmpty() ? QString("Subtitle %1").arg(result.size() + 1) : label);
    }
    return result;
}

QVariantMap EditorModel::segmentValues() const
{
    QVariantMap result;
    for (auto row = m_baseline.cbegin(); row != m_baseline.cend(); ++row) result[row.key()] = QVariantList{row.value().first, row.value().second};
    return result;
}

std::optional<Episode> EditorModel::neighbor(int direction) const
{
    if (!m_context) return {};
    for (const auto &season : m_context->detail.seasons) if (season.number == m_context->episode.seasonNumber)
        for (qsizetype index = 0; index < season.episodes.size(); ++index) {
            const auto &identity = season.episodes[index].identity;
            const auto &current = m_context->episode.identity;
            if (identity.provider == current.provider && identity.itemId == current.itemId && identity.serverId == current.serverId) {
                const auto next = qint64(index) + qint64(direction);
                return next >= 0 && next < season.episodes.size() ? std::optional<Episode>(season.episodes[next]) : std::nullopt;
            }
        }
    return {};
}

void EditorModel::openEpisode(const QString &id)
{
    if (m_closed) return;
    const auto context = m_library.episodeContext(id);
    if (!context) { m_message = "Episode not found. Open the show again."; emit changed(); return; }
    m_context = context;
    open(context->episode);
}

void EditorModel::resetMedia()
{
    m_priming = false; m_ready = false; m_primeActive = false; m_pendingSeek.reset();
    m_activeSubtitleTrack = -1;
    m_player.stop(); m_player.setSource(QUrl());
    m_videoSink.setSubtitleText({}); m_videoSink.setVideoFrame({});
    if (m_outputSink) { m_outputSink->setSubtitleText({}); m_outputSink->setVideoFrame({}); }
    m_stream.reset(); m_levels = {0, 0}; m_ready = false;
    emit subtitlesChanged();
}

void EditorModel::open(const Episode &episode)
{
    m_cancel->store(true); m_cancel = std::make_shared<std::atomic_bool>(false); ++m_generation;
    resetMedia();
    if (m_outputSink) bindSink(m_outputSink);
    m_context->episode = episode; m_baseline.clear();
    m_season = int(episode.seasonNumber.value_or(-1)); refreshCatalog();
    m_busy = true; m_message.clear(); m_priming = true; emit loaded();
    const auto context = *m_context;
    std::optional<ProviderSettings> settings;
    try {
        if (episode.identity.provider != ProviderId::Local) settings = m_library.m_store.loadProvider(providerName(episode.identity.provider));
    } catch (...) {
        m_busy = false; m_priming = false; m_message = "Cannot access the segment database or episode media file."; emit changed(); return;
    }
    submit(Kind::Load, [this, context, settings](const Cancellation &cancel) {
        Result result; checkCancelled(cancel);
        result.values = m_store.load(context.detail.show, context.episode);
        result.edited = m_store.isEdited(context.detail.show, context.episode);
        if (settings) result.stream = editor::RangeStream::open(m_library.m_catalog.config(*settings, cancel), context.episode.identity.itemId, cancel);
        else {
            auto path = context.episode.mediaPath.value_or(QString());
            if (path == "~") path = QDir::homePath();
            else if (path.startsWith("~/")) path = QDir::homePath() + path.mid(1);
            if (path.isEmpty() || !QFileInfo(path).isAbsolute() || !readableMediaFile(path,cancel))
                result.message = "Episode media file is not readable";
            else result.file = QUrl::fromLocalFile(path);
        }
        checkCancelled(cancel); return result;
    });
    emit changed();
}

void EditorModel::submit(Kind kind, Work work)
{
    const auto generation = m_generation;
    const auto cancel = m_cancel;
    m_pool.start(QRunnable::create([this, kind, generation, cancel, work = std::move(work)] {
        std::shared_ptr<Result> result;
        QString error;
        try { result = std::make_shared<Result>(work(cancel)); }
        catch (const Cancelled &) {}
        catch (const SegmentError &failure) { error = QString::fromUtf8(failure.what()); }
        catch (const ProviderError &failure) { error = QString::fromUtf8(failure.what()); }
        catch (...) { error = "Cannot access the segment database or episode media file."; }
        QMetaObject::invokeMethod(this, [this, kind, generation, result, error] { finished(kind, generation, result, error); }, Qt::QueuedConnection);
    }));
}

void EditorModel::finished(Kind kind, quint64 generation, const std::shared_ptr<Result> &result, const QString &error)
{
    if (m_closed || generation != m_generation) return; // stale results release their stream by RAII
    m_busy = false;
    if (!error.isEmpty()) {
        if (kind == Kind::Load) m_priming = false;
        m_message = error;
    } else if (result && kind == Kind::Load) {
        m_baseline = result->values; m_message = result->message;
        m_library.updateSegments(m_context->revision, m_context->episode.identity.itemId, m_baseline, result->edited);
        emit loaded();
        const auto url = result->stream ? result->stream->url() : result->file;
        if (url.isEmpty()) m_priming = false;
        else {
            m_pendingSeek = 0.;
            for (const auto &key : {"intro", "credits", "recap", "preview"})
                if (m_baseline.contains(key)) { m_pendingSeek = m_baseline.value(key).first; break; }
            m_player.setPlaybackRate(playbackRate(m_preferences.value("speed").toInt()));
            m_stream = std::move(result->stream); m_player.setSource(url);
        }
    } else if (result && m_context) {
        m_baseline = result->values;
        m_library.updateSegments(m_context->revision, m_context->episode.identity.itemId, m_baseline, result->edited);
        emit loaded(); emit saved();
    }
    emit changed();
}

void EditorModel::save(const QVariantList &rows)
{
    if (m_closed || m_busy || !m_context) return;
    if(!editingAllowed()) {m_message="Season timestamps cannot be edited during detection";emit changed();return;}
    SegmentChanges changes;
    try {
        if (rows.size() != 4) throw SegmentError("Invalid timeline segments");
        const QStringList kinds{"recap", "intro", "credits", "preview"};
        for (qsizetype index = 0; index < rows.size(); ++index) {
            if (rows[index].metaType().id() != QMetaType::QVariantMap) throw SegmentError("Invalid timeline segments");
            const auto row = rows[index].toMap(); const auto start = row.value("start"), end = row.value("end");
            std::optional<QPair<double, double>> value;
            if ((!start.isNull() && start.isValid()) || (!end.isNull() && end.isValid())) {
                const auto a = number(start), b = number(end);
                if (!a || !b) throw SegmentError("Segment timestamps must be within the video and between 5 and 180 seconds long");
                value = qMakePair(*a, *b);
            }
            const auto baseline = m_baseline.contains(kinds[index]) ? std::optional<QPair<double, double>>(m_baseline.value(kinds[index])) : std::nullopt;
            if (value != baseline) changes[kinds[index]] = value;
        }
        if (changes.isEmpty()) throw SegmentError("No segment changes to save");
    } catch (const SegmentError &error) { m_message = QString::fromUtf8(error.what()); emit changed(); return; }
    const auto context = *m_context; const auto length = duration();
    m_busy = true; m_message.clear();
    submit(Kind::Save, [this, context, length, changes](const Cancellation &) {
        // A requested save remains atomic even if the dialog closes before its
        // worker starts. Only obsolete display results are discarded.
        const auto edit=m_library.m_editLocks.edit({{context.detail.show,context.episode.seasonNumber.value_or(-1)}});
        if(!edit)throw SegmentError("Season timestamps cannot be edited during detection");
        m_store.save(context.detail.show, context.episode, changes, length);
        Result result; result.values = m_store.load(context.detail.show, context.episode);
        result.edited = m_store.isEdited(context.detail.show, context.episode); return result;
    });
    emit changed();
}

void EditorModel::navigate(int direction) { if (!m_closed && !m_busy) if (const auto episode = neighbor(direction)) open(*episode); }
void EditorModel::selectSeason(int number)
{
    if (m_closed || !m_context || !m_seasons.contains(number)) return;
    m_season = number; refreshCatalog();
}
void EditorModel::refreshCatalog()
{
    QVariantList seasons, episodes;
    if (m_context) {
        for (const auto &group : m_context->detail.seasons) seasons.append(int(group.number.value_or(-1)));
        episodes = m_library.episodesForSeason(m_season);
    }
    // Catalog bindings do not rebuild on playback position/audio updates.
    if (m_seasons != seasons || m_episodes != episodes || m_publishedSeason != m_season) {
        m_seasons = seasons; m_episodes = episodes; m_publishedSeason = m_season; emit catalogChanged();
    }
}
void EditorModel::attachVideoSink(QObject *value)
{
    auto *sink = qobject_cast<QVideoSink *>(value);
    if (m_closed || !sink) return;
    m_videoSink.setRhi(sink->rhi());
    if (m_outputSink != sink) bindSink(sink);
}
void EditorModel::bindSink(QVideoSink *sink)
{
    disconnect(m_sinkConnection); disconnect(m_subtitleConnection); disconnect(m_sinkDestroyedConnection);
    m_outputSink = sink;
    m_sinkDestroyedConnection = connect(sink, &QObject::destroyed, this, [this] { m_videoSink.setRhi(nullptr); });
    m_sinkConnection = connect(&m_videoSink, &QVideoSink::videoFrameChanged, this,
        [this, generation = m_generation, guard = QPointer<QVideoSink>(sink)](const QVideoFrame &frame) {
            if (m_closed || !guard || generation != m_generation || m_outputSink != guard) return;
            guard->setSubtitleText(m_activeSubtitleTrack < 0 ? QString() : m_videoSink.subtitleText());
            guard->setVideoFrame(frame); firstFrame(frame);
        });
    m_subtitleConnection = connect(&m_videoSink, &QVideoSink::subtitleTextChanged, this,
        [this, generation = m_generation, guard = QPointer<QVideoSink>(sink)] {
            if (!m_closed && guard && generation == m_generation && m_outputSink == guard) updateSubtitlePresentation();
        });
}
void EditorModel::updateSubtitlePresentation()
{
    if (!m_outputSink) return;
    m_outputSink->setSubtitleText(m_activeSubtitleTrack < 0 ? QString() : m_videoSink.subtitleText());
    const auto frame = m_videoSink.videoFrame();
    if (frame.isValid()) {
        // Re-present the same decoded frame, including while paused. Both
        // updates occur together on the UI thread; playback never seeks.
        m_outputSink->setVideoFrame({}); m_outputSink->setVideoFrame(frame);
    }
}
void EditorModel::seek(double seconds)
{
    if (!m_closed && m_ready && std::isfinite(seconds)) m_player.setPosition(milliseconds(std::clamp(seconds, 0., duration())));
}
void EditorModel::togglePlayback()
{
    if (m_closed || !m_ready) return;
    if (playing()) m_player.pause(); else m_player.play();
}
void EditorModel::pause() { if (!m_closed) m_player.pause(); }
void EditorModel::toggleMuted()
{
    if (m_closed) return;
    m_audio.setMuted(!muted()); m_levels = {0, 0}; emit changed();
}
void EditorModel::setActiveSubtitleTrack(int index)
{
    if (m_closed || !ready() || index < -1 || index >= m_player.subtitleTracks().size()) return;
    if (index == m_activeSubtitleTrack) return;
    m_activeSubtitleTrack = index;
    if (index >= 0 && index != m_player.activeSubtitleTrack()) {
        m_videoSink.setSubtitleText({});
        m_player.setActiveSubtitleTrack(index);
    }
    // Keep the decoder selected when hidden. Qt rebuilds the demuxer on actual
    // track changes, which can disturb audio even when just disabling captions.
    updateSubtitlePresentation(); emit subtitlesChanged();
}
void EditorModel::setPreference(const QString &key, int value)
{
    if (m_closed) return;
    try {
        m_store.setPreference(key, value); m_preferences = m_store.preferences();
        if (key == "speed") m_player.setPlaybackRate(playbackRate(value));
    } catch (...) { m_message = "Cannot save editor preference"; }
    emit changed();
}
void EditorModel::status(QMediaPlayer::MediaStatus value)
{
    if (m_closed || !m_context) return;
    if (value == QMediaPlayer::LoadedMedia || value == QMediaPlayer::BufferedMedia) {
        m_ready = true;
        if (m_pendingSeek) {
            // Prepare the first subtitle decoder before playback starts. On/off
            // can then change presentation without touching audio or demuxing.
            if (!m_player.subtitleTracks().isEmpty()) m_player.setActiveSubtitleTrack(0);
            const auto position = *m_pendingSeek; m_pendingSeek.reset(); m_primeActive = true;
            // Qt's paused decoder prepares a video frame without starting audio.
            seek(position); m_player.pause();
        }
    }
    emit changed();
}
void EditorModel::firstFrame(const QVideoFrame &frame)
{
    if (m_closed || !m_priming || !m_primeActive || !frame.isValid()) return;
    m_priming = false; m_primeActive = false;
    if (m_preferences.value("autoPlay").toBool()) m_player.play();
    emit changed();
}
void EditorModel::libraryChanged()
{
    if (m_context) {
        const auto current = m_library.episodeContext(m_context->episode.identity.itemId);
        if (!current || current->revision != m_context->revision) unload();
        else {
            m_context->detail = current->detail;
            if(!editingAllowed()) {
                const auto values=m_library.m_segments.values.value(m_context->episode.identity.itemId);
                if(values!=m_baseline) {m_baseline=values;emit loaded();}
            }
            refreshCatalog();emit changed();
        }
    }
}
void EditorModel::unload()
{
    m_cancel->store(true); ++m_generation;
    resetMedia(); m_context.reset(); m_busy = false; m_season = -1; refreshCatalog(); emit changed();
}
void EditorModel::close()
{
    if (m_closed) return;
    m_closed = true; unload(); m_pool.waitForDone();
    // Deliver stale completions now, while dependencies and Qt objects remain
    // alive, so every late-created session is released before shutdown returns.
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    disconnect(&m_library, nullptr, this, nullptr);
}

} // namespace introvert::ui

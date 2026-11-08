#include "library_model.h"
#include <QRunnable>
#include <algorithm>
#include <cmath>
#include <limits>

namespace introvert::ui {
using namespace providers;
namespace {

QString disabledCatalogMessage(const ProviderSettings &saved, bool movies)
{
    if (movies)
        return saved.provider == "local" ? QStringLiteral("Movies can be imported from Plex, Emby or Jellyfin.")
            : QStringLiteral("Choose Movies or Both under Import in Settings → Media.");
    return QStringLiteral("Choose TV shows or Both under Import in Settings → Media.");
}

} // namespace

bool CatalogSelection::operator==(const CatalogSelection &other) const
{
    return listing == other.listing && saved.provider == other.saved.provider && saved.remoteUrl == other.saved.remoteUrl
        && saved.localRoot == other.saved.localRoot && saved.fetchImages == other.saved.fetchImages
        && saved.timeoutSeconds == other.saved.timeoutSeconds && saved.importSegments == other.saved.importSegments
        && saved.importMode == other.saved.importMode && saved.importMovieSegments == other.saved.importMovieSegments;
}

LibraryModel::LibraryModel(SettingsModel &settings, SettingsStore store, QString artworkRoot,
                           library::ProviderMap overrides, MetadataClient metadataClient, QObject *parent)
    : QObject(parent), m_editLocks(segments::EditLocks::forDatabase(store.databasePath())), m_settings(settings), m_store(std::move(store)),
      m_cache(std::move(artworkRoot), m_store.databasePath()), m_catalog(m_store, m_cache, std::move(overrides), [this](const QString &level, const QString &message) {
          QMetaObject::invokeMethod(this, [this, level, message] { if (!m_closed) emit diagnosticMessage(level, message); }, Qt::QueuedConnection);
      }),
      m_metadata(new LocalMetadataModel(*this, std::move(metadataClient)))
{
    m_pool.setMaxThreadCount(2);
    connect(&settings, &SettingsModel::changed, this, [this] { settingsChanged(); });
    connect(qobject_cast<ProviderSettingsModel *>(settings.mediaSettings()), &ProviderSettingsModel::saved,
            this, [this] { settingsChanged(true); });
}

LibraryModel::~LibraryModel() { close(); }
QObject *LibraryModel::localMetadata() const { return m_metadata; }

QString LibraryModel::status() const
{
    if (m_pending) return "Not connected";
    if (m_cached && m_key && !m_busy) return library::providerLabel(providerId(m_key->saved.provider));
    if (m_busy) return "Loading…";
    return m_key && m_state == "ready" && m_message.isEmpty()
        ? library::providerLabel(providerId(m_key->saved.provider)) : QString("Not connected");
}

QString LibraryModel::episodeCount() const
{
    qint64 count = 0;
    for (const auto &entry : m_shows) {
        const auto value = entry.toMap().value("episodes");
        if (value.metaType().id() == QMetaType::LongLong || value.metaType().id() == QMetaType::Int) count += value.toLongLong();
    }
    return QString::number(count);
}

std::optional<CatalogSelection> LibraryModel::settingsKey() const
{
    const auto provider = m_settings.mediaProvider();
    if (provider != "plex" && provider != "emby" && provider != "jellyfin" && provider != "local") return {};
    return CatalogSelection{m_store.loadProvider(provider), m_settings.listingType()};
}

void LibraryModel::settingsChanged(bool configuration)
{
    if (m_closed) return;
    try {
        const auto key = settingsKey();
        if (configuration || key != m_key) {
            if (configuration) m_movies.clear();
            if (m_browsing) load();
            else defer(key);
        }
    } catch (...) { refresh(); }
}

void LibraryModel::defer(const std::optional<CatalogSelection> &key)
{
    m_cancel->store(true); ++m_generation;
    m_showCancel->store(true); ++m_showGeneration; m_showActive = false; m_catalogLoading = false;
    m_rebuildGeneration.reset();
    m_pending = true; m_cached = false; m_busy = false; m_state = "ready"; m_message.clear();
    if (key != m_key) {
        m_shows.clear(); m_movies.clear(); m_episodes.clear(); m_show.clear(); m_movie.clear(); m_detail.reset(); m_segments = {};
    }
    m_movieDetail.reset(); m_movie.clear();
    m_key = key;
    emit changed();
}

void LibraryModel::setBrowsing(bool active)
{
    if (m_closed) return;
    m_browsing = active;
    if (active && m_pending) { m_pending = false; load(); }
}

void LibraryModel::setBrowsePage(const QString &page)
{
    if (m_closed) return;
    if (page != "home" && page != "movies") {
        setBrowsing(false);
        return;
    }
    if (!m_browsing)
        qobject_cast<ProviderSettingsModel *>(m_settings.mediaSettings())->flush();
    const bool movies = page == "movies";
    if (m_browseMovies == movies) {
        setBrowsing(true);
        return;
    }
    closeShow();
    m_browseMovies = movies;
    m_browsing = true;
    if (rebuildingCache()) { emit changed(); return; }
    m_pending = false;
    m_catalogLoading = false;
    load();
}

quint64 LibraryModel::begin()
{
    m_cancel->store(true); m_cancel = std::make_shared<std::atomic_bool>(false);
    m_catalogLoading = false;
    m_busy = true; m_state = "loading"; m_message.clear();
    return ++m_generation;
}

void LibraryModel::submit(Kind kind, quint64 generation, Work work)
{
    const auto cancel = detailRequest(kind) ? m_showCancel : m_cancel;
    const auto summary = m_imageSummary;
    m_pool.start(QRunnable::create([this, kind, generation, work = std::move(work), cancel, summary] {
        std::optional<Result> result;
        QString error;
        try {
            checkCancelled(cancel);
            result = work(cancel, generation);
            result->stats = detailRequest(kind) ? summary : m_cache.stats();
        } catch (const Cancelled &) { result.reset(); }
        catch (const ProviderError &failure) { result.reset(); error = QString::fromUtf8(failure.what()); }
        catch (const DatabaseError &failure) { result.reset(); error = QString::fromUtf8(failure.what()); }
        catch (...) { result.reset(); error = "Cannot load media. Check the saved configuration and app.db access, then retry."; }
        QMetaObject::invokeMethod(this, [this, kind, generation, result = std::move(result), error] {
            finished(kind, generation, result, error);
        }, Qt::QueuedConnection);
    }), detailRequest(kind) ? 1 : 0);
}

void LibraryModel::publish(quint64 generation, const library::CatalogResult &catalog)
{
    QMetaObject::invokeMethod(this, [this, generation, catalog] {
        if (m_closed || generation != m_generation) return;
        activeRows() = catalog.rows; m_cached = catalog.cached;
        if (!m_showActive) { m_message = catalog.message; m_busy = false; m_state = "ready"; }
        emit changed();
    }, Qt::QueuedConnection);
}

void LibraryModel::progress(quint64 generation, double value)
{
    QMetaObject::invokeMethod(this, [this, generation, value] {
        if (m_closed || generation != m_generation || !rebuildingCache()) return;
        m_rebuildProgress = std::max(m_rebuildProgress, std::min(.999, value)); emit changed();
    }, Qt::QueuedConnection);
}

void LibraryModel::load() { refresh(false); }
void LibraryModel::refresh() { refresh(true); }

void LibraryModel::refresh(bool force, bool clearLocal)
{
    if (m_closed) return;
    const auto previous = m_key;
    try { m_key = settingsKey(); }
    catch (...) {
        m_cancel->store(true); ++m_generation; m_key.reset(); m_detail.reset(); m_movieDetail.reset(); m_show.clear(); m_movie.clear();
        m_showCancel->store(true); ++m_showGeneration; m_showActive = false; m_catalogLoading = false;
        m_shows.clear(); m_movies.clear(); m_episodes.clear(); m_busy = false; m_state = "error";
        m_message = "Cannot load saved media configuration. Check application database access and retry.";
        emit errorOccurred(m_message);
        emit changed(); return;
    }
    if (!force && !clearLocal && m_key == previous && m_catalogLoading) return;
    m_showCancel->store(true); ++m_showGeneration; m_showActive = false;
    const auto generation = begin();
    m_catalogLoading = m_key.has_value();
    m_cached = false; m_detail.reset(); m_movieDetail.reset(); m_show.clear(); m_movie.clear(); m_episodes.clear();
    if (m_key != previous) { m_shows.clear(); m_movies.clear(); }
    if (!m_key) {
        activeRows().clear(); m_busy = false; m_state = "ready";
        m_message = m_settings.mediaProvider().isEmpty()
            ? "Configure a media provider in Settings → Media to load your library." : "Select a media provider in Media settings.";
        submit(Kind::Stats, generation, [](const Cancellation &, quint64) { return Result{}; });
    } else if (!clearLocal && (m_browseMovies ? !m_key->saved.importsMovies() : !m_key->saved.importsTv())) {
        activeRows().clear();
        m_catalogLoading = false; m_busy = false; m_state = "ready";
        m_message = disabledCatalogMessage(m_key->saved, m_browseMovies);
    } else {
        const auto key = *m_key;
        const bool movies = m_browseMovies && !clearLocal;
        const bool showDisabledMovies = m_browseMovies && clearLocal;
        const auto oldRows = movies ? m_movies : m_shows;
        submit(Kind::Library, generation, [this, key, force, clearLocal, oldRows, movies, showDisabledMovies](const Cancellation &cancel, quint64 gen) {
            Result result;
            const auto config = m_catalog.config(key.saved, cancel);
            const auto progress = [this, gen, showDisabledMovies](const library::CatalogResult &value) {
                if (!showDisabledMovies) publish(gen, value);
            };
            if (movies) result.movieCatalog = m_catalog.browseMovies(config, key.listing, force, oldRows, cancel, progress);
            else result.catalog = m_catalog.browse(config, key.listing, force, clearLocal, oldRows, cancel, progress);
            if (showDisabledMovies)
                result.movieCatalog = library::CatalogResult{{}, disabledCatalogMessage(key.saved, true)};
            return result;
        });
    }
    emit changed();
}

void LibraryModel::rebuildCache()
{
    if (m_closed || m_busy || rebuildingCache()) return;
    auto *media = qobject_cast<ProviderSettingsModel *>(m_settings.mediaSettings());
    media->flush();
    if (media->dirty()) return;
    try { m_key = settingsKey(); }
    catch (...) {
        emit cacheRebuildFailed("Cannot read the saved media configuration. Check Settings → Media and try again."); return;
    }
    if (!m_key || (m_key->saved.provider=="local" && m_metadata->busy())) return;
    const auto key = *m_key;
    m_previousState = m_state; m_previousMessage = m_message; m_previousCached = m_cached; m_previousPending = m_pending;
    const auto generation = begin();
    m_rebuildGeneration = generation; m_rebuildProgress = 0; m_pending = false;
    submit(Kind::Rebuild, generation, [this, key](const Cancellation &cancel, quint64 gen) {
        Result result;
        const auto config = m_catalog.config(key.saved, cancel);
        const bool tv = key.saved.importsTv(), movies = key.saved.importsMovies();
        if (tv)
            result.catalog = m_catalog.rebuild(config, key.listing, cancel,
                [this, gen, movies](double value) { progress(gen, movies ? value * .5 : value); });
        else result.catalog = library::CatalogResult{{}, disabledCatalogMessage(key.saved, false)};
        if (movies)
            result.movieCatalog = m_catalog.rebuildMovies(config, key.listing, cancel,
                [this, gen, tv](double value) { progress(gen, tv ? .5 + value * .5 : value); });
        else result.movieCatalog = library::CatalogResult{{}, disabledCatalogMessage(key.saved, true)};
        return result;
    });
    emit changed();
}

void LibraryModel::reloadLocalMetadata()
{
    if(m_closed || m_browseMovies || !m_key || m_key->saved.provider!="local")return;
    const auto key=*m_key;const auto generation=m_generation;const auto detailRevision=m_detailRevision;
    const auto showId=m_show.value("id").toString();const auto cancel=m_cancel;const auto before=m_segments;
    m_pool.start(QRunnable::create([this,key,generation,detailRevision,showId,cancel,before] {
        try {
            const auto config=m_catalog.config(key.saved,cancel);
            const auto catalog=m_catalog.browse(config,key.listing,false,false,{},cancel,[](const library::CatalogResult &){});
            std::optional<ShowDetail> detail;
            QVariantMap show;
            if(!showId.isEmpty()) {detail=m_catalog.detail(config,key.listing,showId,cancel);show=m_catalog.displayShow(config,detail->show,cancel);}
            const auto overlay=detail?segments::loadCatalogSegments(m_store.databasePath(),*detail,key.address()):segments::CatalogSegments();
            QMetaObject::invokeMethod(this,[this,key,generation,detailRevision,catalog,detail,show,before,overlay] {
                if(m_closed || generation!=m_generation || (!m_key || !(*m_key==key)))return;
                m_shows=catalog.rows;
                if(detail && detailRevision==m_detailRevision && m_detail && m_detail->show.identity.itemId==detail->show.identity.itemId) {
                    m_detail=*detail;m_show=show;
                    if(m_segments.values==before.values && m_segments.edited==before.edited && m_segments.statusRevisions==before.statusRevisions)m_segments=overlay;
                    QVariantList seasons;for(const auto &season:detail->seasons)seasons.append(season.number?QVariant(*season.number):QVariant());
                    m_show["seasonNumbers"]=seasons;m_episodes=episodesForSeason(m_season);
                }
                emit changed();
            },Qt::QueuedConnection);
        } catch(...) {}
    }));
}

void LibraryModel::clearImageCache()
{
    if (m_closed || m_busy) return;
    m_previousState = m_state; m_previousMessage = m_message;
    const auto generation = begin();
    submit(Kind::Images, generation, [this](const Cancellation &cancel, quint64) {
        checkCancelled(cancel); m_cache.clearImages(); return Result{};
    });
    emit changed();
}

void LibraryModel::clearLocalCache()
{
    if (!m_closed && m_settings.mediaProvider() == "local" && !m_metadata->busy()) refresh(true, true);
}

void LibraryModel::openShow(const QString &id) { openShowScoped(id, {}); }
void LibraryModel::openJobShow(const QString &id, const QString &scope) { openShowScoped(id, scope); }

quint64 LibraryModel::beginDetail()
{
    m_showCancel->store(true); m_showCancel = std::make_shared<std::atomic_bool>(false);
    m_showActive = true; m_busy = true; m_state = "loading"; m_message.clear();
    m_detail.reset(); m_movieDetail.reset(); m_episodes.clear(); m_show.clear(); m_movie.clear();
    return ++m_showGeneration;
}

void LibraryModel::openShowScoped(const QString &id, const QString &scope)
{
    if (m_closed || !m_key || m_browseMovies || !m_key->saved.importsTv()) return;
    const auto key = *m_key;
    const auto generation = beginDetail();
    for (const auto &row : m_shows) if (row.toMap().value("id").toString() == id) { m_show = row.toMap(); break; }
    submit(Kind::Show, generation, [this, key, id, scope](const Cancellation &cancel, quint64) {
        Result result;
        const auto config = m_catalog.config(key.saved, cancel);
        result.detail = m_catalog.detail(config, key.listing, id, cancel);
        if (!scope.isEmpty() && scope != providers::providerScope(result.detail->show.identity,
            key.saved.provider == "local" ? key.saved.localRoot : key.saved.remoteUrl))
            throw ProviderError("This job belongs to another media server. Select its connection in Settings → Media.");
        result.display = m_catalog.displayShow(config, result.detail->show, cancel);
        QVariantList seasons;
        for (const auto &season : result.detail->seasons) seasons.append(season.number ? QVariant(*season.number) : QVariant());
        result.display["seasonNumbers"] = seasons;
        checkCancelled(cancel);
        result.segments = segments::loadCatalogSegments(m_store.databasePath(), *result.detail, key.address());
        return result;
    });
    emit changed();
}

void LibraryModel::openMovie(const QString &id) { openMovieScoped(id, {}); }
void LibraryModel::openJobMovie(const QString &id, const QString &scope) { openMovieScoped(id, scope); }

void LibraryModel::openMovieScoped(const QString &id, const QString &scope)
{
    if (m_closed || !m_key || !m_browseMovies || !m_key->saved.importsMovies()) return;
    const auto item = std::find_if(m_movies.cbegin(), m_movies.cend(), [&id](const QVariant &value) {
        return value.toMap().value("id").toString() == id;
    });
    if (item == m_movies.cend() && scope.isEmpty()) return;
    const auto key = *m_key;
    const auto generation = beginDetail();
    m_movie = item == m_movies.cend() ? QVariantMap{{"id", id}} : item->toMap();
    submit(Kind::Movie, generation, [this, key, id, scope](const Cancellation &cancel, quint64) {
        Result result;
        const auto config = m_catalog.config(key.saved, cancel);
        const auto detail = m_catalog.movieDetail(config, key.listing, id, cancel);
        const auto &movie = detail.movie;
        const auto source = providerScope(movie.identity, key.address());
        if (!scope.isEmpty() && scope != source)
            throw ProviderError("This job belongs to another media server. Select its connection in Settings → Media.");
        result.display = m_catalog.displayMovie(config, movie, cancel, key.listing != "basic");
        ShowSummary title{movie.identity, movie.title};
        title.year = movie.year;
        title.externalIds = movie.externalIds;
        title.movieKey = "movie:" + source + ':' + movie.identity.itemId;
        Episode playable{movie.identity, movie.title};
        playable.durationMs = movie.durationMs;
        if (key.saved.importMovieSegments) playable.importedSegments = movieChapterSegments(detail);
        result.detail = ShowDetail{title, {{std::nullopt, {playable}}}};
        checkCancelled(cancel);
        result.segments = segments::loadCatalogSegments(m_store.databasePath(), *result.detail, key.address());
        return result;
    });
    emit changed();
}

void LibraryModel::finished(Kind kind, quint64 generation, const std::optional<Result> &result, const QString &error)
{
    if (m_closed || generation != (detailRequest(kind) ? m_showGeneration : m_generation)) return;
    if (!error.isEmpty() && kind != Kind::Rebuild) emit errorOccurred(error);
    if (kind == Kind::Library || kind == Kind::Rebuild) {
        m_catalogLoading = false;
        if (result) {
            if (result->catalog) m_shows = result->catalog->rows;
            if (result->movieCatalog) m_movies = result->movieCatalog->rows;
            const auto &catalog = m_browseMovies ? result->movieCatalog : result->catalog;
            if (catalog) m_cached = catalog->cached;
            m_imageSummary = result->stats;
        }
        // Background completion must not change a show request's loading/error
        // state or its episode context.
        if (m_showActive) {
            if (kind == Kind::Rebuild) {
                m_rebuildGeneration.reset();
                if (!error.isEmpty()) emit cacheRebuildFailed(error);
                else if (result) m_rebuildProgress = 1;
            }
            emit changed(); return;
        }
    }
    m_busy = false;
    if (kind == Kind::Rebuild) {
        m_rebuildGeneration.reset();
        if (!error.isEmpty() || !result) {
            m_state = m_previousState; m_message = m_previousMessage; m_cached = m_previousCached; m_pending = m_previousPending;
            if (!error.isEmpty()) emit cacheRebuildFailed(error);
            emit changed(); return;
        }
        m_rebuildProgress = 1;
    }
    if (kind == Kind::Stats) {
        if (result) m_imageSummary = result->stats;
        emit changed(); return;
    }
    if (kind == Kind::Images) {
        m_state = m_previousState; m_message = m_previousMessage;
        if (result) m_imageSummary = result->stats;
        else if (!error.isEmpty()) m_imageSummary = error;
        emit changed(); return;
    }
    m_state = error.isEmpty() ? "ready" : "error"; m_message = error;
    if (result) {
        if (!detailRequest(kind)) m_imageSummary = result->stats;
        if (kind == Kind::Library || kind == Kind::Rebuild) {
            const auto &catalog = m_browseMovies ? result->movieCatalog : result->catalog;
            if (catalog) m_message = catalog->message;
        } else if (kind == Kind::Show) {
            m_show = result->display; m_detail = result->detail; m_segments = result->segments; ++m_detailRevision;
            if(!m_segments.deferredSeasons.isEmpty())emit diagnosticMessage("info","Timestamp import deferred for a season reserved by detection. Refresh after detection finishes to retry.");
            selectSeason(m_detail->seasons.isEmpty() ? 1 : int(m_detail->seasons.first().number.value_or(0)));
        } else if (kind == Kind::Movie) {
            m_movie = result->display;
            m_movieDetail = result->detail;
            m_segments = result->segments;
            ++m_detailRevision;
        }
    }
    emit changed();
}

static QString timestamp(double value)
{
    // Source imports may contain reversed markers, but valid timestamps remain
    // raw seconds in segmentValues. Presentation rounds to the nearest ms.
    if (!std::isfinite(value) || value < 0 || value > double(std::numeric_limits<qint64>::max() / 1000)) return "—";
    const auto milliseconds = qint64(value * 1000 + .5);
    const auto seconds = milliseconds / 1000, fraction = milliseconds % 1000;
    auto text = QString("%1:%2").arg(seconds / 60, 2, 10, QChar('0')).arg(seconds % 60, 2, 10, QChar('0'));
    if (fraction) {
        auto decimal = QString("%1").arg(fraction, 3, 10, QChar('0'));
        while (decimal.endsWith('0')) decimal.chop(1);
        text += '.' + decimal;
    }
    return text;
}

void LibraryModel::selectSeason(int number)
{
    if (m_closed) return;
    m_season = number; m_episodes = episodesForSeason(number);
    emit changed();
}

QVariantList LibraryModel::episodesForSeason(int number) const
{
    QVariantList rows;
    if (m_closed) return rows;
    if (m_detail) for (const auto &season : m_detail->seasons) if (season.number.value_or(-1) == number) {
        for (const auto &episode : season.episodes) rows.append(playableRow(*m_detail, episode));
        break;
    }
    return rows;
}

QVariantList LibraryModel::movieItems() const
{
    if (!m_movieDetail || m_closed) return {};
    return {playableRow(*m_movieDetail, m_movieDetail->seasons.first().episodes.first())};
}

QVariantMap LibraryModel::playableRow(const ShowDetail &detail, const Episode &episode) const
{
    QVariantMap values;
    auto segments = m_segments.values.value(episode.identity.itemId);
    if (!m_segments.values.contains(episode.identity.itemId))
        for (const auto &item : episode.importedSegments) segments[segmentName(item.type)] = {item.startSeconds, item.endSeconds};
    QVariantMap row{{"id", episode.identity.itemId}, {"providerId", providerName(episode.identity.provider)},
        {"showId", detail.show.identity.itemId}, {"title", episode.title.value_or(QString())},
        {"itemUrl", m_key ? itemWebUrl(m_key->saved.remoteUrl, episode.identity) : QString()},
        {"number", episode.episodeNumber ? QVariant(*episode.episodeNumber) : QVariant()},
        {"season", episode.seasonNumber ? QVariant(*episode.seasonNumber) : QVariant()},
        {"status", m_segments.edited.contains(episode.identity.itemId) ? QString("Edited")
            : segments.isEmpty() || episode.importedSegments.isEmpty() ? QString() : library::providerLabel(episode.identity.provider)},
        {"recap", "—"}, {"intro", "—"}, {"credits", "—"}, {"preview", "—"}};
    if (detail.show.isMovie()) row["movie"] = true;
    for (auto it = segments.cbegin(); it != segments.cend(); ++it) {
        values[it.key()] = QVariantList{it.value().first, it.value().second};
        row[it.key()] = timestamp(it.value().first) + " – " + timestamp(it.value().second);
    }
    if (m_segments.statusRevisions.contains(episode.identity.itemId))
        row["statusRevision"] = m_segments.statusRevisions.value(episode.identity.itemId);
    row["segmentValues"] = values;
    return row;
}

void LibraryModel::closeShow()
{
    if (m_closed || !m_showActive) return;
    m_showCancel->store(true); ++m_showGeneration; m_showActive = false;
    m_busy = m_catalogLoading && activeRows().isEmpty(); m_state = m_busy ? "loading" : "ready"; m_message.clear();
    m_detail.reset(); m_movieDetail.reset(); m_show.clear(); m_movie.clear(); m_episodes.clear(); emit changed();
}

const ShowDetail *LibraryModel::activeDetail() const
{
    return m_movieDetail ? &*m_movieDetail : m_detail ? &*m_detail : nullptr;
}

std::optional<EpisodeContext> LibraryModel::episodeContext(const QString &id) const
{
    const auto *detail = activeDetail();
    if (m_closed || !detail) return {};
    for (const auto &season : detail->seasons) for (const auto &episode : season.episodes)
        if (episode.identity.itemId == id) return EpisodeContext{*detail, episode, m_detailRevision};
    return {};
}

void LibraryModel::updateSegments(quint64 revision, const QString &id, const segments::SegmentValues &values, bool edited)
{
    if (m_closed || !activeDetail() || revision != m_detailRevision) return;
    m_segments.values[id] = values;
    if (edited) m_segments.edited.insert(id);
    else m_segments.edited.remove(id);
    if (m_movieDetail) emit changed();
    else selectSeason(m_season);
}

void LibraryModel::close()
{
    if (m_closed) return;
    m_closed = true;
    m_metadata->close();
    m_cancel->store(true); m_showCancel->store(true); m_pool.waitForDone();
    disconnect(&m_settings, nullptr, this, nullptr);
    disconnect(m_settings.mediaSettings(), nullptr, this, nullptr);
}

} // namespace introvert::ui

#include "library_model.h"
#include <QRunnable>
#include <algorithm>
#include <cmath>
#include <limits>

namespace introvert::ui {
using namespace providers;

bool CatalogSelection::operator==(const CatalogSelection &other) const
{
    return listing == other.listing && saved.provider == other.saved.provider && saved.remoteUrl == other.saved.remoteUrl
        && saved.localRoot == other.saved.localRoot && saved.fetchImages == other.saved.fetchImages
        && saved.timeoutSeconds == other.saved.timeoutSeconds;
}

LibraryModel::LibraryModel(SettingsModel &settings, SettingsStore store, QString artworkRoot,
                           library::ProviderMap overrides, MetadataClient metadataClient, QObject *parent)
    : QObject(parent), m_settings(settings), m_store(std::move(store)),
      m_cache(std::move(artworkRoot), m_store.databasePath()), m_catalog(m_store, m_cache, std::move(overrides)),
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
        m_shows.clear(); m_episodes.clear(); m_show.clear(); m_detail.reset(); m_segments = {};
    }
    m_key = key;
    emit changed();
}

void LibraryModel::setBrowsing(bool active)
{
    if (m_closed) return;
    m_browsing = active;
    if (active && m_pending) { m_pending = false; load(); }
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
    const auto cancel = kind == Kind::Show ? m_showCancel : m_cancel;
    const auto summary = m_imageSummary;
    m_pool.start(QRunnable::create([this, kind, generation, work = std::move(work), cancel, summary] {
        std::optional<Result> result;
        QString error;
        try {
            checkCancelled(cancel);
            result = work(cancel, generation);
            result->stats = kind == Kind::Show ? summary : m_cache.stats();
        } catch (const Cancelled &) { result.reset(); }
        catch (const ProviderError &failure) { result.reset(); error = QString::fromUtf8(failure.what()); }
        catch (const DatabaseError &failure) { result.reset(); error = QString::fromUtf8(failure.what()); }
        catch (...) { result.reset(); error = "Cannot load media. Check the saved configuration and app.db access, then retry."; }
        QMetaObject::invokeMethod(this, [this, kind, generation, result = std::move(result), error] {
            finished(kind, generation, result, error);
        }, Qt::QueuedConnection);
    }), kind == Kind::Show ? 1 : 0);
}

void LibraryModel::publish(quint64 generation, const library::CatalogResult &catalog)
{
    QMetaObject::invokeMethod(this, [this, generation, catalog] {
        if (m_closed || generation != m_generation) return;
        m_shows = catalog.rows; m_cached = catalog.cached;
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
        m_cancel->store(true); ++m_generation; m_key.reset(); m_detail.reset(); m_show.clear();
        m_showCancel->store(true); ++m_showGeneration; m_showActive = false; m_catalogLoading = false;
        m_shows.clear(); m_episodes.clear(); m_busy = false; m_state = "error";
        m_message = "Cannot load saved media configuration. Check application database access and retry.";
        emit changed(); return;
    }
    if (!force && !clearLocal && m_key == previous && m_catalogLoading) return;
    m_showCancel->store(true); ++m_showGeneration; m_showActive = false;
    const auto generation = begin();
    m_catalogLoading = m_key.has_value();
    m_cached = false; m_detail.reset(); m_show.clear(); m_episodes.clear();
    if (m_key != previous) m_shows.clear();
    if (!m_key) {
        m_shows.clear(); m_busy = false; m_state = "ready";
        m_message = m_settings.mediaProvider().isEmpty()
            ? "Configure a media provider in Settings → Media to load your library." : "Select a media provider in Media settings.";
        submit(Kind::Stats, generation, [](const Cancellation &, quint64) { return Result{}; });
    } else {
        const auto key = *m_key;
        const auto oldRows = m_key == previous ? m_shows : QVariantList();
        submit(Kind::Library, generation, [this, key, force, clearLocal, oldRows](const Cancellation &cancel, quint64 gen) {
            Result result;
            const auto config = m_catalog.config(key.saved, cancel);
            result.catalog = m_catalog.browse(config, key.listing, force, clearLocal, oldRows, cancel,
                [this, gen](const library::CatalogResult &value) { publish(gen, value); });
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
        result.catalog = m_catalog.rebuild(m_catalog.config(key.saved, cancel), key.listing, cancel,
            [this, gen](double value) { progress(gen, value); });
        return result;
    });
    emit changed();
}

void LibraryModel::reloadLocalMetadata()
{
    if(m_closed || !m_key || m_key->saved.provider!="local")return;
    const auto key=*m_key;const auto generation=m_generation;const auto detailRevision=m_detailRevision;
    const auto showId=m_show.value("id").toString();const auto cancel=m_cancel;const auto before=m_segments;
    m_pool.start(QRunnable::create([this,key,generation,detailRevision,showId,cancel,before] {
        try {
            const auto config=m_catalog.config(key.saved,cancel);
            const auto catalog=m_catalog.browse(config,key.listing,false,false,{},cancel,[](const library::CatalogResult &){});
            std::optional<ShowDetail> detail;
            QVariantMap show;
            if(!showId.isEmpty()) {detail=m_catalog.detail(config,key.listing,showId,cancel);show=m_catalog.displayShow(config,detail->show,cancel);}
            const auto overlay=detail?segments::loadCatalogSegments(m_store.databasePath(),*detail):segments::CatalogSegments();
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

void LibraryModel::openShow(const QString &id)
{
    if (m_closed || !m_key) return;
    const auto key = *m_key;
    m_showCancel->store(true); m_showCancel = std::make_shared<std::atomic_bool>(false);
    const auto generation = ++m_showGeneration;
    m_showActive = true; m_busy = true; m_state = "loading"; m_message.clear();
    m_detail.reset(); m_episodes.clear(); m_show.clear();
    for (const auto &row : m_shows) if (row.toMap().value("id").toString() == id) { m_show = row.toMap(); break; }
    submit(Kind::Show, generation, [this, key, id](const Cancellation &cancel, quint64) {
        Result result;
        const auto config = m_catalog.config(key.saved, cancel);
        result.detail = m_catalog.detail(config, key.listing, id, cancel);
        result.display = m_catalog.displayShow(config, result.detail->show, cancel);
        QVariantList seasons;
        for (const auto &season : result.detail->seasons) seasons.append(season.number ? QVariant(*season.number) : QVariant());
        result.display["seasonNumbers"] = seasons;
        checkCancelled(cancel);
        result.segments = segments::loadCatalogSegments(m_store.databasePath(), *result.detail);
        return result;
    });
    emit changed();
}

void LibraryModel::finished(Kind kind, quint64 generation, const std::optional<Result> &result, const QString &error)
{
    if (m_closed || generation != (kind == Kind::Show ? m_showGeneration : m_generation)) return;
    if (kind == Kind::Library || kind == Kind::Rebuild) {
        m_catalogLoading = false;
        if (result) {
            m_shows = result->catalog.rows; m_cached = result->catalog.cached; m_imageSummary = result->stats;
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
        if (kind != Kind::Show) m_imageSummary = result->stats;
        if (kind == Kind::Library || kind == Kind::Rebuild) {
            m_shows = result->catalog.rows; m_message = result->catalog.message; m_cached = result->catalog.cached;
        } else if (kind == Kind::Show) {
            m_show = result->display; m_detail = result->detail; m_segments = result->segments; ++m_detailRevision;
            selectSeason(m_detail->seasons.isEmpty() ? 1 : int(m_detail->seasons.first().number.value_or(0)));
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
        for (const auto &episode : season.episodes) {
            QVariantMap values;
            auto segments = m_segments.values.value(episode.identity.itemId);
            if (!m_segments.values.contains(episode.identity.itemId))
                for (const auto &item : episode.importedSegments) segments[segmentName(item.type)] = {item.startSeconds, item.endSeconds};
            QVariantMap row{{"id", episode.identity.itemId}, {"providerId", providerName(episode.identity.provider)},
                {"showId", m_detail->show.identity.itemId}, {"title", episode.title.value_or(QString())},
                {"itemUrl", m_key ? itemWebUrl(m_key->saved.remoteUrl, episode.identity) : QString()},
                {"number", episode.episodeNumber ? QVariant(*episode.episodeNumber) : QVariant()},
                {"season", episode.seasonNumber ? QVariant(*episode.seasonNumber) : QVariant()},
                {"status", m_segments.edited.contains(episode.identity.itemId) ? QString("Edited")
                    : segments.isEmpty() || episode.importedSegments.isEmpty() ? QString() : library::providerLabel(episode.identity.provider)},
                {"recap", "—"}, {"intro", "—"}, {"credits", "—"}, {"preview", "—"}};
            for (auto it = segments.cbegin(); it != segments.cend(); ++it) {
                values[it.key()] = QVariantList{it.value().first, it.value().second};
                row[it.key()] = timestamp(it.value().first) + " – " + timestamp(it.value().second);
            }
            if(m_segments.statusRevisions.contains(episode.identity.itemId))row["statusRevision"]=m_segments.statusRevisions.value(episode.identity.itemId);
            row["segmentValues"] = values; rows.append(row);
        }
        break;
    }
    return rows;
}

void LibraryModel::closeShow()
{
    if (m_closed || !m_showActive) return;
    m_showCancel->store(true); ++m_showGeneration; m_showActive = false;
    m_busy = m_catalogLoading && m_shows.isEmpty(); m_state = m_busy ? "loading" : "ready"; m_message.clear();
    m_detail.reset(); m_show.clear(); m_episodes.clear(); emit changed();
}

std::optional<EpisodeContext> LibraryModel::episodeContext(const QString &id) const
{
    if (m_closed || !m_detail) return {};
    for (const auto &season : m_detail->seasons) for (const auto &episode : season.episodes)
        if (episode.identity.itemId == id) return EpisodeContext{*m_detail, episode, m_detailRevision};
    return {};
}

void LibraryModel::updateSegments(quint64 revision, const QString &id, const segments::SegmentValues &values, bool edited)
{
    if (m_closed || !m_detail || revision != m_detailRevision) return;
    m_segments.values[id] = values;
    if (edited) m_segments.edited.insert(id);
    else m_segments.edited.remove(id);
    selectSeason(m_season);
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

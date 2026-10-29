#include "segment_management_model.h"
#include <QRunnable>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

namespace introvert::ui {
using namespace providers;
using namespace segments;

SegmentManagementModel::SegmentManagementModel(LibraryModel &library, SegmentStore store,
                                               library::ProviderMap overrides, QObject *parent)
    : QObject(parent), m_library(library), m_store(std::move(store)),
      m_catalog(library.m_store, library.m_cache, std::move(overrides)), m_key(library.selection())
{
    m_pool.setMaxThreadCount(1);
    for (const auto &show : library.shows()) m_catalogIds.append(show.toMap()["id"].toString());
    connect(&library, &LibraryModel::changed, this, &SegmentManagementModel::libraryChanged);
}

SegmentManagementModel::~SegmentManagementModel() { close(); }
bool SegmentManagementModel::editingAllowed() const
{
    return m_library.m_detail && !m_library.m_editLocks.blocked(m_library.m_detail->show,m_library.season());
}

QVariantList SegmentManagementModel::seasons() const
{
    QVariantList result;
    if (m_detail) for (const auto &season : m_detail->seasons) result.append(season.number ? QVariant(*season.number) : QVariant());
    return result;
}

QVariantList SegmentManagementModel::episodes() const
{
    QVariantList result;
    if (m_detail) for (const auto &season : m_detail->seasons) for (const auto &episode : season.episodes)
        if (episode.episodeNumber) result.append(QVariantMap{{"number", *episode.episodeNumber},
            {"season", episode.seasonNumber ? QVariant(*episode.seasonNumber) : QVariant()}, {"title", episode.title.value_or(QString())}});
    return result;
}

void SegmentManagementModel::libraryChanged()
{
    if (m_closed) return;
    if (m_library.selection() != m_key) {
        m_cancel->store(true); ++m_generation; m_key = m_library.selection(); m_detail.reset();
        m_busy = false; m_message.clear(); m_episode.clear(); emit deleted();
    }
    QStringList ids;
    for (const auto &show : shows()) ids.append(show.toMap()["id"].toString());
    if (ids != m_catalogIds) { m_catalogIds = ids; emit deleted(); }
    emit changed();
}

void SegmentManagementModel::start(Kind kind, Work work)
{
    m_cancel->store(true); m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_cancel;
    const auto generation = ++m_generation;
    m_busy = true; m_message.clear();
    m_pool.start(QRunnable::create([this, kind, work = std::move(work), cancel, generation] {
        std::optional<Result> result;
        QString error;
        try { checkCancelled(cancel); result = work(cancel); }
        catch (const Cancelled &) {}
        catch (const SegmentError &failure) { error = QString::fromUtf8(failure.what()); }
        catch (const ProviderError &failure) { error = QString::fromUtf8(failure.what()); }
        catch (...) { error = "Cannot access the segment database. No changes could be confirmed."; }
        QMetaObject::invokeMethod(this, [this, kind, generation, result = std::move(result), error] {
            finished(kind, generation, result, error);
        }, Qt::QueuedConnection);
    }));
    emit changed();
}

void SegmentManagementModel::browseShow(const QString &id)
{
    if (m_closed) return;
    m_detail.reset(); m_episode.clear();
    if (id.isEmpty() || !m_key) {
        m_cancel->store(true); ++m_generation; m_busy = false; emit changed(); return;
    }
    const auto key = *m_key;
    start(Kind::Browse, [this, key, id](const Cancellation &cancel) {
        Result result; result.detail = m_catalog.detail(m_catalog.config(key.saved, cancel), key.listing, id, cancel); return result;
    });
}

void SegmentManagementModel::saveInline(const QString &id, const QString &kind, const QString &start, const QString &end)
{
    inlineChange(id, kind, qMakePair(start, end));
}

void SegmentManagementModel::deleteInline(const QString &id, const QString &kind) { inlineChange(id, kind, {}); }

void SegmentManagementModel::inlineChange(const QString &id, const QString &kind, const std::optional<QPair<QString, QString>> &times)
{
    if (m_closed || m_busy) return;
    const auto context = m_library.episodeContext(id);
    m_episode = id; m_message.clear();
    const auto type = kind.toLower();
    std::optional<QPair<double, double>> value;
    try {
        if (!context) throw SegmentError("This episode is no longer open");
        if(m_library.m_editLocks.blocked(context->detail.show,context->episode.seasonNumber.value_or(-1)))throw SegmentError("Season timestamps cannot be edited during detection");
        if (type != "recap" && type != "intro" && type != "credits" && type != "preview") throw SegmentError("Unsupported segment type");
        if (times) value = qMakePair(parseTime(times->first), parseTime(times->second));
    } catch (const SegmentError &error) { m_message = QString::fromUtf8(error.what()); emit changed(); return; }
    start(Kind::Inline, [this, context = *context, id, type, value](const Cancellation &cancel) {
        checkCancelled(cancel);
        const auto edit=m_library.m_editLocks.edit({{context.detail.show,context.episode.seasonNumber.value_or(-1)}});
        if(!edit)throw SegmentError("Season timestamps cannot be edited during detection");
        m_store.save(context.detail.show, context.episode, {{type, value}}, 0, "manual");
        Result result; result.episode = id; result.revision = context.revision;
        result.values = m_store.load(context.detail.show, context.episode);
        result.edited = m_store.isEdited(context.detail.show, context.episode); return result;
    });
}

namespace {
// QML JavaScript integral numbers arrive as doubles; reject booleans, strings,
// fractions and non-finite values before conversion, preserving source input rules.
std::optional<qint64> number(const QVariant &value)
{
    const auto type = value.metaType().id();
    if (type == QMetaType::Int || type == QMetaType::LongLong) {
        const auto n = value.toLongLong();
        return n >= 0 ? std::optional<qint64>(n) : std::nullopt;
    }
    if (type == QMetaType::UInt || type == QMetaType::ULongLong) {
        const auto n = value.toULongLong();
        return n <= quint64(std::numeric_limits<qint64>::max()) ? std::optional<qint64>(qint64(n)) : std::nullopt;
    }
    if (type != QMetaType::Double) return {};
    const auto n = value.toDouble();
    if (!std::isfinite(n) || n < 0 || n >= double(std::numeric_limits<qint64>::max()) || std::floor(n) != n) return {};
    return qint64(n);
}
}

bool SegmentManagementModel::canDelete(const QVariantList &targets) const
{
    if(!m_key || targets.empty())return false;
    try {
        const auto provider=providerId(m_key->saved.provider);
        for(const auto &value:targets) {
            const auto row=value.toMap();const auto show=row.value("showId").toString();const auto selected=row.value("seasonNumber");
            std::optional<qint64> season;
            if(selected.isValid() && !selected.isNull()) {season=number(selected);if(!season)return false;}
            if(m_library.m_editLocks.blocked(provider,show,season))return false;
            if(m_detail && m_detail->show.identity.itemId==show)for(const auto &group:m_detail->seasons)
                if(group.number && (!season || season==group.number) && m_library.m_editLocks.blocked(m_detail->show,*group.number))return false;
        }
        return true;
    } catch (...) {return false;}
}

void SegmentManagementModel::deleteBulk(const QVariantList &targets)
{
    if (m_closed || m_busy || !m_key) return;
    m_episode.clear();
    struct Target { QString show; std::optional<qint64> season; std::optional<QSet<qint64>> episodes; };
    QList<Target> plan;
    QSet<QString> known;
    for (const auto &show : shows()) known.insert(show.toMap()["id"].toString());
    try {
        if (targets.isEmpty()) throw SegmentError("Select a TV show, season, or episode");
        for (const auto &value : targets) {
            if (value.metaType().id() != QMetaType::QVariantMap) throw SegmentError("Invalid segment deletion selection");
            const auto row = value.toMap();
            const auto id = row.value("showId");
            const auto seasonValue = row.value("seasonNumber");
            Target target{id.toString(), {}, {}};
            if (id.metaType().id() != QMetaType::QString || !known.contains(target.show)) throw SegmentError("Invalid segment deletion selection");
            if (seasonValue.isValid() && !seasonValue.isNull()) {
                target.season = number(seasonValue);
                if (!target.season) throw SegmentError("Invalid segment deletion selection");
            }
            const auto episodes = row.value("episodeNumbers");
            if (episodes.isValid() && !episodes.isNull()) {
                if (!target.season || episodes.metaType().id() != QMetaType::QVariantList || episodes.toList().isEmpty())
                    throw SegmentError("Invalid episode selection");
                target.episodes = QSet<qint64>();
                for (const auto &episode : episodes.toList()) {
                    const auto n = number(episode);
                    if (!n) throw SegmentError("Invalid episode selection");
                    target.episodes->insert(*n);
                }
            }
            plan.append(target);
        }
        if(!canDelete(targets))throw SegmentError("Season timestamps cannot be deleted during detection");
    } catch (const SegmentError &error) { m_message = QString::fromUtf8(error.what()); emit changed(); return; }
    const auto key = *m_key;
    start(Kind::Bulk, [this, key, plan](const Cancellation &cancel) {
        const auto config = m_catalog.config(key.saved, cancel);
        QMap<QString, ShowDetail> details;
        QList<EpisodeTarget> episodes;
        std::set<std::tuple<QString, qint64, qint64>> seen;
        Result result;
        for (const auto &target : plan) {
            checkCancelled(cancel);
            if (!details.contains(target.show)) details.insert(target.show, m_catalog.detail(config, key.listing, target.show, cancel));
            const auto &detail = details[target.show];
            for (const auto &season : detail.seasons) {
                if (target.season && target.season != season.number) continue;
                for (const auto &episode : season.episodes) {
                    if (!episode.seasonNumber || !episode.episodeNumber
                        || (target.episodes && !target.episodes->contains(*episode.episodeNumber))) continue;
                    if (seen.emplace(target.show, *episode.seasonNumber, *episode.episodeNumber).second) {
                        episodes.append({detail.show, episode}); result.affected.append({detail.show.identity.itemId, episode.identity.itemId});
                    }
                }
            }
        }
        if (episodes.isEmpty()) throw SegmentError("No episodes match the selection");
        checkCancelled(cancel);
        QList<QPair<ShowSummary,qint64>> seasons;
        for(const auto &episode:episodes)seasons.append({episode.first,*episode.second.seasonNumber});
        const auto edit=m_library.m_editLocks.edit(seasons);
        if(!edit)throw SegmentError("Season timestamps cannot be deleted during detection");
        result.count = m_store.deleteMany(episodes);
        return result;
    });
}

void SegmentManagementModel::finished(Kind kind, quint64 generation, const std::optional<Result> &result, const QString &error)
{
    if (m_closed || generation != m_generation) return;
    m_busy = false; m_message = error;
    if (result) {
        if (kind == Kind::Browse) m_detail = result->detail;
        else if (kind == Kind::Inline) m_library.updateSegments(result->revision, result->episode, result->values, result->edited);
        else {
            m_message = QString("Segment timestamps deleted from %1 ").arg(result->count)
                + (result->count == 1 ? "episode." : "episodes.");
            m_detail.reset(); emit deleted();
            if (m_library.m_detail) {
                const auto show = m_library.m_detail->show.identity.itemId;
                bool updated = false;
                for (const auto &affected : result->affected) if (affected.first == show) {
                    m_library.m_segments.values[affected.second].clear(); m_library.m_segments.edited.remove(affected.second); updated = true;
                }
                if (updated) m_library.selectSeason(m_library.season());
            }
        }
    }
    emit changed();
}

void SegmentManagementModel::close()
{
    if (m_closed) return;
    m_closed = true; m_cancel->store(true); m_pool.waitForDone(); disconnect(&m_library, nullptr, this, nullptr);
}

} // namespace introvert::ui

#include "media_engine.h"
#include "algorithm_features.h"
#include "algorithm_video.h"
#include "media_probe.h"
#include "resource_profile.h"
#include "pcm_cache.h"
#include "continuous_work.h"
#include "media_work_gate.h"
#include "job_metrics.h"
#include "core/settings_store.h"
#include <QThreadPool>
#include <QRunnable>
#include <QStorageInfo>
#include <future>
#include "editor/range_stream.h"
#include "providers/http_transport.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSet>
#include "candidates.h"
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace introvert::detection {
namespace {
QString binary(QString name)
{
#ifdef Q_OS_WIN
    name += ".exe";
#endif
    const auto bundled = QDir(QCoreApplication::applicationDirPath()).filePath("bin/" + name);
    if (QFileInfo(bundled).isFile()) return bundled;
    const auto found = QStandardPaths::findExecutable(name); if (found.isEmpty()) throw providers::ProviderError("The required " + name + " runtime is unavailable."); return found;
}
QString digest(const QString &value) { return QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex()); }
class MappedResources final : public ProcessRunner {
public:
    explicit MappedResources(ResourceManager &manager,int threads) : decoderThreads(threads),resources(manager) {}
    const int decoderThreads;
    QMap<QString, QString> paths;
    ResourceManager &resources;
    ProcessResult run(const ProcessRequest &request, const Cancellation &cancel) override {
        auto mapped = request; for (auto &argument : mapped.arguments) { const auto value = paths.constFind(argument); if (value != paths.cend()) argument = *value; }
        if (request.category==ProcessCategory::Decoder) {
            mapped.arguments = mapped.arguments.mid(0,1) + QStringList{"-threads",QString::number(decoderThreads),"-filter_threads",QString::number(decoderThreads)} + mapped.arguments.mid(1);
            const auto last=mapped.arguments.takeLast(); mapped.arguments << "-threads" << QString::number(decoderThreads) << last;
        }
        return resources.run(mapped, cancel);
    }
    QList<ProcessDiagnostic> diagnostics() const override { return resources.diagnostics(); }
};
}
MediaDetectionEngine::MediaDetectionEngine(QString path, library::MediaCache &cache, QString ffmpeg, QString ffprobe, QVariantMap parameters, QString algorithmRoot, QString resources)
    : m_path(std::move(path)), m_ffmpeg(std::move(ffmpeg)), m_ffprobe(std::move(ffprobe)), m_algorithmRoot(std::move(algorithmRoot)), m_resourceProfile(std::move(resources)), m_parameters(std::move(parameters)), m_cache(cache) {}
MediaDetectionOutput MediaDetectionEngine::run(const providers::ProviderConfig &config, const QList<providers::Episode> &episodes, const QStringList &kinds,
    const providers::Cancellation &cancel, const DetectionProgress &progress, const QString &jobId)
try
{
    const auto requestedEffort=m_resourceProfile.isEmpty()?SettingsStore(m_path).detectionResources():m_resourceProfile;
    // One application-wide budget, including future show/library queues. Jobs
    // cannot each multiply their own matching pools or decoder/cache allowances.
    static QSemaphore execution(1);
    while (!execution.tryAcquire(1,20)) providers::checkCancelled(cancel);
    struct ExecutionScope {
        QSemaphore &gate; QThread *thread=QThread::currentThread();
        QThread::Priority priority=thread->priority(); QThread::QualityOfService qos=thread->serviceLevel();
        ~ExecutionScope() { thread->setServiceLevel(qos); if (priority!=QThread::InheritPriority) thread->setPriority(priority); gate.release(); }
    } executionScope{execution};
    providers::checkCancelled(cancel);
    auto profile=resourceProfile(requestedEffort);
    WorkAdmission admission(profile.workingBytes,profile.memoryReserveBytes);
    QThread::currentThread()->setPriority(profile.priority); QThread::currentThread()->setServiceLevel(profile.serviceLevel);
    JobMetrics utilization;
    QElapsedTimer elapsed; elapsed.start();
    const auto ffmpeg = m_ffmpeg.isEmpty() ? binary("ffmpeg") : m_ffmpeg, ffprobe = m_ffprobe.isEmpty() ? binary("ffprobe") : m_ffprobe;
    QTemporaryDir temporary(QDir::tempPath() + "/introvert-detection-XXXXXX"); if (!temporary.isValid()) throw providers::ProviderError("Cannot create temporary detection storage.");
    QList<SegmentType> types;
    for (const auto &kind : kinds) {
        if (kind != "intro" && kind != "credits") throw std::invalid_argument("Unsupported audio search type.");
        const auto type = kind == "intro" ? SegmentType::Intro : SegmentType::Credits;
        if (!types.contains(type)) types.append(type);
    }
    if (types.empty()) throw std::invalid_argument("Audio analysis requires requested search types.");
    if (jobId.isEmpty()) throw std::invalid_argument("Detection job ID is required.");
    const auto package = AlgorithmPackages(m_path, m_algorithmRoot).selected();
    const auto pinnedAlgorithm = std::make_shared<Algorithm>(package); auto &algorithm = *pinnedAlgorithm;
    auto description = algorithm.description(m_parameters,profile.name);
    const bool effortSupported=description.value("effort").toMap().value("version")=="effort-v1"
        && description.value("effort").toMap().value("name")==profile.name;
    const auto algorithmEffort=effortSupported?profile.name:QString();
    auto visualDescription=description.value("visual").toMap();
    const double visualRadius=std::min(effortSupported?15.:5.,visualDescription.value("edge_radius_seconds",5.).toDouble());
    visualDescription["edge_radius_seconds"]=visualRadius;description["visual"]=visualDescription;
    const auto parameters = description.value("parameters").toMap();
    if (!description.value("parallel_matches").toBool()) profile.matchingWorkers=1;
    if (!description.value("parallel_extraction").toBool()) profile.mediaWorkers=1;
    if (!description.value("parallel_visual").toBool()) profile.visualWorkers=1;
    const auto trackLimit = std::min(qint64(256 * 1024 * 1024), parameters.value("maximum_track_bytes").toLongLong());
    const auto matchLimit = std::min(4096, parameters.value("maximum_matches").toInt());
    if (trackLimit <= 0 || matchLimit <= 0) throw DetectionError("algorithm_invalid_description", "Invalid algorithm work limits.");
    ResourceLimits processLimits; processLimits.decoders=profile.mediaWorkers; processLimits.application=processLimits.job=std::max(6,profile.mediaWorkers+2);
    processLimits.probes=profile.mediaWorkers;
    ResourceManager processResources(processLimits);
    QVariantList failures, cacheOperations, coverage, matches, pairs, matchingWarnings, searchWindows, mediaRecoveries;
    QVariantMap visual;
    const bool visualEnabled=types.contains(SegmentType::Credits) && description.value("visual").toMap().value("available").toBool();
    int visualFrames=0;
    std::vector<std::shared_ptr<const FeatureTrack>> tracks;
    std::map<std::pair<QString, SegmentType>, TimeRange> allowedWindows; QMap<QString, MediaInfo> preparedMedia;
    qint64 retainedBytes = 0; QSet<QString> episodeIds;
    const auto cancelled = [cancel] { if (cancel && cancel->load()) throw detection::Cancelled(); };
    for (const auto &episode : episodes) {
        providers::checkCancelled(cancel);
        if (episode.identity.itemId.isEmpty() || episodeIds.contains(episode.identity.itemId)) throw std::invalid_argument("Episode IDs must be non-empty and unique.");
        episodeIds.insert(episode.identity.itemId);
    }
    std::atomic<qint64> streamResumes{0}, retriedEpisodes{0};
    MediaWorkGate remoteWork(profile.mediaWorkers);
    struct Source {
        QString path; MediaIdentity identity; std::unique_ptr<editor::RangeStream> stream;
        std::unique_ptr<MediaWorkGate::Lease> lease;
        std::atomic<qint64> *resumes;
        explicit Source(std::atomic<qint64> *total) : resumes(total) {}
        Source(Source &&)=default;
        ~Source() { if(stream) {stream->close(); resumes->fetch_add(stream->resumedRequests());} }
    };
    const auto prepare = [&](const providers::Episode &episode) {
        Source source(&streamResumes); auto &path = source.path; auto &identity = source.identity; auto &stream = source.stream;
        if (config.provider == providers::ProviderId::Local) {
            if (!episode.mediaPath) throw DetectionError("media_unavailable", "Episode media is unavailable.");
            path = QFileInfo(normalizedMediaPath(*episode.mediaPath)).canonicalFilePath(); QFile file(path);
            if (path.isEmpty() || !QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly)) throw DetectionError("media_unavailable", "Episode media is unavailable.");
            file.read(1); if (file.error() != QFileDevice::NoError) throw DetectionError("media_unavailable", "Episode media is unavailable.");
            identity = readMediaIdentity(path);
        } else {
            source.lease=remoteWork.acquire(cancelled);
            stream = editor::RangeStream::open(config, episode.identity.itemId, cancel,8);
            const auto head = providers::detail::exchange(stream->url(), {}, std::min(config.timeoutSeconds, 10.), 0, cancel, {}, false, true, true);
            bool valid = false; const auto size = head.headers.value("content-length", "0").toLongLong(&valid);
            if (!valid || size < 0) throw DetectionError("media_size_invalid", "Media server returned an invalid media size.");
            const auto version = !head.headers.value("etag").isEmpty() ? QString::fromLatin1(head.headers.value("etag"))
                : !head.headers.value("last-modified").isEmpty() ? QString::fromLatin1(head.headers.value("last-modified")) : jobId;
            path = "introvert-media://" + digest(m_cache.scope(config, "detection") + '\n' + episode.identity.itemId);
            identity = {path, size, digest(version).left(15).toLongLong(nullptr, 16)};
        }
        return source;
    };
    const auto freeDisk=QStorageInfo(temporary.path()).bytesAvailable();
    const qint64 pcmLimit=freeDisk>0?std::min(profile.pcmDiskBytes,freeDisk/4):profile.pcmDiskBytes;
    PcmCache pcmCache(pcmLimit);
    const auto retainPcm=[&](const QString &episode,SegmentType type,DecodedAudio &&audio) {
        const auto rate=uint32_t(audio.window().specification.sampleRate);
        pcmCache.retain({episode,type,rate},std::move(audio),cancelled);
    };
    struct EpisodeResult {
        std::vector<std::shared_ptr<const FeatureTrack>> tracks;
        QMap<QString,MediaInfo> media;
        std::map<std::pair<QString,SegmentType>,TimeRange> windows;
        QVariantList failures,cache,coverage,search;
        qint64 bytes;
        QVariantList retriedFailures;
    };
    std::atomic<qint64> descriptorReserved{0}; std::atomic_bool descriptorLimitExceeded{false}; QVariantMap extractionExecution;
    {
    const auto extractEpisode=[&](size_t n) {
        const auto &episode=episodes[qsizetype(n)]; providers::checkCancelled(cancel);
        auto reservation=admission.acquire(256LL*1024*1024,cancelled);
        MappedResources resources(processResources,profile.decoderThreads);
        std::vector<std::shared_ptr<const FeatureTrack>> tracks;
        QMap<QString,MediaInfo> preparedMedia; std::map<std::pair<QString,SegmentType>,TimeRange> allowedWindows;
        QVariantList failures,cacheOperations,coverage,searchWindows; qint64 retainedBytes=0;
        QString stage = "preparation";
        try {
            auto source = prepare(episode); const auto &path = source.path; const auto &identity = source.identity; const auto &stream = source.stream;
            resources.paths.clear(); resources.paths.insert(path, stream ? stream->url().toString() : path);
            stage = "inspection";
            const auto media = inspectMedia(resources, path, episode.identity.itemId, jobId, cancelled, ffprobe, [identity](const QString &) { return identity; });
            preparedMedia.insert(episode.identity.itemId, media);
            stage = "extraction";
            for (const auto &window : planWindows(media.duration, types)) {
                searchWindows.append(QVariantMap{{"episode", episode.identity.itemId}, {"segment_type", segmentName(window.type)},
                    {"start", window.range.start}, {"end", window.range.end}});
                allowedWindows.emplace(std::make_pair(episode.identity.itemId, window.type), window.range);
                try {
                    const qint64 rate=description.value("sample_rate").toLongLong(),hop=description.value("hop_samples").toLongLong(),context=description.value("context_samples").toLongLong();
                    const qint64 samples=qint64(std::floor(window.range.duration()*rate+1e-8));
                    const qint64 expected=samples<context?0:(samples-context)/hop+1;
                    const qint64 reservation=expected*(sizeof(double)+description.value("columns").toLongLong()*sizeof(float));
                    const auto previous=descriptorReserved.fetch_add(reservation);
                    if (reservation>trackLimit || previous>trackLimit-reservation) {descriptorReserved.fetch_sub(reservation); descriptorLimitExceeded.store(true); throw DetectionError("raw_audio_limit_exceeded","Audio descriptor memory budget exceeded.");}
                    struct Reservation {std::atomic<qint64> &used; qint64 bytes; bool keep=false; ~Reservation(){if(!keep)used.fetch_sub(bytes);}} held{descriptorReserved,reservation};
                    auto budget = parameters; budget["maximum_track_bytes"] = std::max(qint64(1),reservation);
                    if (budget.value("maximum_track_bytes").toLongLong() <= 0) throw DetectionError("raw_audio_limit_exceeded", "Audio descriptor memory budget exceeded.");
                    auto track = extractAlgorithmFeatures(algorithm, description, resources, media, episode.identity.itemId, jobId, temporary.path(), ffmpeg, nullptr, cancelled,
                        [&](const QVariantMap &event) {if(event.value("status")=="coverage")coverage.append(event);else cacheOperations.append(event.value("status"));},
                        budget, window, description.value("refinement_sample_rate").toInt(),
                        [&](DecodedAudio &&audio){retainPcm(episode.identity.itemId,window.type,std::move(audio));});
                    const auto bytes=qint64(track->rows())*qint64(sizeof(double)+track->columns*sizeof(float));
                    descriptorReserved.fetch_sub(reservation-bytes); held.keep=true;
                    retainedBytes+=bytes; tracks.push_back(std::move(track));
                } catch (const providers::Cancelled &) { throw; }
                catch (const detection::Cancelled &) { throw; }
                catch (const DetectionError &error) { failures.append(QVariantMap{{"episode", episode.identity.itemId}, {"stage", stage},
                    {"segment_type", segmentName(window.type)}, {"codes", QStringList{error.code}}}); }
                catch (...) { failures.append(QVariantMap{{"episode", episode.identity.itemId}, {"stage", stage},
                    {"segment_type", segmentName(window.type)}, {"codes", QStringList{"extraction_failed"}}}); }

            }
        } catch (const providers::Cancelled &) { throw; }
        catch (const detection::Cancelled &) { throw providers::Cancelled(); }
        catch (const DetectionError &error) { failures.append(QVariantMap{{"episode", episode.identity.itemId}, {"stage", stage}, {"codes", QStringList{error.code}}}); }
        catch (...) { failures.append(QVariantMap{{"episode", episode.identity.itemId}, {"stage", stage}, {"codes", QStringList{stage + "_failed"}}}); }
        return EpisodeResult{std::move(tracks),std::move(preparedMedia),std::move(allowedWindows),failures,cacheOperations,coverage,searchWindows,retainedBytes};
    };
    ContinuousWork<EpisodeResult> tasks(episodes.size(),profile.mediaWorkers,profile.priority,profile.serviceLevel,[&](size_t n) {
        auto result=extractEpisode(n);
        if(config.provider!=providers::ProviderId::Local && profile.mediaWorkers>1 && !result.failures.empty()) {
            const auto retriedFailures=result.failures;
            remoteWork.reduceToOne();++retriedEpisodes;
            descriptorReserved.fetch_sub(result.bytes);result=EpisodeResult{};
            result=extractEpisode(n);
            result.retriedFailures=retriedFailures;
        }
        return result;
    });
    for (qsizetype n=0;n<episodes.size();++n) {
        if(progress)progress("extraction",int(n),episodes.size());
        auto result=tasks.take(size_t(n),cancelled);
        for (auto &track:result.tracks)tracks.push_back(std::move(track));
        preparedMedia.insert(result.media); allowedWindows.insert(result.windows.begin(),result.windows.end());
        failures.append(result.failures); cacheOperations.append(result.cache); coverage.append(result.coverage); searchWindows.append(result.search); retainedBytes+=result.bytes;
        if(!result.retriedFailures.empty())mediaRecoveries.append(QVariantMap{{"episode",episodes[n].identity.itemId},{"failures",result.retriedFailures},{"remote_media_limit",1}});
    }
    extractionExecution=tasks.diagnostics();
    }
    if(descriptorLimitExceeded.load())throw DetectionError("raw_audio_limit_exceeded","Audio descriptor memory budget exceeded.");
    providers::checkCancelled(cancel);
    std::sort(tracks.begin(), tracks.end(), [](const auto &a, const auto &b) { return a->type != b->type ? a->type < b->type : unicodeLess(a->episodeId, b->episodeId); });
    qint64 pairCount = 0; QSet<QString> usableEpisodes;
    for (const auto type : types) {
        const qint64 count = std::count_if(tracks.begin(), tracks.end(), [type](const auto &track) { return track->type == type; });
        pairCount += count * (count - 1) / 2;
    }
    for (const auto &track : tracks) usableEpisodes.insert(track->episodeId);
    if (pairCount > 10000) throw providers::ProviderError("The season exceeds the initial audio analysis pair budget.");
    const qint64 extractionMs = elapsed.elapsed(); const auto extractionUtilization=utilization.diagnostics();
    struct PairResult { QVariantMap pair, statistics; QVariantList matches; bool success; };
    std::vector<std::pair<size_t,size_t>> work;
    for (size_t i=0;i<tracks.size();++i) for (size_t j=i+1;j<tracks.size();++j) if (tracks[i]->type==tracks[j]->type) work.emplace_back(i,j);
    int pairIndex=0,successfulPairs=0;
    if (progress) progress("generation",0,int(pairCount));
    QVariantMap matchingExecution;
    {
    ContinuousWork<PairResult> tasks(work.size(),profile.matchingWorkers,profile.priority,profile.serviceLevel,[&](size_t n) {
        auto reservation=admission.acquire(32LL*1024*1024,cancelled);
        const auto i=work[n].first,j=work[n].second;
                providers::checkCancelled(cancel);
                QVariantMap pair{{"episode_a", tracks[i]->episodeId}, {"episode_b", tracks[j]->episodeId}, {"segment_type", segmentName(tracks[i]->type)}};
                QVariantMap statistics; QVariantList foundMatches; bool success=false;
                try {
                    auto budget = parameters; budget["maximum_matches"] = matchLimit;
                    if (budget.value("maximum_matches").toInt() <= 0) throw DetectionError("raw_audio_limit_exceeded", "Audio result budget exceeded.");
                    QList<TimeRange> pcmWindows{tracks[i]->window,tracks[j]->window};
                    const auto read = [&](uint32_t index, uint32_t rate, qint64 first, uint32_t count) {
                        if(index>1 || first<0)throw DetectionError("algorithm_invalid_audio_read","Invalid scoped PCM source.");
                        const auto &track=*(index==0?tracks[i]:tracks[j]);
                        const auto &part=pcmWindows[index];
                        if(first>qint64(std::floor(part.duration()*rate+1e-8))-count)
                            throw DetectionError("algorithm_invalid_audio_read","PCM read exceeds its hard search window.");
                        if(const auto full=pcmCache.retained({track.episodeId,track.type,rate})) {
                            const auto offset=first+qint64(std::llround((part.start-track.window.start)*rate));
                            const auto &window=full->window();const auto available=std::min(qint64(count),std::max(qint64(0),window.sampleCount-offset));
                            return available?window.readSamples(offset,qsizetype(available)):std::vector<qint16>{};
                        }
                        const auto decode=[&](const TimeRange &part) {
                            const auto item=std::find_if(episodes.begin(),episodes.end(),[&](const auto &episode){return episode.identity.itemId==track.episodeId;});
                            if(item==episodes.end() || !preparedMedia.contains(track.episodeId))throw DetectionError("algorithm_invalid_audio_read","Unknown audio source.");
                            const auto attempt=[&] {
                                auto source=prepare(*item);const auto media=preparedMedia.value(track.episodeId);
                                if(source.identity.normalizedPath!=media.identity.normalizedPath || source.identity.fileSize!=media.identity.fileSize || source.identity.modifiedTimeNs!=media.identity.modifiedTimeNs)
                                    throw DetectionError("media_changed","Episode media changed during detection.");
                                MappedResources workerResources(processResources,profile.decoderThreads);
                                workerResources.paths.insert(source.path,source.stream?source.stream->url().toString():source.path);
                                AudioDecodeSpecification spec;spec.sampleRate=int(rate);
                                auto audio=decodeAudio(workerResources,media,track.episodeId,track.type,part,jobId,temporary.path(),cancelled,spec,{},300.,{},ffmpeg);
                                if(std::abs(audio.window().durationDifference)>.05)throw DetectionError("algorithm_incomplete_audio_read","Refinement audio does not cover its requested span.");
                                return audio;
                            };
                            try{return attempt();}
                            catch(const detection::Cancelled &){throw;}
                            catch(const DetectionError &e) {
                                if(config.provider==providers::ProviderId::Local || e.code=="media_changed")throw;
                                remoteWork.reduceToOne();return attempt();
                            }
                        };
                        // All reads for this source/pair use one continuous
                        // component-declared PCM region. Decoder priming cannot
                        // change between its alignment anchor and edge reads.
                        const qint64 bytes=qint64(std::ceil(part.duration()*rate))*2+qint64(std::ceil(.05*rate))*2+2;
                        const bool full=part.start==track.window.start && part.end==track.window.end;
                        const auto audio=pcmCache.get({track.episodeId,track.type,rate,full?-1:qint64(std::llround((part.start-track.window.start)*rate)),full?-1:qint64(std::llround((part.end-track.window.start)*rate))},bytes,[&]{return decode(part);},cancelled);
                        const auto available=std::min(qint64(count),std::max(qint64(0),audio->window().sampleCount-first));
                        return available?audio->window().readSamples(first,qsizetype(available)):std::vector<qint16>{};
                    };
                    QVariantMap request{{"a",featureMessage(*tracks[i])},{"b",featureMessage(*tracks[j])},{"parameters",budget},{"refine",true},{"effort",algorithmEffort}};
                    QVariantMap result;
                    if(description.value("scoped_refinement")=="pair-window-v1") {
                        request["refine"]=false;result=algorithm.invoke("match",request,cancelled,{},&statistics);
                        statistics=result.value("statistics").toMap();
                        if(result.value("matches").metaType().id()!=QMetaType::QVariantList || result.value("matches").toList().size()>matchLimit)
                            throw DetectionError("algorithm_invalid_result","Invalid coarse audio matches.");
                        if(!result.value("matches").toList().empty()) {
                            const auto windows=result.value("refinement_windows").toList();if(windows.size()!=2)throw DetectionError("algorithm_invalid_result","Missing refinement windows.");
                            for(int n=0;n<2;++n) {
                                const auto w=windows[n].toMap();const TimeRange part(w.value("start").toDouble(),w.value("end").toDouble());
                                if(!pcmWindows[n].contains(part))throw DetectionError("algorithm_invalid_result","Refinement exceeds its hard search window.");
                                pcmWindows[n]=part;
                            }
                            request["matches"]=result.value("matches");
                            const auto refined=algorithm.invoke("refine_matches",request,cancelled,read);
                            result["matches"]=refined.value("matches");pair["pcm_windows"]=windows;
                        }
                    } else result=algorithm.invoke("match",request,cancelled,read,&statistics);
                    if (result.value("matches").metaType().id() != QMetaType::QVariantList || result.value("statistics").metaType().id() != QMetaType::QVariantMap
                        || result.value("insufficient_evidence").metaType().id() != QMetaType::Bool)
                        throw DetectionError("algorithm_invalid_result", "The algorithm match result is incomplete.");
                    const auto found = result.value("matches").toList(); statistics = result.value("statistics").toMap();
                    if (found.size() > budget.value("maximum_matches").toInt()) throw DetectionError("algorithm_invalid_result", "Algorithm exceeded its result budget.");
                    for (const auto &value : found) {
                        const auto m = value.toMap();
                        if (m.value("episode_a") != tracks[i]->episodeId || m.value("episode_b") != tracks[j]->episodeId || m.value("segment_type") != segmentName(tracks[i]->type)
                            || !tracks[i]->window.contains({m.value("start_a").toDouble(), m.value("end_a").toDouble()})
                            || !tracks[j]->window.contains({m.value("start_b").toDouble(), m.value("end_b").toDouble()}))
                            throw DetectionError("algorithm_invalid_result", "Algorithm match exceeds its scoped request.");
                    }
                    foundMatches=found; pair["status"] = found.empty() ? "no_match" : "matched"; pair["matches"] = int(found.size()); success=true;
                    if (result.value("insufficient_evidence").toBool()) pair["status"] = "insufficient_evidence";
                } catch (const detection::Cancelled &) { throw providers::Cancelled(); }
                catch (const providers::Cancelled &) { throw; }
                catch (const DetectionError &error) { pair["status"] = "failed"; pair["code"] = error.code; }
                catch (...) { pair["status"] = "failed"; pair["code"] = "audio_matching_failed"; }
                return PairResult{pair,statistics,foundMatches,success};
    });
    for (size_t n=0;n<work.size();++n) {
            auto result=tasks.take(n,cancelled); auto pair=result.pair; const auto statistics=result.statistics;
            providers::checkCancelled(cancel); if (progress) progress("generation",pairIndex++,int(pairCount));
            if (result.success && result.matches.size()>matchLimit-matches.size()) {
                result.success=false; pair["status"]="failed"; pair["code"]="raw_audio_limit_exceeded";
            }
            if (result.success) { matches.append(result.matches); ++successfulPairs; }
            pair["matching"] = statistics;
            if (pair.value("status") == "failed") failures.append(pair);
            if (statistics.value("suppressed_seed_lookups").toLongLong()) {
                pair["retrieval_limited"] = true;
                if (pair.value("status") == "no_match") pair["status"] = "inconclusive";
                matchingWarnings.append(QVariantMap{{"episode_a",pair.value("episode_a")},{"episode_b",pair.value("episode_b")},
                    {"segment_type",pair.value("segment_type")}, {"code", "common_fingerprints_suppressed"}, {"suppressed_seed_lookups", statistics.value("suppressed_seed_lookups").toLongLong()}});
            }
            pairs.append(pair);
    }
    matchingExecution=tasks.diagnostics();
    }
    providers::checkCancelled(cancel);
    const auto matchingMs=elapsed.elapsed()-extractionMs; const auto matchingUtilization=utilization.diagnostics();
    MediaDetectionOutput output;
    for (const auto &episode : episodes) output.episodes[episode.identity.itemId] = {};
    QVariantList selected, decisions, audioSelected, audioDecisions; qint64 visualMs=0; QVariantMap visualExecution;
    try {
        QSet<QString> assigned;
        auto selection = algorithm.invoke("select", {{"matches", matches}, {"parameters", parameters}}, cancelled);
        audioDecisions=selection.value("decisions").toList(); audioSelected=selection.value("selected").toList();
        QMap<QString,QVariantMap> audioByKind;
        for (const auto &entry:audioSelected) {
            const auto a=entry.toMap(); const auto id=a.value("episode").toString(),kind=a.value("segment_type").toString();
            const auto type=kind=="intro"?SegmentType::Intro:SegmentType::Credits;
            const auto window=allowedWindows.find({id,type}); const auto key=id+'\n'+kind;
            if ((kind!="intro" && kind!="credits") || window==allowedWindows.end() || audioByKind.contains(key)
                || !window->second.contains({a.value("start").toDouble(),a.value("end").toDouble()}))
                throw DetectionError("algorithm_invalid_result","Invalid audio selection scope.");
            audioByKind.insert(key,a);
        }
        QElapsedTimer visualTimer; visualTimer.start();
        QVariantList creditSelections;
        for(const auto &entry:audioSelected)if(entry.toMap().value("segment_type")=="credits")creditSelections.append(entry);
        struct VisualResult { QString episode; QVariantMap result; QVariantList cache; };
        std::atomic_int visualActive{0},visualPeak{0};
        {
        ContinuousWork<VisualResult> tasks(size_t(creditSelections.size()),profile.visualWorkers,profile.priority,profile.serviceLevel,[&](size_t n) {
            const auto selectedAudio=creditSelections[qsizetype(n)].toMap();const auto id=selectedAudio.value("episode").toString();
            QVariantList operations;
            if(!visualEnabled)return VisualResult{id,{{"status","unavailable"},{"reason",description.value("visual").toMap().value("reason","optional_backend_unavailable")}},operations};
            providers::checkCancelled(cancel);
            auto reservation=admission.acquire(512LL*1024*1024,cancelled);
            const int active=++visualActive;auto peak=visualPeak.load();while(active>peak && !visualPeak.compare_exchange_weak(peak,active)){}
            struct ActiveVisual {std::atomic_int &active;~ActiveVisual(){--active;}} activeVisual{visualActive};
            const auto item=std::find_if(episodes.begin(),episodes.end(),[&](const auto &episode){return episode.identity.itemId==id;});
            const auto media=preparedMedia.value(id);
            if(!media.selectedVideo)return VisualResult{id,{{"status","unavailable"},{"reason","no_video_stream"}},operations};
            QVariantMap result;
            try {
                const auto attempt=[&] {
                auto source=prepare(*item);
                if(source.identity.normalizedPath!=media.identity.normalizedPath || source.identity.fileSize!=media.identity.fileSize || source.identity.modifiedTimeNs!=media.identity.modifiedTimeNs)
                    throw DetectionError("media_changed","Episode media changed during detection.");
                MappedResources workerResources(processResources,profile.decoderThreads);
                workerResources.paths.insert(source.path,source.stream?source.stream->url().toString():source.path);
                return extractAlgorithmVideo(algorithm,description,workerResources,media,id,jobId,ffmpeg,nullptr,
                    {SegmentType::Credits,allowedWindows.at({id,SegmentType::Credits})},selectedAudio.value("start").toDouble(),cancelled,
                    [&](const QVariantMap &event){operations.append(event.value("status"));});
                };
                try{result=attempt();}
                catch(const detection::Cancelled &){throw;}
                catch(const DetectionError &error) {
                    if(config.provider==providers::ProviderId::Local || (error.code!="visual_decode_incomplete" && error.code!="visual_decode_timeout"))throw;
                    remoteWork.reduceToOne();result=attempt();result["transport_retry"]=true;result["first_failure"]=error.code;
                }
            } catch(const detection::Cancelled &){throw;}
            catch(const providers::Cancelled &){throw;}
            catch(const DetectionError &error){result={{"status","failed"},{"code",error.code}};}
            catch(...){result={{"status","failed"},{"code","visual_analysis_failed"}};}
            return VisualResult{id,result,operations};
        });
        for(qsizetype n=0;n<creditSelections.size();++n) {
            auto result=tasks.take(size_t(n),cancelled);visual[result.episode]=result.result;cacheOperations.append(result.cache);
            visualFrames+=result.result.value("coarse_frames").toInt()+result.result.value("dense_frames").toInt();
            if(progress)progress("refinement",int(n+1),int(creditSelections.size()));
        }
        visualExecution=tasks.diagnostics();
        visualExecution["peak_admitted"]=visualPeak.load();
        }
        visualMs=visualTimer.elapsed();
        if (visualEnabled) try {
            const auto fused=algorithm.invoke("fuse",{{"audio_selected",audioSelected},{"matches",matches},{"visual",visual},{"effort",algorithmEffort}},cancelled);
            const auto proposed=fused.value("selected").toList(); QSet<QString> seen;
            if (proposed.size()!=audioByKind.size()) throw DetectionError("visual_invalid_fusion","Visual refinement must preserve audio selections.");
            for (const auto &entry:proposed) {
                const auto p=entry.toMap(); const auto kind=p.value("segment_type").toString(),key=p.value("episode").toString()+'\n'+kind;
                const auto original=audioByKind.constFind(key); const auto start=p.value("start").toDouble(),end=p.value("end").toDouble();
                if (original==audioByKind.cend() || seen.contains(key) || !std::isfinite(start) || !std::isfinite(end)
                    || start<0 || start>=end || end!=original->value("end").toDouble()
                    || !allowedWindows.at({p.value("episode").toString(),kind=="intro"?SegmentType::Intro:SegmentType::Credits}).contains({start,end})
                    || std::abs(start-original->value("start").toDouble())>(kind=="credits"?visualRadius+.000001:0.))
                    throw DetectionError("visual_invalid_fusion","Visual refinement exceeded its audio edge scope.");
                seen.insert(key);
            }
            selection=fused;
        } catch (const detection::Cancelled &) { throw; }
        catch (...) { for (auto it=visual.begin();it!=visual.end();++it) {auto v=it->toMap(); v["fusion_status"]="audio_fallback"; *it=v;} }

        decisions = selection.value("decisions").toList();
        if (selection.value("selected").metaType().id() != QMetaType::QVariantList) throw DetectionError("algorithm_invalid_result", "The algorithm selection result is incomplete.");
        for (const auto &value : selection.value("selected").toList()) {
            const auto segment = value.toMap(); const auto episode = segment.value("episode").toString(), kind = segment.value("segment_type").toString();
            const auto type = kind == "intro" ? SegmentType::Intro : SegmentType::Credits;
            const auto allowed = allowedWindows.find({episode, type}); const TimeRange range(segment.value("start").toDouble(), segment.value("end").toDouble());
            if ((kind != "intro" && kind != "credits") || allowed == allowedWindows.end() || !allowed->second.contains(range) || assigned.contains(episode + '\n' + kind))
                throw DetectionError("algorithm_invalid_result", "Selected segment exceeds its scoped request.");
            assigned.insert(episode + '\n' + kind); output.episodes[episode][kind] = {range.start, range.end}; selected.append(segment);
        }
    } catch (const detection::Cancelled &) { throw providers::Cancelled(); }
    output.failed = pairCount == 0 || successfulPairs == 0;
    if (description.value("finalization").toString() == "segments-v1") {
        QMap<QString, double> durations; for (auto it = preparedMedia.cbegin(); it != preparedMedia.cend(); ++it) durations[it.key()] = it->duration;
        output.finalize = [pinnedAlgorithm, durations, allowedWindows](const QString &episode, const QString &kind, const QPair<double, double> &measured,
            const segments::SegmentValues &saved, const providers::Cancellation &cancel) {
            QVariantMap segments; for (auto it = saved.cbegin(); it != saved.cend(); ++it) segments[it.key()] = QVariantList{it->first, it->second};
            try {
                const auto result = pinnedAlgorithm->invoke("finalize", {{"episode", episode}, {"segment_type", kind},
                    {"start", measured.first}, {"end", measured.second}, {"duration", durations.value(episode)}, {"segments", segments}},
                    [cancel] { providers::checkCancelled(cancel); });
                bool startOk = false, endOk = false;
                const double start = result.value("start").toDouble(&startOk), end = result.value("end").toDouble(&endOk);
                const auto allowed = allowedWindows.find({episode, kind == "intro" ? SegmentType::Intro : SegmentType::Credits});
                if (!startOk || !endOk || !std::isfinite(start) || !std::isfinite(end) || start < 0 || start >= end
                    || (kind != "intro" && kind != "credits") || allowed == allowedWindows.end() || !allowed->second.contains({start, end})
                    || result.value("diagnostics").metaType().id() != QMetaType::QVariantMap)
                    throw DetectionError("algorithm_invalid_result", "Finalized segment exceeds its scoped request.");
                return FinalizedSegment{{start, end}, result.value("diagnostics").toMap()};
            } catch (const detection::Cancelled &) { throw providers::Cancelled(); }
        };
    }
    output.diagnostics = {{"composition", algorithm.metadata().value("composition")}, {"detector_version", package.version}, {"mode", visualEnabled ? "audio_visual" : "audio_only"},
        {"selection", algorithm.metadata().value("selection")}, {"selected", selected}, {"selection_decisions", decisions},
        {"analysis_reuse",QVariantMap{{"policy","fresh-job-v1"},{"persistent_features",false},{"temporary_pcm","job-only"}}},
        {"audio_selected",audioSelected},{"audio_selection_decisions",audioDecisions},{"visual",visual},
        {"window_policy", "hard-audio-windows-v1"}, {"search_windows", searchWindows},
        {"algorithm_interface", algorithmInterfaceVersion}, {"algorithm_package_hash", package.manifestHash}, {"parameters", parameters}, {"coverage", coverage}, {"matches", matches}, {"pairs", pairs}, {"failures", failures}, {"cache", cacheOperations},
        {"matching_warnings", matchingWarnings},{"media_recoveries",mediaRecoveries}, {"outcome", output.failed ? "failed" : failures.empty() && matchingWarnings.empty() ? "complete" : "partial"},
        {"usable_episodes", int(usableEpisodes.size())}, {"descriptor_bytes", retainedBytes},
        {"extraction_execution",extractionExecution},{"matching_execution",matchingExecution},{"visual_execution",visualExecution},
        {"effort",QVariantMap{{"requested",profile.name},{"supported",effortSupported},{"algorithm",description.value("effort")},{"admission",admission.diagnostics()}}},{"resources",[&]{auto d=profile.diagnostics(); d["concurrent_decoders"]=processLimits.decoders; return d;}()},
        {"utilization",utilization.diagnostics()},{"utilization_stages",QVariantMap{{"after_extraction",extractionUtilization},{"after_matching",matchingUtilization}}},{"processes",processResources.statistics()},
        {"performance", QVariantMap{{"extraction_ms", extractionMs},{"matching_ms",matchingMs},{"visual_ms",visualMs}, {"matching_selection_ms", elapsed.elapsed()-extractionMs},
            {"stream_resumes",streamResumes.load()},{"retried_episodes",retriedEpisodes.load()},{"remote_media_limit",remoteWork.limit()},
            {"pcm_prefetched",pcmCache.statistics().value("pcm_prefetched")},{"pcm_decodes",pcmCache.statistics().value("pcm_decodes")}, {"pcm_cache_hits",pcmCache.statistics().value("pcm_cache_hits")}, {"peak_pcm_disk_bytes",pcmCache.statistics().value("peak_pcm_disk_bytes")}, {"peak_parallel_pcm_loads",pcmCache.statistics().value("peak_parallel_pcm_loads")}, {"pcm_disk_budget",pcmLimit},{"scoped_pcm",description.value("scoped_refinement")},{"visual_frames",visualFrames}}}};
    return output;
} catch(const detection::Cancelled &){throw providers::Cancelled();}
} // namespace introvert::detection

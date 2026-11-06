#include "algorithm_pipeline.h"
#include "continuous_work.h"
#include "candidates.h"
#include <QElapsedTimer>
#include <QSet>
#include <cmath>
#include <cstring>
#include <map>

namespace introvert::detection {
namespace {
struct Reply {
    QByteArray bytes;
    bool invalid=false;
    static int respond(void *context,const uint8_t *data,uint64_t size) {
        auto &reply=*static_cast<Reply *>(context);
        try {
            if(!data || !size || size>uint64_t(algorithmMessageLimit) || !reply.bytes.isEmpty()) {reply.invalid=true;return 0;}
            reply.bytes=QByteArray(reinterpret_cast<const char *>(data),qsizetype(size));return 1;
        } catch(...) {reply.invalid=true;return 0;}
    }
    QVariantMap value() const {
        if(invalid)throw DetectionError("algorithm_invalid_result","Invalid host service response.");
        const auto response=parseAlgorithmMessage(bytes);
        if(response.value("ok").metaType().id()!=QMetaType::Bool)throw DetectionError("algorithm_invalid_result","Invalid host service outcome.");
        if(!response.value("ok").toBool()) {
            if(response.value("code")=="cancelled")throw Cancelled();
            if(response.value("code")=="host_failure")throw std::runtime_error(response.value("message").toString().toStdString());
            throw DetectionError(response.value("code").toString(),response.value("message").toString());
        }
        if(response.value("value").metaType().id()!=QMetaType::QVariantMap)throw DetectionError("algorithm_invalid_result","Invalid host service payload.");
        return response.value("value").toMap();
    }
};
struct PipelineTrack {
    QString episodeId;
    SegmentType type;
    TimeRange window;
    QVariantMap message;
};
}
void PipelineServices::check() const { if(m_host.cancelled(m_host.context))throw Cancelled(); }
QVariantMap PipelineServices::request(const char *operation,const QVariantMap &input) const {
    const auto bytes=algorithmMessage(input);Reply reply;
    if(!m_host.request(m_host.context,operation,reinterpret_cast<const uint8_t *>(bytes.constData()),uint64_t(bytes.size()),&reply,Reply::respond))
        throw DetectionError("algorithm_service_failed","The detector host service did not complete.");
    return reply.value();
}
void PipelineServices::publish(const QVariantMap &value) const {
    const auto bytes=algorithmMessage(value);
    if(!m_host.publish(m_host.context,reinterpret_cast<const uint8_t *>(bytes.constData()),uint64_t(bytes.size())))throw DetectionError("algorithm_invalid_result","The detector result was rejected.");
}
int64_t PipelineServices::readInto(const QString &episode,SegmentType type,uint32_t rate,const TimeRange &window,const TimeRange &part,qint64 first,uint32_t count,int16_t *output) const {
    const auto id=episode.toUtf8();
    const IntrovertDetectorAudioReadV1 request{reinterpret_cast<const uint8_t *>(id.constData()),uint64_t(id.size()),uint32_t(type),rate,window.start,window.end,part.start,part.end,first,count};
    Reply error;
    const auto read=m_host.read_pcm(m_host.context,&request,output,&error,Reply::respond);
    if(read<0) {error.value();throw DetectionError("algorithm_invalid_audio_read","The host did not return scoped PCM.");}
    if(read>count)throw DetectionError("algorithm_invalid_audio_read","PCM response exceeds its request.");
    return read;
}
PipelineServices::Lease::Lease(const PipelineServices &services,const char *operation,const QVariantMap &input):m_services(services),m_id(services.request(operation,input).value("id").toLongLong()) {
    if(m_id<=0)throw DetectionError("algorithm_service_failed","Invalid host resource lease.");
}
PipelineServices::Lease::~Lease() {try{m_services.request("release",{{"id",m_id}});}catch(...) {}}
QVariantMap acceptedBoundarySelection(const QVariantMap &original,const QVariantMap &refined)
{
    if(!original.value("requires_visual_confirmation").toBool() || refined.value("confirmation_status")=="confirmed")return refined;
    auto fallback=original.value("evidence").toMap().value("review_fallback").toMap();
    const double start=fallback.value("start").toDouble(),end=fallback.value("end").toDouble();
    if(fallback.empty() || fallback.value("episode")!=original.value("episode") || fallback.value("segment_type")!=original.value("segment_type")
        || fallback.value("requires_visual_confirmation").toBool() || !std::isfinite(start) || !std::isfinite(end) || start<0 || start>=end)return {};
    fallback["season_review_status"]="unconfirmed_fallback";return fallback;
}
void runAlgorithmPipeline(Algorithm &algorithm,const QVariantMap &input,const PipelineServices &services)
{
    const auto m_parameters=input.value("parameters").toMap();
    const bool visualRefinementEnabled=input.value("visual_refinement").toBool();
    const auto limits=input.value("resources").toMap();
    struct Profile {QString name;int matchingWorkers,mediaWorkers,visualWorkers;QThread::Priority priority;QThread::QualityOfService serviceLevel;};
    Profile profile{limits.value("profile").toString(),limits.value("matching_workers").toInt(),limits.value("media_workers").toInt(),limits.value("visual_workers").toInt(),QThread::Priority(input.value("priority").toInt()),QThread::QualityOfService(input.value("qos").toInt())};
    QList<SegmentType> types;
    for(const auto &kind:input.value("kinds").toStringList())types.append(kind=="intro"?SegmentType::Intro:SegmentType::Credits);
    const auto report=[&](const char *stage,int current,int total) {services.publish({{"progress",QString::fromLatin1(stage)},{"current",current},{"total",total}});};
    QElapsedTimer timer;timer.start();
    // Effort controls host scheduling/storage only. Use the fullest refinement
    // at every resource level, including with older effort-v1 components.
    const QString analysisEffort=QStringLiteral("high");
    auto description = algorithm.description(m_parameters,analysisEffort);
    const bool effortSupported=description.value("effort").toMap().value("version")=="effort-v1"
        && description.value("effort").toMap().value("name")==analysisEffort;
    const auto algorithmEffort=effortSupported?analysisEffort:QString();
    const auto parameters = description.value("parameters").toMap();
    if (!description.value("parallel_matches").toBool()) profile.matchingWorkers=1;
    if (!description.value("parallel_extraction").toBool()) profile.mediaWorkers=1;
    if (!description.value("parallel_visual").toBool()) profile.visualWorkers=1;
    const auto trackLimit = std::min(qint64(256 * 1024 * 1024), parameters.value("maximum_track_bytes").toLongLong());
    const auto matchLimit = std::min(4096, parameters.value("maximum_matches").toInt());
    if (trackLimit <= 0 || matchLimit <= 0) throw DetectionError("algorithm_invalid_description", "Invalid algorithm work limits.");
    QVariantList failures, cacheOperations, coverage, matches, pairs, matchingWarnings, searchWindows, mediaRecoveries;
    QVariantMap visual;
    const auto boundaryDescription=description.value("boundary_refinement").toMap();
    const bool visualEnabled=visualRefinementEnabled && boundaryDescription.value("available").toBool();
    int visualFrames=0;
    std::vector<std::shared_ptr<const PipelineTrack>> tracks;
    std::map<std::pair<QString, SegmentType>, TimeRange> allowedWindows; QMap<QString, bool> videoAvailable;
    qint64 retainedBytes = 0;
    const auto cancelled = [&] { services.check(); };
    struct Episode { struct Identity { QString itemId; } identity; };
    QList<Episode> episodes;
    for (const auto &value : input.value("episodes").toList()) episodes.append(Episode{{value.toString()}});
    std::atomic<qint64> retriedEpisodes{0}, boundarySources{0};
    struct EpisodeResult {
        std::vector<std::shared_ptr<const PipelineTrack>> tracks;
        QMap<QString,bool> video;
        std::map<std::pair<QString,SegmentType>,TimeRange> windows;
        QVariantList failures,cache,coverage,search;
        qint64 bytes;
        QVariantList retriedFailures;
    };
    QVariantMap extractionExecution;
    {
    const auto extractEpisode=[&](size_t n) {
        services.check();
        const auto result=services.request("extract_episode",{{"episode",episodes[qsizetype(n)].identity.itemId}});
        EpisodeResult out;
        for(const auto &value:result.value("tracks").toList()) {
            const auto message=value.toMap();const auto track=featureFromMessage(message);
            // Retain the validated wire arrays once per job, replacing the raw
            // vectors rather than duplicating them or serializing every pair.
            out.tracks.push_back(std::make_shared<const PipelineTrack>(PipelineTrack{track->episodeId,track->type,track->window,message}));
        }
        out.failures=result.value("failures").toList();out.cache=result.value("cache").toList();out.coverage=result.value("coverage").toList();out.search=result.value("search").toList();
        out.bytes=result.value("bytes").toLongLong();out.video[episodes[qsizetype(n)].identity.itemId]=result.value("video").toBool();
        for(const auto &value:out.search) {
            const auto w=value.toMap();const auto type=w.value("segment_type")=="intro"?SegmentType::Intro:SegmentType::Credits;
            out.windows.emplace(std::make_pair(w.value("episode").toString(),type),TimeRange(w.value("start").toDouble(),w.value("end").toDouble()));
        }
        return out;
    };
    ContinuousWork<EpisodeResult> tasks(episodes.size(),profile.mediaWorkers,profile.priority,profile.serviceLevel,[&](size_t n) {
        auto result=extractEpisode(n);
        if(input.value("remote").toBool() && profile.mediaWorkers>1 && !result.failures.empty()) {
            const auto retriedFailures=result.failures;
            services.request("retry_episode",{{"bytes",result.bytes}});++retriedEpisodes;
            result=EpisodeResult{};
            result=extractEpisode(n);
            result.retriedFailures=retriedFailures;
        }
        return result;
    });
    for (qsizetype n=0;n<episodes.size();++n) {
        report("extraction",int(n),episodes.size());
        auto result=tasks.take(size_t(n),cancelled);
        for (auto &track:result.tracks)tracks.push_back(std::move(track));
        videoAvailable.insert(result.video); allowedWindows.insert(result.windows.begin(),result.windows.end());
        failures.append(result.failures); cacheOperations.append(result.cache); coverage.append(result.coverage); searchWindows.append(result.search); retainedBytes+=result.bytes;
        if(!result.retriedFailures.empty())mediaRecoveries.append(QVariantMap{{"episode",episodes[n].identity.itemId},{"failures",result.retriedFailures},{"remote_media_limit",1}});
    }
    extractionExecution=tasks.diagnostics();
    }
    if(services.request("metrics").value("descriptor_limit_exceeded").toBool())throw DetectionError("raw_audio_limit_exceeded","Audio descriptor memory budget exceeded.");
    services.check();
    std::sort(tracks.begin(), tracks.end(), [](const auto &a, const auto &b) { return a->type != b->type ? a->type < b->type : unicodeLess(a->episodeId, b->episodeId); });
    qint64 pairCount = 0; QSet<QString> usableEpisodes;
    for (const auto type : types) {
        const qint64 count = std::count_if(tracks.begin(), tracks.end(), [type](const auto &track) { return track->type == type; });
        pairCount += count * (count - 1) / 2;
    }
    for (const auto &track : tracks) usableEpisodes.insert(track->episodeId);
    if (pairCount > 10000) throw std::invalid_argument("The season exceeds the initial audio analysis pair budget.");
    const qint64 extractionMs = timer.elapsed(); const auto extractionUtilization=services.request("metrics").value("utilization").toMap();
    struct PairResult { QVariantMap pair, statistics; QVariantList matches; bool success; };
    std::vector<std::pair<size_t,size_t>> work;
    for (size_t i=0;i<tracks.size();++i) for (size_t j=i+1;j<tracks.size();++j) if (tracks[i]->type==tracks[j]->type) work.emplace_back(i,j);
    int pairIndex=0,successfulPairs=0;
    report("generation",0,int(pairCount));
    QVariantMap matchingExecution;
    {
    ContinuousWork<PairResult> tasks(work.size(),profile.matchingWorkers,profile.priority,profile.serviceLevel,[&](size_t n) {
        PipelineServices::Lease reservation(services,"admit",{{"bytes",32LL*1024*1024}});
        const auto i=work[n].first,j=work[n].second;
                services.check();
                QVariantMap pair{{"episode_a", tracks[i]->episodeId}, {"episode_b", tracks[j]->episodeId}, {"segment_type", segmentName(tracks[i]->type)}};
                QVariantMap statistics; QVariantList foundMatches; bool success=false;
                try {
                    auto budget = parameters; budget["maximum_matches"] = matchLimit;
                    if (budget.value("maximum_matches").toInt() <= 0) throw DetectionError("raw_audio_limit_exceeded", "Audio result budget exceeded.");
                    QList<TimeRange> pcmWindows{tracks[i]->window,tracks[j]->window};
                    const auto read = [&](uint32_t index, uint32_t rate, qint64 first, uint32_t count,int16_t *output) {
                        if(index>1 || first<0)throw DetectionError("algorithm_invalid_audio_read","Invalid scoped PCM source.");
                        const auto &track=*(index==0?tracks[i]:tracks[j]);
                        return services.readInto(track.episodeId,track.type,rate,track.window,pcmWindows[index],first,count,output);
                    };
                    QVariantMap request{{"a",tracks[i]->message},{"b",tracks[j]->message},{"parameters",budget},{"refine",true},{"effort",algorithmEffort}};
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
                            const auto refined=algorithm.invoke("refine_matches",request,cancelled,{},nullptr,read);
                            result["matches"]=refined.value("matches");pair["pcm_windows"]=windows;
                        }
                    } else result=algorithm.invoke("match",request,cancelled,{},&statistics,read);
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
                } catch (const detection::Cancelled &) { throw; }
                catch (const DetectionError &error) { pair["status"] = "failed"; pair["code"] = error.code; }
                catch (...) { pair["status"] = "failed"; pair["code"] = "audio_matching_failed"; }
                return PairResult{pair,statistics,foundMatches,success};
    });
    for (size_t n=0;n<work.size();++n) {
            auto result=tasks.take(n,cancelled); auto pair=result.pair; const auto statistics=result.statistics;
            services.check(); report("generation",pairIndex++,int(pairCount));
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
    services.check();
    const auto matchingMs=timer.elapsed()-extractionMs; const auto matchingUtilization=services.request("metrics").value("utilization").toMap();
    QVariantList selected, decisions, audioSelected, audioDecisions,confirmationCandidates; qint64 visualMs=0; QVariantMap visualExecution;
    try {
        QSet<QString> assigned;
        auto selection = algorithm.invoke("select", {{"matches", matches}, {"parameters", parameters},{"search_windows",searchWindows}}, cancelled);
        audioDecisions=selection.value("decisions").toList(); audioSelected=selection.value("selected").toList();
        confirmationCandidates=selection.value("confirmation_candidates").toList();
        auto visualInput=audioSelected;
        if(visualEnabled && boundaryDescription.value("confirmation_candidates").toBool())visualInput.append(confirmationCandidates);
        QMap<QString,QVariantMap> audioByKind;
        for (const auto &entry:visualInput) {
            const auto a=entry.toMap(); const auto id=a.value("episode").toString(),kind=a.value("segment_type").toString();
            const auto type=kind=="intro"?SegmentType::Intro:SegmentType::Credits;
            const auto window=allowedWindows.find({id,type}); const auto key=id+'\n'+kind;
            if ((kind!="intro" && kind!="credits") || window==allowedWindows.end() || audioByKind.contains(key)
                || !window->second.contains({a.value("start").toDouble(),a.value("end").toDouble()}))
                throw DetectionError("algorithm_invalid_result","Invalid audio selection scope.");
            audioByKind.insert(key,a);
        }
        QMap<QString,QVariantList> selectionsByEpisode;
        for(const auto &entry:visualInput)selectionsByEpisode[entry.toMap().value("episode").toString()].append(entry);
        const auto visualEpisodes=selectionsByEpisode.keys();
        if (visualEnabled) {
            QElapsedTimer visualTimer; visualTimer.start();
            struct VisualResult {QString episode;QVariantMap result;QVariantList selected,decisions;};
            QMap<QString,QVariantMap> refinedSelections;QVariantList boundaryDecisions;
            std::atomic_int visualActive{0},visualPeak{0};
            ContinuousWork<VisualResult> tasks(size_t(visualEpisodes.size()),profile.visualWorkers,profile.priority,profile.serviceLevel,[&](size_t n) {
                const auto id=visualEpisodes[qsizetype(n)];
                QVariantMap result{{"status","analyzed"},{"frames",0},{"decoded",false}};
                services.check();PipelineServices::Lease reservation(services,"admit",{{"bytes",256LL*1024*1024}});
                const int active=++visualActive;auto peak=visualPeak.load();while(active>peak && !visualPeak.compare_exchange_weak(peak,active)){}
                struct ActiveVisual {std::atomic_int &active;~ActiveVisual(){--active;}} activeVisual{visualActive};
                // One authenticated route/identity check for all this episode's
                // edges. Do not resolve a route for clipped edges with no work.
                std::unique_ptr<PipelineServices::Lease> source;
                QMap<QPair<double,double>,QVariantMap> scannedWindows;
                for(const auto &entry:selectionsByEpisode.value(id)) {
                    const auto segment=entry.toMap();const auto kind=segment.value("segment_type").toString();
                    const auto type=kind=="intro"?SegmentType::Intro:SegmentType::Credits;
                    const auto hard=allowedWindows.at({id,type});QVariantMap edges;
                    for(const auto &edge:kind=="intro"?QStringList{"start","end"}:QStringList{"start"}) {
                        QVariantMap scanned;const double audioTime=segment.value(edge).toDouble();
                        const double reference=segment.value("boundary_search").toMap().value(edge).toMap().value("reference",audioTime).toDouble();bool shared=false;
                        try {
                            if(!std::isfinite(reference) || reference<0 || std::abs(reference-audioTime)>boundaryDescription.value("maximum_search_offset_seconds",0.).toDouble()
                                || (reference!=audioTime && (kind!="intro" || edge!="start" || reference>audioTime)))
                                throw DetectionError("visual_invalid_scope","Invalid aligned boundary search.");
                            if(description.value("boundary_refinement").toMap().value("audio_boundary_protection").toBool()
                                && segment.value("boundary_protection").toMap().value(edge).toBool())
                                scanned={{"status","skipped"},{"reason","corroborated_audio_boundary"}};
                            else if(!videoAvailable.value(id))scanned={{"status","unavailable"},{"reason","no_video_stream"}};
                            else {
                                if(reference>hard.start && reference<hard.end && !source) {
                                    source=std::make_unique<PipelineServices::Lease>(services,"open_media",QVariantMap{{"episode",id}});
                                    ++boundarySources;
                                }
                                const auto key=qMakePair(std::max(hard.start,reference-3.),std::min(hard.end,reference+3.));
                                if(reference>hard.start && reference<hard.end && scannedWindows.contains(key)) {
                                    scanned=scannedWindows.value(key);shared=true;scanned["shared_scan"]=true;scanned["audio_time"]=reference;scanned["runtime_ms"]=0;
                                }
                                else {
                                    scanned=services.request("scan",{{"episode",id},{"source",source?source->id():0},{"segment_type",segmentName(type)},{"start",hard.start},{"end",hard.end},{"reference",reference},{"description",description}});
                                    if(scanned.value("decoded").toBool())scannedWindows[key]=scanned;
                                }
                            }
                        } catch(const detection::Cancelled &){throw;}
                        catch(const DetectionError &error){scanned={{"status","failed"},{"code",error.code}};}
                        catch(...){scanned={{"status","failed"},{"code","visual_analysis_failed"}};}
                        edges[edge]=scanned;
                        result["frames"]=result.value("frames").toInt()+(shared?0:scanned.value("frames").toInt());
                        result["decoded"]=result.value("decoded").toBool() || scanned.value("decoded").toBool();
                    }
                    result[kind]=edges;
                }
                auto proposed=selectionsByEpisode.value(id);QVariantList decisions;
                try {
                    const auto fused=algorithm.invoke("fuse_boundaries",{{"audio_selected",proposed},{"visual",QVariantMap{{id,result}}}},cancelled);
                    const auto refined=fused.value("selected").toList();QSet<QString> seen;
                    if(refined.size()!=proposed.size())throw DetectionError("visual_invalid_fusion","Visual refinement must preserve audio selections.");
                    for(const auto &entry:refined) {
                        const auto segment=entry.toMap();const auto kind=segment.value("segment_type").toString(),key=id+'\n'+kind;
                        const auto original=audioByKind.constFind(key);const double start=segment.value("start").toDouble(),end=segment.value("end").toDouble();
                        const double startReference=original==audioByKind.cend()?start:original->value("boundary_search").toMap().value("start").toMap().value("reference",original->value("start")).toDouble();
                        if(segment.value("episode")!=id || original==audioByKind.cend() || seen.contains(key) || !std::isfinite(start) || !std::isfinite(end)
                            || start<0 || start>=end || (kind=="credits" && end!=original->value("end").toDouble())
                            || !allowedWindows.at({id,kind=="intro"?SegmentType::Intro:SegmentType::Credits}).contains({start,end})
                            || (start!=original->value("start").toDouble() && std::abs(start-startReference)>3.000001)
                            || (startReference!=original->value("start").toDouble() && start>original->value("start").toDouble())
                            || std::abs(end-original->value("end").toDouble())>3.000001)
                            throw DetectionError("visual_invalid_fusion","Visual refinement exceeded its audio edge scope.");
                        seen.insert(key);
                    }
                    proposed=refined;decisions=fused.value("decisions").toList();
                } catch(const detection::Cancelled &){throw;}
                catch(...) {result["fusion_status"]="audio_fallback";}
                // Release the per-episode frame statistics before publishing a
                // worker result. A large season never creates one huge ABI call
                // or retains every episode's observations until the job ends.
                for(const auto &kind:QStringList{"intro","credits"}) {
                    auto edges=result.value(kind).toMap();
                    for(const auto &edge:QStringList{"start","end"}) {
                        auto scanned=edges.value(edge).toMap();if(scanned.empty())continue;
                        scanned.remove("observations");edges[edge]=scanned;
                    }
                    if(!edges.empty())result[kind]=edges;
                }
                return VisualResult{id,result,proposed,decisions};
            });
            for(qsizetype n=0;n<visualEpisodes.size();++n) {
                auto result=tasks.take(size_t(n),cancelled);visual[result.episode]=result.result;
                visualFrames+=result.result.value("frames").toInt();boundaryDecisions.append(result.decisions);
                for(const auto &entry:result.selected) {const auto segment=entry.toMap();refinedSelections[result.episode+'\n'+segment.value("segment_type").toString()]=segment;}
                report("refinement",int(n+1),int(visualEpisodes.size()));
            }
            QVariantList refined;
            for(const auto &entry:visualInput) {
                const auto segment=entry.toMap();const auto result=refinedSelections.value(segment.value("episode").toString()+'\n'+segment.value("segment_type").toString(),segment);
                const auto accepted=acceptedBoundarySelection(segment,result);
                if(!accepted.empty())refined.append(accepted);
            }
            selection["selected"]=refined;selection["decisions"]=boundaryDecisions;
            visualExecution=tasks.diagnostics();visualExecution["peak_admitted"]=visualPeak.load();visualMs=visualTimer.elapsed();
        } else for(const auto &id:visualEpisodes) {
            const QVariantMap state{{"status",visualRefinementEnabled?"unavailable":"disabled"},
                {"reason",visualRefinementEnabled?boundaryDescription.value("reason","boundary_refinement_unavailable"):QVariant("disabled_by_setting")}};
            auto v=state;
            for(const auto &entry:selectionsByEpisode.value(id))v[entry.toMap().value("segment_type").toString()]=state;
            visual[id]=v;
        }
        decisions = selection.value("decisions").toList();
        if (selection.value("selected").metaType().id() != QMetaType::QVariantList) throw DetectionError("algorithm_invalid_result", "The algorithm selection result is incomplete.");
        for (const auto &value : selection.value("selected").toList()) {
            const auto segment = value.toMap(); const auto episode = segment.value("episode").toString(), kind = segment.value("segment_type").toString();
            const auto type = kind == "intro" ? SegmentType::Intro : SegmentType::Credits;
            const auto allowed = allowedWindows.find({episode, type}); const TimeRange range(segment.value("start").toDouble(), segment.value("end").toDouble());
            if ((kind != "intro" && kind != "credits") || allowed == allowedWindows.end() || !allowed->second.contains(range) || assigned.contains(episode + '\n' + kind))
                throw DetectionError("algorithm_invalid_result", "Selected segment exceeds its scoped request.");
            assigned.insert(episode + '\n' + kind); selected.append(segment);
        }
    } catch (const detection::Cancelled &) { throw detection::Cancelled(); }
    const bool failed = pairCount == 0 || successfulPairs == 0;
    const auto hostMetrics=services.request("metrics");
    const QVariantMap diagnostics = {{"composition", algorithm.metadata().value("composition")}, {"detector_version", algorithm.metadata().value("version")}, {"mode", visualEnabled ? "audio_visual" : "audio_only"},
        {"visual_refinement",QVariantMap{{"enabled",visualRefinementEnabled},{"active",visualEnabled}}},
        {"selection", algorithm.metadata().value("selection")}, {"selected", selected}, {"selection_decisions", decisions},
        {"analysis_reuse",QVariantMap{{"policy","fresh-job-v1"},{"persistent_features",false},{"temporary_pcm","job-only"}}},
        {"audio_selected",audioSelected},{"audio_selection_decisions",audioDecisions},{"confirmation_candidates",confirmationCandidates},{"visual",visual},
        {"window_policy", "hard-audio-windows-v1"}, {"search_windows", searchWindows},
        {"algorithm_interface", algorithmInterfaceVersion}, {"algorithm_package_hash", input.value("manifest_hash")}, {"parameters", parameters}, {"coverage", coverage}, {"matches", matches}, {"pairs", pairs}, {"failures", failures}, {"cache", cacheOperations},
        {"matching_warnings", matchingWarnings},{"media_recoveries",mediaRecoveries},{"media_streams",hostMetrics.value("media_streams")}, {"outcome", failed ? "failed" : failures.empty() && matchingWarnings.empty() ? "complete" : "partial"},
        {"usable_episodes", int(usableEpisodes.size())}, {"descriptor_bytes", retainedBytes},
        {"extraction_execution",extractionExecution},{"matching_execution",matchingExecution},{"visual_execution",visualExecution},
        {"effort",QVariantMap{{"policy","resource-only-v1"},{"requested",profile.name},{"supported",effortSupported},{"algorithm",description.value("effort")},{"admission",hostMetrics.value("admission")}}},{"resources",hostMetrics.value("resources")},
        {"utilization",services.request("metrics").value("utilization").toMap()},{"utilization_stages",QVariantMap{{"after_extraction",extractionUtilization},{"after_matching",matchingUtilization}}},{"processes",hostMetrics.value("processes")},
        {"performance", QVariantMap{{"extraction_ms", extractionMs},{"matching_ms",matchingMs},{"visual_ms",visualMs}, {"matching_selection_ms", timer.elapsed()-extractionMs},
            {"stream_resumes",hostMetrics.value("stream_resumes")},{"retried_episodes",retriedEpisodes.load()},{"remote_media_limit",hostMetrics.value("remote_media_limit")},{"boundary_source_opens",boundarySources.load()},
            {"pcm_prefetched",hostMetrics.value("pcm_prefetched")},{"pcm_decodes",hostMetrics.value("pcm_decodes")}, {"pcm_cache_hits",hostMetrics.value("pcm_cache_hits")}, {"peak_pcm_disk_bytes",hostMetrics.value("peak_pcm_disk_bytes")}, {"peak_parallel_pcm_loads",hostMetrics.value("peak_parallel_pcm_loads")}, {"pcm_disk_budget",hostMetrics.value("pcm_disk_budget")},{"scoped_pcm",description.value("scoped_refinement")},{"visual_frames",visualFrames}}}};
    services.publish({{"result",true},{"failed",failed}});
    for(auto it=diagnostics.cbegin();it!=diagnostics.cend();++it)services.publish({{"diagnostic",it.key()},{"value",it.value()}});
}
} // namespace introvert::detection

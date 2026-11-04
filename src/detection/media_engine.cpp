#include "media_engine.h"
#include "algorithm_features.h"
#include "algorithm_pipeline.h"
#include "boundary_scan.h"
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
#include <cstring>

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
namespace {
class HostServices final {
public:
    const providers::ProviderConfig &config;
    const QList<providers::Episode> &episodes;
    const providers::Cancellation &cancel;
    const DetectionProgress &progress;
    const QString jobId,ffmpeg,ffprobe;
    library::MediaCache &cache;
    std::shared_ptr<Algorithm> algorithm;
    QVariantMap description,parameters;
    ResourceProfile profile;
    WorkAdmission admission;
    ResourceLimits processLimits;
    ResourceManager processResources;
    QTemporaryDir temporary;
    const qint64 trackLimit,pcmLimit;
    PcmCache pcmCache;
    MediaWorkGate remoteWork;
    QList<SegmentType> types;
    std::atomic<qint64> descriptorReserved{0},streamResumes{0};
    std::atomic_bool descriptorLimitExceeded{false};
    JobMetrics utilization;
    std::mutex mutex;
    QMap<QString,MediaInfo> preparedMedia;
    std::map<std::pair<QString,SegmentType>,TimeRange> allowedWindows;
    QVariantMap diagnostics;
    bool completed=false,failed=false;
    std::exception_ptr publicationError;
    QString errorCode,errorMessage;
    const Cancellation cancelled;
    static ResourceLimits limits(const ResourceProfile &profile) {
        ResourceLimits limits;limits.decoders=profile.mediaWorkers;limits.application=limits.job=std::max(6,profile.mediaWorkers+2);limits.probes=profile.mediaWorkers;return limits;
    }
    static qint64 diskLimit(const QString &path,qint64 budget) {const auto available=QStorageInfo(path).bytesAvailable();return available>0?std::min(budget,available/4):budget;}
    HostServices(const providers::ProviderConfig &config,const QList<providers::Episode> &episodes,const QStringList &kinds,
        const providers::Cancellation &cancel,const DetectionProgress &progress,QString jobId,QString ffmpeg,QString ffprobe,
        library::MediaCache &cache,std::shared_ptr<Algorithm> algorithm,QVariantMap description,ResourceProfile profile)
        :config(config),episodes(episodes),cancel(cancel),progress(progress),jobId(std::move(jobId)),ffmpeg(std::move(ffmpeg)),ffprobe(std::move(ffprobe)),cache(cache),algorithm(std::move(algorithm)),
        description(std::move(description)),parameters(this->description.value("parameters").toMap()),profile(std::move(profile)),admission(this->profile.workingBytes,this->profile.memoryReserveBytes),
        processLimits(limits(this->profile)),processResources(processLimits),temporary(QDir::tempPath()+"/introvert-detection-XXXXXX"),
        trackLimit(std::min(qint64(256*1024*1024),parameters.value("maximum_track_bytes").toLongLong())),pcmLimit(diskLimit(temporary.path(),this->profile.pcmDiskBytes)),
        pcmCache(pcmLimit),remoteWork(this->profile.mediaWorkers),cancelled([&]{providers::checkCancelled(this->cancel);})
    {
        if(!temporary.isValid())throw providers::ProviderError("Cannot create temporary detection storage.");
        if(trackLimit<=0)throw DetectionError("algorithm_invalid_description","Invalid algorithm work limits.");
        for(const auto &kind:kinds) {if(kind!="intro" && kind!="credits")throw std::invalid_argument("Unsupported audio search type.");const auto type=kind=="intro"?SegmentType::Intro:SegmentType::Credits;if(!types.contains(type))types.append(type);}
        if(types.empty())throw std::invalid_argument("Audio analysis requires requested search types.");
        if(this->jobId.isEmpty())throw std::invalid_argument("Detection job ID is required.");
        QSet<QString> ids;for(const auto &episode:episodes) {if(episode.identity.itemId.isEmpty() || ids.contains(episode.identity.itemId))throw std::invalid_argument("Episode IDs must be non-empty and unique.");ids.insert(episode.identity.itemId);}
    }
    const providers::Episode &episodeFor(const QString &id) const {
        const auto found=std::find_if(episodes.begin(),episodes.end(),[&](const auto &episode){return episode.identity.itemId==id;});
        if(found==episodes.end())throw DetectionError("algorithm_invalid_media_request","Unknown episode.");return *found;
    }
    MediaInfo mediaFor(const QString &id) {std::lock_guard<std::mutex> guard(mutex);const auto found=preparedMedia.constFind(id);if(found==preparedMedia.cend())throw DetectionError("algorithm_invalid_media_request","Unknown inspected episode.");return *found;}
    bool hasMedia(const QString &id) {std::lock_guard<std::mutex> guard(mutex);return preparedMedia.contains(id);}
    struct StreamTotals {
        std::mutex mutex;QVariantMap counts;
        void add(const QVariantMap &values) {std::lock_guard<std::mutex> lock(mutex);for(auto it=values.cbegin();it!=values.cend();++it)counts[it.key()]=counts.value(it.key()).toLongLong()+it.value().toLongLong();}
    };
    StreamTotals streamTotals;
    struct Source {
        QString path; MediaIdentity identity; std::unique_ptr<editor::RangeStream> stream;
        std::unique_ptr<MediaWorkGate::Lease> lease;
        std::atomic<qint64> *resumes;
        StreamTotals *totals;
        explicit Source(std::atomic<qint64> *total,StreamTotals *statistics) : resumes(total),totals(statistics) {}
        Source(Source &&)=default;
        ~Source() { if(stream) {stream->close(); resumes->fetch_add(stream->resumedRequests());totals->add(stream->diagnostics());} }
    };
    Source prepare(const providers::Episode &episode) {
        Source source(&streamResumes,&streamTotals); auto &path = source.path; auto &identity = source.identity; auto &stream = source.stream;
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
            path = "introvert-media://" + digest(cache.scope(config, "detection") + '\n' + episode.identity.itemId);
            identity = {path, size, digest(version).left(15).toLongLong(nullptr, 16)};
        }
        return source;
    }
    struct ResourceLease {
        std::unique_ptr<WorkAdmission::Lease> admission;
        std::optional<Source> source;
        std::unique_ptr<MappedResources> resources;
        QString episode;
    };
    std::map<qint64,std::shared_ptr<ResourceLease>> leases;
    qint64 nextLease=0;
    qint64 keep(std::shared_ptr<ResourceLease> lease) {std::lock_guard<std::mutex> guard(mutex);const auto id=++nextLease;leases[id]=std::move(lease);return id;}
    void retainPcm(const QString &episode,SegmentType type,DecodedAudio &&audio) {const auto rate=uint32_t(audio.window().specification.sampleRate);pcmCache.retain({episode,type,rate},std::move(audio),cancelled);}
    QVariantMap extract(const QString &id) {
        const auto &episode=episodeFor(id); providers::checkCancelled(cancel);
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
                    auto track = extractAlgorithmFeatures(*algorithm, description, resources, media, episode.identity.itemId, jobId, temporary.path(), ffmpeg, nullptr, cancelled,
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
        QVariantList encoded;
        for(const auto &track:tracks)encoded.append(featureMessage(*track));
        {
            std::lock_guard<std::mutex> guard(mutex);
            this->preparedMedia.insert(preparedMedia);
            this->allowedWindows.insert(allowedWindows.begin(),allowedWindows.end());
        }
        return {{"tracks",encoded},{"failures",failures},{"cache",cacheOperations},{"coverage",coverage},{"search",searchWindows},{"bytes",retainedBytes},
            {"video",preparedMedia.contains(id) && preparedMedia.value(id).selectedVideo.has_value()}};
    }
    std::vector<qint16> read(const IntrovertDetectorAudioReadV1 &request) {
        if(!request.episode || !request.episode_size || request.episode_size>65536 || request.segment_type>1 || request.sample_rate<8000 || request.sample_rate>96000 || request.first<0 || !request.count || request.count>262144)
            throw DetectionError("algorithm_invalid_audio_read","Invalid scoped PCM read.");
        const auto episode=QString::fromUtf8(reinterpret_cast<const char *>(request.episode),qsizetype(request.episode_size));
        const auto type=SegmentType(request.segment_type);const auto rate=request.sample_rate;const auto first=request.first;const auto count=request.count;
        const TimeRange window(request.window_start,request.window_end),part(request.start,request.end);
        const auto media=mediaFor(episode);
        bool allowed=false;for(const auto &scope:planWindows(media.duration,types))if(scope.type==type && scope.range.contains(window))allowed=true;
        if(!allowed || !window.contains(part))throw DetectionError("algorithm_invalid_audio_read","PCM read exceeds its hard search window.");
                        if(first>qint64(std::floor(part.duration()*rate+1e-8))-count)
                            throw DetectionError("algorithm_invalid_audio_read","PCM read exceeds its hard search window.");
                        if(const auto full=pcmCache.retained({episode,type,rate})) {
                            const auto offset=first+qint64(std::llround((part.start-window.start)*rate));
                            const auto &audioWindow=full->window();const auto available=std::min(qint64(count),std::max(qint64(0),audioWindow.sampleCount-offset));
                            return available?audioWindow.readSamples(offset,qsizetype(available)):std::vector<qint16>{};
                        }
                        const auto decode=[&](const TimeRange &part) {
                            const auto item=std::find_if(episodes.begin(),episodes.end(),[&](const auto &candidate){return candidate.identity.itemId==episode;});
                            if(item==episodes.end() || !hasMedia(episode))throw DetectionError("algorithm_invalid_audio_read","Unknown audio source.");
                            const auto attempt=[&] {
                                auto source=prepare(*item);const auto media=mediaFor(episode);
                                if(source.identity.normalizedPath!=media.identity.normalizedPath || source.identity.fileSize!=media.identity.fileSize || source.identity.modifiedTimeNs!=media.identity.modifiedTimeNs)
                                    throw DetectionError("media_changed","Episode media changed during detection.");
                                MappedResources workerResources(processResources,profile.decoderThreads);
                                workerResources.paths.insert(source.path,source.stream?source.stream->url().toString():source.path);
                                AudioDecodeSpecification spec;spec.sampleRate=int(rate);
                                auto audio=decodeAudio(workerResources,media,episode,type,part,jobId,temporary.path(),cancelled,spec,{},300.,{},ffmpeg);
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
                        const bool full=part.start==window.start && part.end==window.end;
                        const auto audio=pcmCache.get({episode,type,rate,full?-1:qint64(std::llround((part.start-window.start)*rate)),full?-1:qint64(std::llround((part.end-window.start)*rate))},bytes,[&]{return decode(part);},cancelled);
                        const auto available=std::min(qint64(count),std::max(qint64(0),audio->window().sampleCount-first));
                        return available?audio->window().readSamples(first,qsizetype(available)):std::vector<qint16>{};
    }
    QVariantMap snapshot() {
        auto metrics=pcmCache.statistics();auto resources=profile.diagnostics();resources["concurrent_decoders"]=processLimits.decoders;
        metrics["resources"]=resources;metrics["utilization"]=utilization.diagnostics();metrics["processes"]=processResources.statistics();metrics["admission"]=admission.diagnostics();
        metrics["stream_resumes"]=streamResumes.load();metrics["remote_media_limit"]=remoteWork.limit();metrics["pcm_disk_budget"]=pcmLimit;metrics["descriptor_limit_exceeded"]=descriptorLimitExceeded.load();
        {std::lock_guard<std::mutex> guard(streamTotals.mutex);metrics["media_streams"]=streamTotals.counts;}
        return metrics;
    }
    QVariantMap request(const QString &operation,const QVariantMap &input) {
        if(operation=="release") {
            std::shared_ptr<ResourceLease> released;
            {std::lock_guard<std::mutex> guard(mutex);const auto found=leases.find(input.value("id").toLongLong());if(found==leases.end())throw DetectionError("algorithm_invalid_lease","Unknown resource lease.");released=std::move(found->second);leases.erase(found);}
            return {};
        }
        providers::checkCancelled(cancel);
        if(operation=="capabilities")return {{"version",1},{"services",QStringList{"inspect_episode","extract_episode","read_pcm","scan","open_media","admit","release","metrics","retry_episode"}},
            {"audio_format","mono-s16-v1"},{"video_format","signalstats-yuv-v1"}};
        if(operation=="metrics")return snapshot();
        if(operation=="inspect_episode") {
            auto reservation=admission.acquire(256LL*1024*1024,cancelled);auto source=prepare(episodeFor(input.value("episode").toString()));
            const auto episode=input.value("episode").toString();MappedResources resources(processResources,profile.decoderThreads);
            resources.paths.insert(source.path,source.stream?source.stream->url().toString():source.path);
            const auto media=inspectMedia(resources,source.path,episode,jobId,cancelled,ffprobe,[identity=source.identity](const QString &){return identity;});
            QVariantList windows;
            {std::lock_guard<std::mutex> guard(mutex);preparedMedia.insert(episode,media);
                for(const auto &window:planWindows(media.duration,types)) {
                    allowedWindows.insert_or_assign({episode,window.type},window.range);
                    windows.append(QVariantMap{{"segment_type",segmentName(window.type)},{"start",window.range.start},{"end",window.range.end}});
                }
            }
            return {{"episode",episode},{"duration",media.duration},{"audio_end",media.audioEnd?QVariant(*media.audioEnd):QVariant{}},
                {"audio",media.selectedAudio.has_value()},{"video",media.selectedVideo.has_value()},{"windows",windows}};
        }
        if(operation=="extract_episode")return extract(input.value("episode").toString());
        if(operation=="retry_episode") {
            const auto bytes=input.value("bytes").toLongLong();
            if(bytes<0 || bytes>descriptorReserved.load())throw DetectionError("algorithm_invalid_result","Invalid descriptor reservation.");
            remoteWork.reduceToOne();descriptorReserved.fetch_sub(bytes);return {};
        }
        if(operation=="admit") {
            const auto bytes=input.value("bytes").toLongLong();if(bytes<=0 || bytes>8LL*1024*1024*1024)throw DetectionError("algorithm_invalid_lease","Invalid work reservation.");
            auto lease=std::make_shared<ResourceLease>();lease->admission=admission.acquire(bytes,cancelled);return {{"id",keep(std::move(lease))}};
        }
        if(operation=="open_media") {
            auto lease=std::make_shared<ResourceLease>();lease->episode=input.value("episode").toString();const auto media=mediaFor(lease->episode);
            lease->source.emplace(prepare(episodeFor(lease->episode)));const auto &source=*lease->source;
            if(source.identity.normalizedPath!=media.identity.normalizedPath || source.identity.fileSize!=media.identity.fileSize || source.identity.modifiedTimeNs!=media.identity.modifiedTimeNs)
                throw DetectionError("media_changed","Episode media changed during detection.");
            lease->resources=std::make_unique<MappedResources>(processResources,profile.decoderThreads);lease->resources->paths.insert(source.path,source.stream?source.stream->url().toString():source.path);
            return {{"id",keep(std::move(lease))}};
        }
        if(operation=="scan") {
            const auto episode=input.value("episode").toString();const auto media=mediaFor(episode);const auto kind=input.value("segment_type").toString();
            if(kind!="intro" && kind!="credits")throw DetectionError("visual_invalid_scope","Invalid bounded video request.");
            const auto type=kind=="intro"?SegmentType::Intro:SegmentType::Credits;const TimeRange hard(input.value("start").toDouble(),input.value("end").toDouble());
            bool allowed=false;for(const auto &scope:planWindows(media.duration,types))if(scope.type==type && scope.range.start==hard.start && scope.range.end==hard.end)allowed=true;
            if(!allowed)throw DetectionError("visual_invalid_scope","Invalid bounded video request.");
            const auto reference=input.value("reference").toDouble();std::shared_ptr<ResourceLease> lease;
            {std::lock_guard<std::mutex> guard(mutex);const auto found=leases.find(input.value("source").toLongLong());if(found!=leases.end())lease=found->second;}
            const auto descriptor=input.value("description").toMap();
            if(reference==hard.start || reference==hard.end) {MappedResources resources(processResources,profile.decoderThreads);return scanAlgorithmBoundary(descriptor,resources,media,episode,jobId,ffmpeg,{type,hard},reference,cancelled);}
            if(!lease || !lease->resources || lease->episode!=episode)throw DetectionError("visual_invalid_scope","Missing media lease.");
            return scanAlgorithmBoundary(descriptor,*lease->resources,media,episode,jobId,ffmpeg,{type,hard},reference,cancelled);
        }
        throw DetectionError("algorithm_unknown_service","Unsupported detector host service.");
    }
    static int respond(void *context,IntrovertDetectorEmitV1 callback,const QVariantMap &value) {
        try {const auto bytes=algorithmMessage(value);return callback(context,reinterpret_cast<const uint8_t *>(bytes.constData()),uint64_t(bytes.size()));}catch(...) {return 0;}
    }
    template<class Work> static int reply(void *context,IntrovertDetectorEmitV1 callback,Work work) {
        try {return respond(context,callback,{{"ok",true},{"value",work()}});}
        catch(const providers::Cancelled &) {return respond(context,callback,{{"ok",false},{"code","cancelled"}});}
        catch(const DetectionError &error) {return respond(context,callback,{{"ok",false},{"code",error.code},{"message",QString::fromUtf8(error.what())}});}
        catch(...) {return respond(context,callback,{{"ok",false},{"code","host_failure"},{"message","The detector host service could not complete."}});}
    }
    static int cancelledCallback(void *context) {try{providers::checkCancelled(static_cast<HostServices *>(context)->cancel);return 0;}catch(...) {return 1;}}
    static int requestCallback(void *context,const char *operation,const uint8_t *data,uint64_t size,void *output,IntrovertDetectorEmitV1 callback) {
        if(!operation || !data || !size || size>uint64_t(algorithmMessageLimit) || !callback)return 0;
        return reply(output,callback,[&]{return static_cast<HostServices *>(context)->request(QString::fromLatin1(operation),parseAlgorithmMessage(QByteArray::fromRawData(reinterpret_cast<const char *>(data),qsizetype(size))));});
    }
    static int64_t readCallback(void *context,const IntrovertDetectorAudioReadV1 *request,int16_t *output,void *error,IntrovertDetectorEmitV1 callback) {
        if(!request || !output || !callback)return -1;
        try {
            auto &host=*static_cast<HostServices *>(context);providers::checkCancelled(host.cancel);const auto samples=host.read(*request);
            if(samples.size()>request->count)throw DetectionError("algorithm_invalid_audio_read","PCM response exceeded its request.");
            if(!samples.empty())std::memcpy(output,samples.data(),samples.size()*sizeof(int16_t));return int64_t(samples.size());
        } catch(const providers::Cancelled &) {respond(error,callback,{{"ok",false},{"code","cancelled"}});}
        catch(const DetectionError &failure) {respond(error,callback,{{"ok",false},{"code",failure.code},{"message",QString::fromUtf8(failure.what())}});}
        catch(...) {respond(error,callback,{{"ok",false},{"code","host_failure"},{"message","The host could not read scoped PCM."}});}
        return -1;
    }
    static int publishCallback(void *context,const uint8_t *data,uint64_t size) {
        auto &host=*static_cast<HostServices *>(context);
        try {
            if(!data || !size || size>uint64_t(algorithmMessageLimit))throw DetectionError("algorithm_invalid_result","Invalid detector session output.");
            const auto value=parseAlgorithmMessage(QByteArray::fromRawData(reinterpret_cast<const char *>(data),qsizetype(size)));
            if(value.contains("progress")) {if(host.progress)host.progress(value.value("progress").toString(),value.value("current").toInt(),value.value("total").toInt());}
            else if(value.contains("error")) {host.errorCode=value.value("error").toString();host.errorMessage=value.value("message").toString();}
            else if(value.contains("diagnostic")) {const auto key=value.value("diagnostic").toString();if(key.isEmpty() || host.diagnostics.contains(key))throw DetectionError("algorithm_invalid_result","Repeated detector diagnostic field.");host.diagnostics[key]=value.value("value");}
            else if(value.value("result").toBool() && !host.completed && value.value("failed").metaType().id()==QMetaType::Bool) {host.completed=true;host.failed=value.value("failed").toBool();}
            else throw DetectionError("algorithm_invalid_result","Invalid detector session event.");
            return 1;
        } catch(...) {host.publicationError=std::current_exception();return 0;}
    }
    IntrovertDetectorServicesV1 api() {return {sizeof(IntrovertDetectorServicesV1),1,this,cancelledCallback,requestCallback,readCallback,publishCallback};}
    MediaDetectionOutput finish() {
        if(publicationError)std::rethrow_exception(publicationError);
        if(!errorCode.isEmpty()) {
            if(errorCode=="cancelled")throw providers::Cancelled();
            if(errorCode=="invalid_request")throw std::invalid_argument(errorMessage.toStdString());
            throw DetectionError(errorCode,errorMessage);
        }
        if(!completed || diagnostics.value("selected").metaType().id()!=QMetaType::QVariantList)throw DetectionError("algorithm_invalid_result","The detector session result is incomplete.");
        MediaDetectionOutput output;output.failed=failed;output.diagnostics=std::move(diagnostics);
        for(const auto &episode:episodes)output.episodes[episode.identity.itemId]={};
        QSet<QString> assigned;
        const auto algorithm=this->algorithm;
        const auto allowedWindows=this->allowedWindows;
        for(const auto &value:output.diagnostics.value("selected").toList()) {
            const auto segment=value.toMap();const auto episode=segment.value("episode").toString(),kind=segment.value("segment_type").toString();
            const auto type=kind=="intro"?SegmentType::Intro:SegmentType::Credits;const TimeRange range(segment.value("start").toDouble(),segment.value("end").toDouble());
            const auto allowed=allowedWindows.find({episode,type});
            if((kind!="intro" && kind!="credits") || allowed==allowedWindows.end() || !allowed->second.contains(range) || assigned.contains(episode+'\n'+kind))throw DetectionError("algorithm_invalid_result","Selected segment exceeds its scoped request.");
            assigned.insert(episode+'\n'+kind);output.episodes[episode][kind]={range.start,range.end};
        }
    if (description.value("finalization").toString() == "segments-v1") {
        QMap<QString, double> durations; for (auto it = preparedMedia.cbegin(); it != preparedMedia.cend(); ++it) durations[it.key()] = it->duration;
        output.finalize = [algorithm, durations, allowedWindows](const QString &episode, const QString &kind, const QPair<double, double> &measured,
            const segments::SegmentValues &saved, const providers::Cancellation &cancel) {
            QVariantMap segments; for (auto it = saved.cbegin(); it != saved.cend(); ++it) segments[it.key()] = QVariantList{it->first, it->second};
            try {
                const auto result = algorithm->invoke("finalize", {{"episode", episode}, {"segment_type", kind},
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
        return output;
    }
};
} // namespace
MediaDetectionEngine::MediaDetectionEngine(QString path,library::MediaCache &cache,QString ffmpeg,QString ffprobe,QVariantMap parameters,QString algorithmRoot,QString resources)
    :m_path(std::move(path)),m_ffmpeg(std::move(ffmpeg)),m_ffprobe(std::move(ffprobe)),m_algorithmRoot(std::move(algorithmRoot)),m_resourceProfile(std::move(resources)),m_parameters(std::move(parameters)),m_cache(cache) {}
MediaDetectionOutput MediaDetectionEngine::run(const providers::ProviderConfig &config,const QList<providers::Episode> &episodes,const QStringList &kinds,
    const providers::Cancellation &cancel,const DetectionProgress &progress,const QString &jobId)
try {
    const SettingsStore settings(m_path);const auto requestedEffort=m_resourceProfile.isEmpty()?settings.detectionResources():m_resourceProfile;
    const bool visualRefinementEnabled=settings.visualRefinementEnabled();
    static QSemaphore execution(1);
    while(!execution.tryAcquire(1,20))providers::checkCancelled(cancel);
    struct ExecutionScope {QSemaphore &gate;QThread *thread=QThread::currentThread();QThread::Priority priority=thread->priority();QThread::QualityOfService qos=thread->serviceLevel();
        ~ExecutionScope() {thread->setServiceLevel(qos);if(priority!=QThread::InheritPriority)thread->setPriority(priority);gate.release();}} scope{execution};
    providers::checkCancelled(cancel);auto profile=resourceProfile(requestedEffort);
    QThread::currentThread()->setPriority(profile.priority);QThread::currentThread()->setServiceLevel(profile.serviceLevel);
    const auto package=AlgorithmPackages(m_path,m_algorithmRoot).selected();const auto algorithm=std::make_shared<Algorithm>(package);
    const auto description=algorithm->description(m_parameters,"high");
    if(!description.value("parallel_matches").toBool())profile.matchingWorkers=1;
    if(!description.value("parallel_extraction").toBool())profile.mediaWorkers=1;
    if(!description.value("parallel_visual").toBool())profile.visualWorkers=1;
    HostServices host(config,episodes,kinds,cancel,progress,jobId,m_ffmpeg.isEmpty()?binary("ffmpeg"):m_ffmpeg,m_ffprobe.isEmpty()?binary("ffprobe"):m_ffprobe,m_cache,algorithm,description,profile);
    QVariantList ids;for(const auto &episode:episodes)ids.append(episode.identity.itemId);
    const QVariantMap input{{"episodes",ids},{"kinds",kinds},{"parameters",m_parameters},{"visual_refinement",visualRefinementEnabled},{"remote",config.provider!=providers::ProviderId::Local},
        {"resources",profile.diagnostics()},{"priority",int(profile.priority)},{"qos",int(profile.serviceLevel)},{"manifest_hash",package.manifestHash}};
    const auto services=host.api();
    if(algorithm->hasSession())algorithm->runSession(input,services);
    else runAlgorithmPipeline(*algorithm,input,PipelineServices(services));
    return host.finish();
} catch(const detection::Cancelled &) {throw providers::Cancelled();}
} // namespace introvert::detection

#include "algorithm_video.h"
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>

namespace introvert::detection {
namespace {
QVariantList decodeObservations(Algorithm &algorithm, ProcessRunner &runner, const MediaInfo &media, const QString &episode,
    const QString &job, const QString &ffmpeg, const TimeRange &window, int width, int height, int columns, double fps, const Cancellation &cancel, QVariantMap *timings)
{
    if (!media.selectedVideo || !TimeRange(0,media.duration).contains(window) || window.duration()>600.000001
        || width<64 || width>1280 || height<64 || height>720 || columns<1 || columns>512 || fps<.5 || fps>16)
        throw DetectionError("visual_invalid_scope","Invalid bounded video request.");
    const auto frameBytes=qint64(width)*height*3; const auto expected=qint64(std::ceil(window.duration()*fps-1e-8));
    if (expected>2400) throw DetectionError("visual_frame_limit","Video frame budget exceeded.");
    const double fast=std::max(0.,window.start-5.);
    const double relative=window.start-fast;
    const auto filter="trim=start="+mediaDecimal(relative)+":end="+mediaDecimal(relative+window.duration())
        +",setpts=PTS-STARTPTS,fps=fps="+mediaDecimal(fps)+":start_time=0:round=near,scale="+QString::number(width)+':'+QString::number(height)
        +":force_original_aspect_ratio=decrease:flags=area,pad="+QString::number(width)+':'+QString::number(height)+":(ow-iw)/2:(oh-ih)/2,format=rgb24";
    ProcessRequest request{{ffmpeg,"-hide_banner","-nostdin","-loglevel","error","-ss",mediaDecimal(fast),"-i",media.identity.normalizedPath,
        "-map","0:"+QString::number(*media.selectedVideo),"-t",mediaDecimal(window.duration()),
        "-an","-sn","-dn","-vf",filter,"-pix_fmt","rgb24","-f","rawvideo","pipe:1"},ProcessCategory::Decoder,"visual-frame-stream",job,episode,SegmentType::Credits,300.};
    QByteArray pending; QVariantList observations; qint64 frames=0,recognitionNs=0;
    request.maximumStreamBytes=(expected+1)*frameBytes;
    request.stdoutConsumer=[&](const QByteArray &bytes) {
        checkCancelled(cancel); pending.append(bytes);
        while (pending.size()>=frameBytes) {
            const double t=window.start+double(frames)/fps;
            if (frames>=expected || t>=window.end) throw DetectionError("visual_frame_limit","Decoder exceeded the scoped frame grid.");
            QElapsedTimer recognition; recognition.start();
            const auto result=algorithm.invoke("observe_video_frame",{{"pixels",pending.left(frameBytes)},{"width",width},{"height",height}},cancel);
            recognitionNs+=recognition.nsecsElapsed();
            const auto rawValues=result.value("values").toList(); QVariantList values;
            if (rawValues.size()!=columns) throw DetectionError("visual_invalid_features","Invalid visual feature layout.");
            for (const auto &v:rawValues) {
                const float f=float(v.toDouble()); if (!std::isfinite(f) || f<0) throw DetectionError("visual_invalid_features","Invalid visual feature value.");
                values.append(double(f)); // Match the persisted feature precision on cold and warm scans.
            }
            observations.append(QVariantMap{{"time",t},{"values",values}}); ++frames; pending.remove(0,frameBytes);
        }
    };
    const auto result=runner.run(request,cancel); checkCancelled(cancel);
    if(timings) {
        (*timings)["recognition_ms"]=timings->value("recognition_ms").toDouble()+recognitionNs/1e6;
        (*timings)["stream_and_decode_ms"]=timings->value("stream_and_decode_ms").toDouble()+std::max(0.,result.runtimeSeconds*1000.-recognitionNs/1e6);
    }
    if (result.cancelled) throw Cancelled();
    if (result.timedOut) throw DetectionError("visual_decode_timeout","Visual analysis exceeded its time budget.");
    if (result.outputLimitExceeded || result.exitCode!=0 || !pending.isEmpty() || frames==0 || std::abs(double(frames)/fps-window.duration())>1./fps+.05)
        throw DetectionError("visual_decode_incomplete","Scoped video coverage is incomplete.");
    return observations;
}
}
QVariantMap extractAlgorithmVideo(Algorithm &algorithm, const QVariantMap &description, ProcessRunner &runner,
    const MediaInfo &media, const QString &episode, const QString &job, const QString &ffmpeg, FeatureCache *cache,
    const ScanWindow &hardWindow, double audioStart, const Cancellation &cancel, const DiagnosticSink &sink,
    const std::function<void(const QVariantList &,bool)> &observe)
{
    QElapsedTimer timer; timer.start(); QVariantMap timings;
    const auto d=description.value("visual").toMap(); if (!d.value("available").toBool()) return {{"status","unavailable"}};
    const double radius=std::min(15.,d.value("edge_radius_seconds",5.).toDouble());
    if (!std::isfinite(audioStart) || !std::isfinite(radius) || radius<=0 || audioStart<hardWindow.range.start || audioStart>=hardWindow.range.end)
        throw DetectionError("visual_invalid_scope","Visual refinement requires an audio start within its hard window.");
    const ScanWindow scope{hardWindow.type,{std::max(hardWindow.range.start,audioStart-radius),std::min(hardWindow.range.end,audioStart+radius)}};
    const int width=d.value("width").toInt(),height=d.value("height").toInt(),columns=d.value("columns").toInt();
    const double fps=d.value("frames_per_second").toDouble(),denseFps=d.value("refinement_frames_per_second").toDouble();
    if (columns<1 || columns>512 || width<64 || width>1280 || height<64 || height>720 || !std::isfinite(fps) || fps<.5 || fps>4
        || !std::isfinite(denseFps) || denseFps<fps || denseFps>16 || scope.type!=SegmentType::Credits)
        throw DetectionError("visual_invalid_description","Unsupported bounded video format.");
    auto config=d.value("configuration").toList(); config.append(QVariant(QVariantList{"cadence",fps}));
    config.append(QVariant(QVariantList{"decoder","rgb-stream-trim-cfr-fit-pad-v1"}));
    config.append(QVariant(QVariantList{"width",width})); config.append(QVariant(QVariantList{"height",height}));
    for (const auto &entry:config) if (entry.metaType().id()!=QMetaType::QVariantList || entry.toList().size()!=2)
        throw DetectionError("visual_invalid_description","Invalid visual cache configuration.");
    const auto request=buildFeatureCacheRequest(media,episode,scope.type,scope.range,d.value("extractor").toString(),d.value("version").toString(),d.value("schema").toString(),config,"video");
    std::shared_ptr<const FeatureTrack> track;
    if (cache) try { const auto read=cache->read(request); track=read.track; if (sink) sink(read.operation); } catch (...) { if (sink) sink(cacheOperation(request,"error",0,0,"visual_cache_read")); }
    const auto expected=qint64(std::ceil(scope.range.duration()*fps-1e-8));
    if (expected>2400 || expected*(sizeof(double)+columns*sizeof(float))>16*1024*1024)
        throw DetectionError("visual_feature_limit","Visual features exceed their work budget.");
    if (track && (track->columns!=size_t(columns) || qint64(track->rows())!=expected || track->episodeId!=episode)) track.reset();
    QVariantList observations; bool decoded=false;
    if (track) {
        for (size_t i=0;i<track->rows();++i) {
            if (std::abs(track->timestamps[i]-(scope.range.start+double(i)/fps))>.00001) { track.reset(); observations.clear(); break; }
            QVariantList values; for (int c=0;c<columns;++c) values.append(double(track->values[i*columns+c]));
            observations.append(QVariantMap{{"time",track->timestamps[i]},{"values",values}});
        }
        if (track) try { algorithm.invoke("visual_candidates",{{"observations",observations},{"step",1./fps},{"start",scope.range.start},{"end",scope.range.end}},cancel); }
        catch (const Cancelled &) { throw; }
        catch (...) { track.reset(); observations.clear(); if (sink) sink(cacheOperation(request,"corrupt",0,0,"visual_layout_mismatch")); }
    }
    if (!track) {
        observations=decodeObservations(algorithm,runner,media,episode,job,ffmpeg,scope.range,width,height,columns,fps,cancel,&timings); decoded=true;
        std::vector<double> times; std::vector<float> values;
        for (const auto &o:observations) { times.push_back(o.toMap().value("time").toDouble()); for (const auto &v:o.toMap().value("values").toList()) values.push_back(float(v.toDouble())); }
        track=std::make_shared<FeatureTrack>(episode,scope.type,scope.range,QString(),d.value("extractor").toString(),d.value("version").toString(),d.value("schema").toString(),std::move(times),std::move(values),size_t(columns));
        if (cache) try { const auto written=cache->write(request,track); if (sink) sink(written.operation); } catch (...) { if (sink) sink(cacheOperation(request,"error",0,0,"visual_cache_write")); }
    }
    if(observe)observe(observations,false);
    auto found=algorithm.invoke("visual_candidates",{{"observations",observations},{"step",1./fps},{"start",scope.range.start},{"end",scope.range.end}},cancel);
    auto candidates=found.value("candidates").toList(); QVariantList refined; int denseFrames=0;
    const int candidateLimit=std::clamp(d.value("maximum_refinement_candidates",4).toInt(),1,8);
    if (candidates.size()>candidateLimit) { found["limited"]=true; found["limitation"]="visual_refinement_candidate_limit"; }
    for (int i=0;i<candidates.size();++i) {
        checkCancelled(cancel); const auto c=candidates[i].toMap();
        if (i>=candidateLimit) { refined.append(c); continue; }
        const double start=c.value("start").toDouble();
        const TimeRange local(std::max(scope.range.start,start-2.),std::min(scope.range.end,start+2.));
        const auto dense=decodeObservations(algorithm,runner,media,episode,job,ffmpeg,local,width,height,columns,denseFps,cancel,&timings); denseFrames+=dense.size();
        if(observe)observe(dense,true);
        const auto r=algorithm.invoke("refine_visual",{{"candidates",QVariantList{c}},{"observations",dense},{"start",scope.range.start},{"end",scope.range.end}},cancel);
        refined.append(r.value("candidates").toList());
    }
    found["candidates"]=refined; found["coarse_frames"]=observations.size(); found["dense_frames"]=denseFrames; found["decoded"]=decoded;
    found["audio_start"]=audioStart; found["edge_radius_seconds"]=radius;
    found["window_start"]=scope.range.start; found["window_end"]=scope.range.end; found["backend"]=d.value("version");
    found["runtime_ms"]=timer.elapsed(); found["timing"]=timings;
    found["timestamp_basis"]="window-relative-cfr-grid";
    return found;
}
} // namespace introvert::detection

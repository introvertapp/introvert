#include "boundary_scan.h"
#include <QElapsedTimer>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace introvert::detection {
QVariantList parseBoundaryStatistics(const QByteArray &output,const TimeRange &window,const Cancellation &cancel)
{
    if(output.size()>4*1024*1024)throw DetectionError("visual_frame_limit","Boundary statistics exceeded their byte budget.");
    static const QRegularExpression header(QStringLiteral("^frame:([0-9]+)\\s+pts:[-0-9]+\\s+pts_time:([^\\s]+)\\s*$"));
    static const QMap<QByteArray,QString> fields{{"YLOW","low"},{"YAVG","mean"},{"YHIGH","high"},{"YMAX","maximum"},{"SATAVG","saturation"},{"YDIF","difference"}};
    QVariantList observations;QVariantMap current;double previous=-1.;int expected=0;
    const auto append=[&] {
        if(current.empty())return;
        if(current.size()!=7)throw DetectionError("visual_invalid_observations","Incomplete boundary statistics.");
        observations.append(current);current.clear();
    };
    for(const auto &raw:output.split('\n')) {
        checkCancelled(cancel);const auto line=raw.trimmed();if(line.isEmpty())continue;
        if(line.startsWith("frame:")) {
            append();const auto match=header.match(QString::fromLatin1(line));bool valid=false;
            const double relative=match.captured(2).toDouble(&valid);
            if(!match.hasMatch() || !valid || !std::isfinite(relative) || match.captured(1).toInt()!=expected++ || expected>2048
                || relative<0 || relative>=window.duration() || relative<=previous)
                throw DetectionError("visual_invalid_observations","Invalid boundary frame timestamps.");
            previous=relative;current["time"]=window.start+relative;
        } else if(line.startsWith("lavfi.signalstats.")) {
            const auto separator=line.indexOf('=');const auto name=line.mid(18,separator-18);const auto field=fields.constFind(name);
            if(field==fields.cend())continue;
            bool valid=false;const double value=line.mid(separator+1).toDouble(&valid);
            if(current.empty() || current.contains(*field) || !valid || !std::isfinite(value) || value<0 || value>255.)
                throw DetectionError("visual_invalid_observations","Invalid boundary frame statistics.");
            current[*field]=value;
        } else throw DetectionError("visual_invalid_observations","Unexpected boundary scan output.");
    }
    append();return observations;
}
QVariantMap scanAlgorithmBoundary(const QVariantMap &description,ProcessRunner &runner,const MediaInfo &media,
    const QString &episode,const QString &job,const QString &ffmpeg,const ScanWindow &hardWindow,double reference,const Cancellation &cancel)
{
    const auto d=description.value("boundary_refinement").toMap();
    if(!d.value("available").toBool())return {{"status","unavailable"},{"reason","boundary_refinement_unavailable"}};
    if(d.value("feature_format")!="signalstats-yuv-v1"
        || d.value("width").toInt()!=160 || d.value("height").toInt()!=96 || d.value("edge_radius_seconds").toDouble()!=3.
        || d.value("maximum_frames").toInt()!=2048 || d.value("maximum_scan_seconds").toDouble()!=6. || d.value("timeout_seconds").toDouble()!=30.)
        throw DetectionError("visual_invalid_description","Unsupported boundary scan contract.");
    if(!media.selectedVideo || !std::isfinite(reference) || reference<hardWindow.range.start || reference>hardWindow.range.end
        || !TimeRange(0,media.duration).contains(hardWindow.range))throw DetectionError("visual_invalid_scope","Invalid bounded video request.");
    if(reference==hardWindow.range.start || reference==hardWindow.range.end)
        return {{"status","unavailable"},{"reason","boundary_at_search_limit"},{"frames",0}};
    const TimeRange window(std::max(hardWindow.range.start,reference-3.),std::min(hardWindow.range.end,reference+3.));
    QElapsedTimer elapsed;elapsed.start();
    // One bounded decode at native frame timestamps. Metadata stays in the
    // worker; no RGB images, per-frame ABI calls, OCR or second dense seek.
    // Scale before statistics and keep source range explicit across platforms.
    const QString filters="trim=start=0:end="+mediaDecimal(window.duration())+",scale=160:96:flags=area:out_range=tv,format=yuv420p,signalstats,metadata=mode=print:file=-";
    ProcessRequest request{{ffmpeg,"-hide_banner","-nostdin","-loglevel","error","-ss",mediaDecimal(window.start),"-i",media.identity.normalizedPath,
        "-map","0:"+QString::number(*media.selectedVideo),"-t",mediaDecimal(window.duration()),"-an","-sn","-dn","-vf",filters,
        "-frames:v","2049","-f","null","-"},ProcessCategory::Decoder,"boundary-statistics",job,episode,hardWindow.type,30.};
    const auto result=runner.run(request,cancel);checkCancelled(cancel);
    if(result.cancelled)throw Cancelled();
    if(result.timedOut)throw DetectionError("visual_decode_timeout","Boundary analysis exceeded its time budget.");
    if(result.outputLimitExceeded || result.exitCode!=0)throw DetectionError("visual_decode_incomplete","Bounded boundary scan did not complete.");
    const auto observations=parseBoundaryStatistics(result.out,window,cancel);
    if(observations.empty())throw DetectionError("visual_decode_incomplete","Boundary scan returned no frames.");
    return {{"status","analyzed"},{"decoded",true},{"window_start",window.start},{"window_end",window.end},
        {"audio_time",reference},{"observations",observations},{"frames",observations.size()},{"metadata_bytes",result.out.size()},
        {"backend",d.value("version")},{"timestamp_basis","source-frame-pts"},{"runtime_ms",elapsed.elapsed()}};
}
}

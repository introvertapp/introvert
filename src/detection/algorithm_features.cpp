#include "algorithm_features.h"
#include "media_probe.h"
#include <algorithm>
#include <cmath>

namespace introvert::detection {
std::shared_ptr<const FeatureTrack> extractAlgorithmFeatures(Algorithm &algorithm, const QVariantMap &d,
    ProcessRunner &runner, const MediaInfo &media, const QString &episode, const QString &job, const QString &root,
    const QString &ffmpeg, FeatureCache *cache, const Cancellation &cancel, const DiagnosticSink &sink,
    const QVariantMap &parameters, const ScanWindow &scope, int refinementRate, const std::function<void(DecodedAudio &&)> &retainRefinement)
{
    checkCancelled(cancel);
    if (!media.selectedAudio) throw DetectionError("audio_stream_unavailable", "Media has no usable audio stream.");
    if (!TimeRange(0, media.duration).contains(scope.range) || scope.range.duration() > 600. + 1e-8) throw DetectionError("algorithm_invalid_window", "Audio window exceeds its hard search limit.");
    const auto allowed = audioAnalysisWindow(media,scope.range);
    const int rate = d.value("sample_rate").toInt(), hop = d.value("hop_samples").toInt(), context = d.value("context_samples").toInt(), columns = d.value("columns").toInt();
    const qint64 requestedSamples = qint64(std::floor(allowed.duration() * rate + 1e-8));
    const auto frames = [&](qint64 samples) { return samples < context ? qint64(0) : (samples - context) / hop + 1; };
    const auto expected = frames(requestedSamples);
    if (expected * (qint64(sizeof(double)) + columns * qint64(sizeof(float))) > parameters.value("maximum_track_bytes").toLongLong())
        throw DetectionError("raw_audio_limit_exceeded", "Audio analysis exceeded its configured resource budget.");
    const auto request = buildFeatureCacheRequest(media, episode, scope.type, allowed, d.value("extractor").toString(), d.value("extractor_version").toString(), d.value("schema").toString(), d.value("configuration").toList());
    const auto validate = [&](const FeatureTrack &track) {
        if (track.episodeId != episode || track.type != scope.type || track.window.start != allowed.start || track.window.end != allowed.end
            || track.extractorId != request.extractorId || track.extractorVersion != request.extractorVersion || track.schemaVersion != request.schemaVersion
            || track.columns != size_t(columns) || track.rows() > size_t(expected) || expected - qint64(track.rows()) > 1)
            throw DetectionError("algorithm_invalid_features", "Algorithm features do not match their scoped request.");
        algorithm.invoke("validate", {{"track", featureMessage(track)}, {"parameters", parameters}}, cancel);
    };
    std::shared_ptr<const FeatureTrack> track; bool decoded = false;
    if (cache) try { const auto read = cache->read(request); track = read.track; if (sink) sink(read.operation); }
    catch (const Cancelled &) { throw; } catch (...) { if (sink) sink(cacheOperation(request, "error", 0, 0, "cache_read_error")); }
    if (track) try { validate(*track); }
    catch (const Cancelled &) { throw; } catch (...) { track.reset(); if (sink) sink(cacheOperation(request, "corrupt", 0, 0, "fingerprint_layout_mismatch")); }
    if (!track) {
        QVariantMap input{{"episode", episode}, {"type", scope.type == SegmentType::Intro ? "intro" : "credits"}, {"start", allowed.start}, {"end", allowed.end}, {"samples", requestedSamples}, {"parameters", parameters}};
        if (expected) {
            AudioDecodeSpecification spec; spec.sampleRate = rate;
            std::vector<DecodedAudio> decodedOutputs;
            if (refinementRate>=8000 && refinementRate<=96000 && retainRefinement) {
                AudioDecodeSpecification refinement; refinement.sampleRate=refinementRate;
                decodedOutputs=decodeAudioVariants(runner,media,episode,scope.type,allowed,job,root,{spec,refinement},cancel,{},300.,{},ffmpeg);
            } else decodedOutputs.push_back(decodeAudio(runner,media,episode,scope.type,allowed,job,root,cancel,spec,{},300.,{},ffmpeg));
            const auto &audio=decodedOutputs.front(); decoded=true;
            if (std::abs(audio.window().durationDifference) > .05) throw DetectionError("raw_audio_incomplete_chunk", "Decoded audio does not cover its fingerprint window.");
            input["samples"] = audio.window().sampleCount;
            track = featureFromMessage(algorithm.invoke("extract", input, cancel, [&](uint32_t source, uint32_t requestedRate, qint64 first, uint32_t count) {
                if (source || requestedRate != uint32_t(rate) || first > audio.window().sampleCount || count > audio.window().sampleCount - first)
                    throw DetectionError("algorithm_invalid_audio_read", "PCM read exceeds its scoped audio window.");
                return audio.window().readSamples(first, qsizetype(count));
            }));
            if (decodedOutputs.size()>1) retainRefinement(std::move(decodedOutputs[1]));
        } else track = featureFromMessage(algorithm.invoke("extract", input, cancel));
        validate(*track);
        if (cache) try { const auto write = cache->write(request, track); if (sink) sink(write.operation); }
        catch (const Cancelled &) { throw; } catch (...) { if (sink) sink(cacheOperation(request, "error", 0, 0, "cache_write_error")); }
    }
    if (sink) sink({{"status", "coverage"}, {"episode", episode}, {"segment_type", scope.type == SegmentType::Intro ? "intro" : "credits"}, {"start", allowed.start}, {"end", allowed.end},
        {"frames", qulonglong(track->rows())}, {"expected_frames", expected}, {"decoded", decoded}, {"decoded_request_end", allowed.end},
        {"analyzed_end", track->rows() ? allowed.start + (double(track->rows() - 1) * hop + context) / rate : allowed.start},
        {"sample_rate", rate}, {"hop_samples", hop}, {"context_samples", context}, {"search_start", scope.range.start}, {"search_end", scope.range.end},
        {"media_duration", media.duration}, {"audio_stream", *media.selectedAudio}});
    checkCancelled(cancel); return track;
}
} // namespace introvert::detection

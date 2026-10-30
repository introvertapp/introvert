#include "visual_credits.h"
#include <QRegularExpression>
#include <QDebug>
#include <mutex>
#import <Vision/Vision.h>
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

namespace introvert::detection {
namespace {
VNRecognizeTextRequest *textRequest(VNRequestTextRecognitionLevel level)
{
    auto request=[[[VNRecognizeTextRequest alloc] init] autorelease];
    request.revision=VNRecognizeTextRequestRevision3; request.recognitionLevel=level;
    request.usesLanguageCorrection=NO; request.automaticallyDetectsLanguage=YES; request.minimumTextHeight=.012;
    return request;
}
QString recognitionError(NSError *error)
{
    return error ? QString("%1/%2").arg(QString::fromUtf8([error.domain UTF8String])).arg(error.code) : QStringLiteral("no_error_details");
}
}
QVariantMap visualCreditsDescription()
{
    @autoreleasepool {
        @try {
        if (@available(macOS 13.0, *)) {
            const Class recognizer=NSClassFromString(@"VNRecognizeTextRequest");
            if (!recognizer || ![[recognizer supportedRevisions] containsIndex:VNRecognizeTextRequestRevision3])
                return {{"available",false},{"reason","text_revision_unavailable"}};
        } else return {{"available",false},{"reason","text_backend_unavailable"}};
        const QVariantList configuration{
            QVariantList{"os",QString::fromUtf8([NSProcessInfo.processInfo.operatingSystemVersionString UTF8String])},
            QVariantList{"request_revision",3},QVariantList{"recognition","accurate-fallback-fast-on-error-v1"},QVariantList{"language","automatic"},
            QVariantList{"language_correction",false},QVariantList{"pixels","rgb24-960x540-fit-pad-v1"}};
        return {{"available",true},{"extractor","vision-credit-text"},{"version","vision-text-r3-v3"},{"schema","1"},
            {"width",960},{"height",540},{"frames_per_second",1.},{"refinement_frames_per_second",4.},{"columns",visualFeatureColumns},
            {"edge_radius_seconds",5.},{"configuration",configuration}};
        } @catch (NSException *) { return {{"available",false},{"reason","text_backend_unavailable"}}; }
    }
}
QVariantList observeCreditFrame(const QByteArray &rgb, int width, int height, const Cancellation &cancel)
{
    checkCancelled(cancel);
    if (!visualCreditsDescription().value("available").toBool()) throw DetectionError("visual_unavailable","Optional text recognition is unavailable.");
    if (width < 64 || height < 64 || width > 1920 || height > 1080 || rgb.size()!=qint64(width)*height*3)
        throw DetectionError("visual_invalid_frame","Invalid scoped RGB frame.");
    @autoreleasepool {
        @try {
        CGColorSpaceRef color=CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef provider=CGDataProviderCreateWithData(nullptr,rgb.constData(),size_t(rgb.size()),nullptr);
        CGImageRef image=CGImageCreate(size_t(width),size_t(height),8,24,size_t(width)*3,color,kCGImageAlphaNone,provider,nullptr,false,kCGRenderingIntentDefault);
        CGColorSpaceRelease(color); CGDataProviderRelease(provider);
        if (!image) throw DetectionError("visual_image_failed","Cannot construct a scoped video image.");
        VNRecognizeTextRequest *request=textRequest(VNRequestTextRecognitionLevelAccurate);
        VNImageRequestHandler *handler=nil;
        @try { handler=[[[VNImageRequestHandler alloc] initWithCGImage:image options:@{}] autorelease]; }
        @finally { CGImageRelease(image); }
        NSError *error=nil;
        if (![handler performRequests:@[request] error:&error]) {
            checkCancelled(cancel);
            const auto accurateError=recognitionError(error);
            // Some OS/runtime combinations advertise the accurate model but fail
            // to compile it. Retry once with Vision's independent character path;
            // empty successful recognition is not a failure and is never retried.
            request=textRequest(VNRequestTextRecognitionLevelFast); error=nil;
            if (![handler performRequests:@[request] error:&error]) {
                checkCancelled(cancel);
                throw DetectionError("visual_text_failed",QString("Text recognition did not complete (accurate: %1; fast: %2).")
                    .arg(accurateError,recognitionError(error)));
            }
            static std::once_flag recoveryNotice;
            std::call_once(recoveryNotice,[&] {qInfo().noquote()<<"Vision text recognition recovered with fast recognition after accurate failed ("+accurateError+").";});
        }
        static const QRegularExpression roles(QStringLiteral("\\b(directors?|producers?|production|written|writers?|cast|music|editors?|editing|sound|camera|photography|costumes?|makeup|executive|assistants?|starring|screenplay|created|design)\\b"),QRegularExpression::CaseInsensitiveOption);
        QList<VisualTextLine> lines;
        for (VNRecognizedTextObservation *observation in request.results) {
            VNRecognizedText *text=[[observation topCandidates:1] firstObject]; if (!text || text.confidence < .3) continue;
            const auto value=QString::fromUtf8([text.string UTF8String]).trimmed(); const auto box=observation.boundingBox;
            if (value.size()<3 || box.size.height<.012 || box.size.width<.018) continue;
            int letters=0,upper=0; for (const auto ch:value) { letters+=ch.isLetter(); upper+=ch.isUpper(); }
            const bool name=letters>=4 && upper>=3 && double(upper)/letters>=.45;
            lines.append({box.origin.x,1.-box.origin.y-box.size.height,box.size.width,box.size.height,text.confidence,roles.match(value).hasMatch(),name});
        }
        checkCancelled(cancel);
        qint64 dark=0; for (qsizetype i=0;i<rgb.size();i+=3) if (uchar(rgb[i])+uchar(rgb[i+1])+uchar(rgb[i+2])<96) ++dark;
        return visualFrameFeatures(lines,double(dark)/(width*height));
        } @catch (NSException *) { throw DetectionError("visual_backend_failed","Optional text recognition could not run."); }
    }
}
} // namespace introvert::detection

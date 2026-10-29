#include "feature_cache.h"
#include "candidates.h"
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QtEndian>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>

namespace introvert::detection {
namespace {
QByteArray floating(double value)
{
    if (!std::isfinite(value)) throw std::invalid_argument("non-finite detector JSON number");
    if (value == 0.) return std::signbit(value) ? "-0.0" : "0.0";
    char buffer[64]; const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (converted.ec != std::errc()) throw std::invalid_argument("invalid detector JSON number");
    QByteArray text(buffer, converted.ptr - buffer), sign;
    if (text.startsWith('-')) { sign = "-"; text.remove(0, 1); }
    int exponent = 0; const auto e = text.indexOf('e'); if (e >= 0) { exponent = text.mid(e + 1).toInt(); text.truncate(e); }
    const auto dot = text.indexOf('.'); int position = int(dot < 0 ? text.size() : dot) + exponent; if (dot >= 0) text.remove(dot, 1);
    while (text.size() > 1 && text.startsWith('0')) { text.remove(0, 1); --position; }
    while (text.size() > 1 && text.endsWith('0')) text.chop(1);
    const int power = position - 1;
    if (power < -4 || power >= 16) {
        QByteArray result = sign + text.left(1); if (text.size() > 1) result += '.' + text.mid(1);
        return result + 'e' + (power < 0 ? '-' : '+') + QByteArray::number(std::abs(power)).rightJustified(2, '0');
    }
    if (position <= 0) return sign + "0." + QByteArray(-position, '0') + text;
    if (position >= text.size()) return sign + text + QByteArray(position - text.size(), '0') + ".0";
    return sign + text.left(position) + '.' + text.mid(position);
}
QByteArray npyHeader(const QByteArray &dtype, const std::vector<size_t> &shape)
{
    QByteArray tuple("("); for (size_t i = 0; i < shape.size(); ++i) { if (i) tuple += ", "; tuple += QByteArray::number(qulonglong(shape[i])); }
    if (shape.size() == 1) tuple += ','; tuple += ')';
    QByteArray header = "{'descr': '" + dtype + "', 'fortran_order': False, 'shape': " + tuple + ", }";
    header += QByteArray(std::max(0, 21 - int(QByteArray::number(qulonglong(shape[0])).size())), ' ');
    header += QByteArray(64 - (10 + header.size() + 1) % 64, ' '); header += '\n';
    if (header.size() > 65535) throw std::length_error("feature array header exceeds supported format");
    QByteArray result("\x93NUMPY\x01\x00", 8); char bytes[2]; qToLittleEndian<quint16>(quint16(header.size()), bytes); result.append(bytes, 2); return result + header;
}
struct Array { QByteArray dtype, bytes; std::vector<size_t> shape; };
Array npy(const QByteArray &encoded)
{
    if (encoded.size() < 10 || encoded.first(6) != QByteArray("\x93NUMPY", 6) || encoded[7] != 0) throw std::invalid_argument("invalid feature array header");
    qsizetype offset; quint32 length;
    if (encoded[6] == 1) { offset = 10; length = qFromLittleEndian<quint16>(encoded.constData() + 8); }
    else if (encoded[6] == 2 || encoded[6] == 3) { if (encoded.size() < 12) throw std::invalid_argument("invalid feature array header"); offset = 12; length = qFromLittleEndian<quint32>(encoded.constData() + 8); }
    else throw std::invalid_argument("unsupported feature array version");
    if (length > 10000 || length > quint32(encoded.size() - offset)) throw std::invalid_argument("invalid feature array header length");
    const auto header = QString::fromLatin1(encoded.mid(offset, length));
    const auto fields = QRegularExpression("\\A\\s*\\{\\s*'descr'\\s*:\\s*'([^']+)'\\s*,\\s*'fortran_order'\\s*:\\s*(False|True)\\s*,\\s*'shape'\\s*:\\s*\\(([^)]*)\\)\\s*,?\\s*\\}\\s*\\z").match(header);
    if (!fields.hasMatch() || fields.captured(2) != "False") throw std::invalid_argument("unsupported feature array layout");
    if (!QRegularExpression("\\A\\s*(?:0|[1-9][0-9]*)\\s*,(?:\\s*(?:0|[1-9][0-9]*)\\s*,?)?\\s*\\z").match(fields.captured(3)).hasMatch())
        throw std::invalid_argument("invalid feature array shape");
    Array result{fields.captured(1).toLatin1(), encoded.mid(offset + length), {}};
    for (const auto &part : fields.captured(3).split(',')) if (!part.trimmed().isEmpty()) {
        bool ok = false; const auto count = part.trimmed().toULongLong(&ok); if (!ok || count > qulonglong(std::numeric_limits<qsizetype>::max())) throw std::invalid_argument("invalid feature array shape"); result.shape.push_back(size_t(count));
    }
    if (result.shape.empty() || result.shape.size() > 2) throw std::invalid_argument("unsupported feature array dimensions"); return result;
}
}
QByteArray detectorJson(const QVariant &value, bool compact)
{
    const QByteArray separator = compact ? "," : ", ", colon = compact ? ":" : ": ";
    if (!value.isValid() || value.isNull()) return "null";
    switch (value.typeId()) {
    case QMetaType::QString: return canonicalIdentityJson(value.toString());
    case QMetaType::Bool: return value.toBool() ? "true" : "false";
    case QMetaType::Int: case QMetaType::LongLong: return QByteArray::number(value.toLongLong());
    case QMetaType::UInt: case QMetaType::ULongLong: return QByteArray::number(value.toULongLong());
    case QMetaType::Float: case QMetaType::Double: return floating(value.toDouble());
    case QMetaType::QVariantList: {
        QByteArray result("["); bool first = true; for (const auto &v : value.toList()) { if (!first) result += separator; first = false; result += detectorJson(v, compact); } return result + ']';
    }
    case QMetaType::QVariantMap: {
        const auto map = value.toMap(); auto keys = map.keys(); std::sort(keys.begin(), keys.end(), unicodeLess); QByteArray result("{"); bool first = true;
        for (const auto &key : keys) { if (!first) result += separator; first = false; result += canonicalIdentityJson(key) + colon + detectorJson(map[key], compact); } return result + '}';
    }
    default: throw std::invalid_argument("unsupported detector JSON value");
    }
}
QString FeatureCacheRequest::storageKey() const { return key + ':' + segmentName(type); }
QByteArray FeatureCacheRequest::descriptor() const
{
    return detectorJson(QVariantMap{{"cache_key", key}, {"media_path_digest", pathDigest}, {"media_file_size", fileSize}, {"media_modified_time_ns", modifiedTimeNs},
        {"segment_type", segmentName(type)}, {"window", QVariantMap{{"start_seconds", window.start}, {"end_seconds", window.end}}}, {"stream_index", streamIndex ? QVariant(*streamIndex) : QVariant{}},
        {"extractor_id", extractorId}, {"extractor_version", extractorVersion}, {"feature_schema_version", schemaVersion}, {"extractor_configuration", configuration}, {"format_version", "2"}}, false);
}
FeatureCacheRequest buildFeatureCacheRequest(const MediaInfo &media, const QString &episode, SegmentType type, const TimeRange &window, const QString &extractorId,
    const QString &extractorVersion, const QString &schemaVersion, QVariantList configuration, const QString &streamKind)
{
    std::stable_sort(configuration.begin(), configuration.end(), [](const auto &a, const auto &b) { return unicodeLess(a.toList()[0].toString(), b.toList()[0].toString()); });
    std::optional<qint64> stream;
    if (streamKind == "audio") stream = media.selectedAudio; else if (streamKind == "video") stream = media.selectedVideo; else if (streamKind != "none") throw std::invalid_argument("unsupported extractor stream kind");
    const auto digest = QString::fromLatin1(QCryptographicHash::hash(media.identity.normalizedPath.toUtf8(), QCryptographicHash::Sha256).toHex());
    const auto payload = detectorJson(QVariantMap{{"cache_format_version", "2"}, {"media_identity", QVariantMap{{"path_digest", digest}, {"file_size", media.identity.fileSize}, {"modified_time_ns", media.identity.modifiedTimeNs}}},
        {"window", QVariantList{window.start, window.end}}, {"stream_index", stream ? QVariant(*stream) : QVariant{}}, {"extractor_id", extractorId}, {"extractor_version", extractorVersion},
        {"feature_schema_version", schemaVersion}, {"extractor_configuration", configuration}});
    return {QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex()), digest, media.identity.fileSize, media.identity.modifiedTimeNs,
        episode, type, window, stream, extractorId, extractorVersion, schemaVersion, configuration};
}
QVariantMap cacheOperation(const FeatureCacheRequest &request, const QString &status, qint64 bytes, double seconds, const QVariant &reason)
{
    return {{"episode_id", request.episode}, {"segment_type", segmentName(request.type)}, {"extractor_id", request.extractorId}, {"cache_key", request.key},
        {"status", status}, {"bytes_processed", bytes}, {"runtime_seconds", seconds}, {"regeneration_reason", reason}};
}
EncodedFeatures encodeFeatures(const FeatureTrack &track)
{
    EncodedFeatures result{npyHeader("<f8", {track.rows()}), npyHeader("<f4", {track.rows(), track.columns})};
    for (const auto value : track.timestamps) { quint64 bits; std::memcpy(&bits, &value, 8); char bytes[8]; qToLittleEndian(bits, bytes); result.timestamps.append(bytes, 8); }
    for (const auto value : track.values) { quint32 bits; std::memcpy(&bits, &value, 4); char bytes[4]; qToLittleEndian(bits, bytes); result.values.append(bytes, 4); }
    return result;
}
std::shared_ptr<const FeatureTrack> decodeFeatures(const FeatureCacheRequest &request, const EncodedFeatures &encoded)
{
    const auto times = npy(encoded.timestamps), values = npy(encoded.values);
    if (times.dtype != "<f8" || values.dtype != "<f4" || times.shape.size() != 1 || values.shape.size() != 2 || times.shape[0] != values.shape[0]
        || !values.shape[1] || times.shape[0] > size_t(std::numeric_limits<qsizetype>::max() / 8) || values.shape[0] > size_t(std::numeric_limits<qsizetype>::max() / 4) / values.shape[1]
        || times.bytes.size() != qsizetype(times.shape[0] * 8) || values.bytes.size() != qsizetype(values.shape[0] * values.shape[1] * 4)) throw std::invalid_argument("invalid production feature arrays");
    std::vector<double> timestamps(times.shape[0]); std::vector<float> descriptors(values.shape[0] * values.shape[1]);
    for (size_t i = 0; i < timestamps.size(); ++i) { const auto bits = qFromLittleEndian<quint64>(times.bytes.constData() + i * 8); std::memcpy(&timestamps[i], &bits, 8); }
    for (size_t i = 0; i < descriptors.size(); ++i) { const auto bits = qFromLittleEndian<quint32>(values.bytes.constData() + i * 4); std::memcpy(&descriptors[i], &bits, 4); }
    return std::make_shared<FeatureTrack>(request.episode, request.type, request.window, "cache:" + request.key, request.extractorId, request.extractorVersion, request.schemaVersion, std::move(timestamps), std::move(descriptors), values.shape[1]);
}
} // namespace introvert::detection

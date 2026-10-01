#include "model/video_representation_codec.h"

QJsonObject representationToJson(const VideoRepresentation &r) {
    QJsonObject m{{"file_path", r.metadata.filePath},
                  {"file_name", r.metadata.fileName},
                  {"format", r.metadata.format},
                  {"duration_ms", qint64(r.metadata.durationMs)},
                  {"bit_rate", qint64(r.metadata.bitRate)},
                  {"fps", r.metadata.frameRate},
                  {"width", r.metadata.width},
                  {"height", r.metadata.height},
                  {"has_audio", r.metadata.hasAudio}};
    QJsonArray shots, speech;
    for (const auto &s : r.scenes) {
        QJsonArray frames;
        for (const auto &f : s.representativeFrames)
            frames.append(QJsonObject{
                {"requested_ms", qint64(f.requestedMs)}, {"pts_ms", qint64(f.ptsMs)}, {"path", f.imagePath}});
        shots.append(QJsonObject{{"id", s.id},
                                 {"start_ms", qint64(s.startMs)},
                                 {"end_ms", qint64(s.endMs)},
                                 {"keyframe_ms", qint64(s.keyframeMs)},
                                 {"keyframe_path", s.keyframePath},
                                 {"frames", frames}});
    }
    for (const auto &s : r.speechSegments)
        speech.append(
            QJsonObject{{"start_ms", qint64(s.startMs)}, {"end_ms", qint64(s.endMs)}, {"text", s.text}});
    return {{"video_id", r.videoId}, {"metadata", m}, {"shots", shots}, {"speech", speech}};
}
VideoRepresentation representationFromJson(const QJsonObject &j) {
    VideoRepresentation r;
    r.videoId = j["video_id"].toString();
    auto m = j["metadata"].toObject();
    r.metadata.filePath = m["file_path"].toString();
    r.metadata.fileName = m["file_name"].toString();
    r.metadata.format = m["format"].toString();
    r.metadata.durationMs = m["duration_ms"].toVariant().toLongLong();
    r.metadata.bitRate = m["bit_rate"].toVariant().toLongLong();
    r.metadata.frameRate = m["fps"].toDouble();
    r.metadata.width = m["width"].toInt();
    r.metadata.height = m["height"].toInt();
    r.metadata.hasAudio = m["has_audio"].toBool();
    for (auto v : j["shots"].toArray()) {
        auto o = v.toObject();
        Scene s;
        s.id = o["id"].toInt();
        s.startMs = o["start_ms"].toVariant().toLongLong();
        s.endMs = o["end_ms"].toVariant().toLongLong();
        s.keyframeMs = o["keyframe_ms"].toVariant().toLongLong();
        s.keyframePath = o["keyframe_path"].toString();
        for (auto f : o["frames"].toArray()) {
            auto x = f.toObject();
            SceneFrame frame;
            frame.requestedMs = x["requested_ms"].toVariant().toLongLong();
            frame.ptsMs = x["pts_ms"].toVariant().toLongLong();
            frame.imagePath = x["path"].toString();
            s.representativeFrames << frame;
        }
        r.scenes << s;
    }
    for (auto v : j["speech"].toArray()) {
        auto o = v.toObject();
        SpeechSegment s;
        s.startMs = o["start_ms"].toVariant().toLongLong();
        s.endMs = o["end_ms"].toVariant().toLongLong();
        s.text = o["text"].toString();
        r.speechSegments << s;
    }
    return r;
}

#include "service/agent/frame_extractor.h"
#include "service/agent/media_probe.h"
#include "util/audio_decoder.h"
#include <QDataStream>
#include <QTemporaryDir>
#include <QtTest>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

class MediaTests : public QObject {
    Q_OBJECT
    static bool writeVideo(const QString &path) {
        AVFormatContext *output = nullptr;
        if (avformat_alloc_output_context2(&output, nullptr, "avi", path.toUtf8().constData()) < 0 || !output)
            return false;
        AVStream *stream = avformat_new_stream(output, nullptr);
        stream->time_base = {1, 10};
        stream->avg_frame_rate = {10, 1};
        auto *parameters = stream->codecpar;
        parameters->codec_type = AVMEDIA_TYPE_VIDEO;
        parameters->codec_id = AV_CODEC_ID_RAWVIDEO;
        parameters->format = AV_PIX_FMT_BGR24;
        parameters->width = 32;
        parameters->height = 32;
        parameters->bits_per_coded_sample = 24;
        bool ok = avio_open(&output->pb, path.toUtf8().constData(), AVIO_FLAG_WRITE) >= 0 &&
                  avformat_write_header(output, nullptr) >= 0;
        for (int i = 0; ok && i < 200; ++i) {
            AVPacket *packet = av_packet_alloc();
            av_new_packet(packet, 32 * 32 * 3);
            for (int pixel = 0; pixel < 32 * 32; ++pixel) {
                packet->data[pixel * 3] = i < 100 ? 255 : 0;
                packet->data[pixel * 3 + 1] = 0;
                packet->data[pixel * 3 + 2] = i < 100 ? 0 : 255;
            }
            packet->pts = packet->dts = i;
            packet->duration = 1;
            packet->stream_index = stream->index;
            packet->flags = AV_PKT_FLAG_KEY;
            ok = av_interleaved_write_frame(output, packet) >= 0;
            av_packet_free(&packet);
        }
        if (ok)
            ok = av_write_trailer(output) >= 0;
        if (output->pb)
            avio_closep(&output->pb);
        avformat_free_context(output);
        return ok;
    }
  private slots:
    void seekAndActualPts() {
        QTemporaryDir dir;
        const auto path = dir.filePath("fixture.avi");
        QVERIFY(writeVideo(path));
        QString error;
        auto info = MediaProbe::inspect(path, nullptr, &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(info.durationMs, 20000);
        QVERIFY(!info.hasAudio);
        auto frames = FrameExtractor::extract(path, {15000, 19500}, {}, nullptr, &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(frames.size(), 2);
        for (const auto &frame : frames) {
            QVERIFY(frame.ptsMs >= frame.requestedMs);
            QVERIFY(frame.ptsMs - frame.requestedMs < 100);
            QVERIFY(frame.image.pixelColor(0, 0).red() > 240);
        }
        std::atomic_bool cancelled{true};
        QVERIFY(FrameExtractor::extract(path, {15000}, {}, &cancelled).isEmpty());
    }
    void audioRangeAndTail() {
        QTemporaryDir dir;
        const auto path = dir.filePath("fixture.wav");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QDataStream out(&file);
        out.setByteOrder(QDataStream::LittleEndian);
        const quint32 samples = 48000 * 6;
        out.writeRawData("RIFF", 4);
        out << quint32(36 + samples * 2);
        out.writeRawData("WAVEfmt ", 8);
        out << quint32(16) << quint16(1) << quint16(1) << quint32(48000) << quint32(96000) << quint16(2)
            << quint16(16);
        out.writeRawData("data", 4);
        out << quint32(samples * 2);
        for (quint32 i = 0; i < samples; ++i)
            out << qint16(8192);
        file.close();
        AudioDecoder decoder;
        const auto full = decoder.decodeToFloat32(path);
        QVERIFY(qAbs(qint64(full.size()) - 96000) <= 1);
        const auto range = decoder.decodeToFloat32(path, 1234, 5678);
        QVERIFY(qAbs(qint64(range.size()) - 71104) <= 48);
        QVERIFY(!range.empty());
        QVERIFY(qAbs(range[range.size() / 2] - 0.25f) < 0.01f);
    }
};
QTEST_GUILESS_MAIN(MediaTests)
#include "video_media_tests.moc"

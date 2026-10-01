#include "service/agent/media_probe.h"
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QCryptographicHash>
extern "C" {
#include <libavformat/avformat.h>
}
namespace { int interrupt(void* ptr) { return ptr && static_cast<const std::atomic_bool*>(ptr)->load(); } }
VideoInfo MediaProbe::inspect(const QString& path,const std::atomic_bool* cancelled,QString* error) {
    VideoInfo info;info.filePath=path;info.fileName=QFileInfo(path).fileName();
    AVFormatContext* context=avformat_alloc_context();if(!context)return info;
    context->interrupt_callback={interrupt,const_cast<std::atomic_bool*>(cancelled)};
    const auto bytes=path.toUtf8();
    if(avformat_open_input(&context,bytes.constData(),nullptr,nullptr)<0 || avformat_find_stream_info(context,nullptr)<0) {
        if(error)*error=QStringLiteral("无法读取视频元信息");if(context)avformat_close_input(&context);return info;
    }
    info.durationMs=context->duration>0?context->duration/(AV_TIME_BASE/1000):0;info.bitRate=context->bit_rate;
    info.format=QString::fromUtf8(context->iformat->name);
    for(unsigned i=0;i<context->nb_streams;++i) {auto* s=context->streams[i];auto* p=s->codecpar;
        if(p->codec_type==AVMEDIA_TYPE_AUDIO)info.hasAudio=true;
        if(p->codec_type==AVMEDIA_TYPE_VIDEO && info.width==0) {info.width=p->width;info.height=p->height;info.frameRate=av_q2d(s->avg_frame_rate);if(info.durationMs<=0 && s->duration>0)info.durationMs=int64_t(s->duration*av_q2d(s->time_base)*1000);}
    }
    avformat_close_input(&context);if(info.durationMs<=0 && error)*error=QStringLiteral("视频时长无效");return info;
}
QString MediaProbe::fingerprint(const QString& path) {
    QFile f(path);if(!f.open(QIODevice::ReadOnly))return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);QFileInfo fi(f);
    hash.addData(QByteArray::number(fi.size()));hash.addData(QByteArray::number(fi.lastModified().toMSecsSinceEpoch()));
    // Distributed samples detect changes beyond the legacy first-MB video ID.
    for(int i=0;i<5;++i) {f.seek(qMax<qint64>(0,(fi.size()-65536)*i/4));hash.addData(f.read(65536));}
    return QString::fromLatin1(hash.result().toHex());
}

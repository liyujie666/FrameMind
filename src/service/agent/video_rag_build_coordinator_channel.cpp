#include "service/agent/one_shot_vlm_channel.h"
#include "service/agent/video_rag_build_coordinator.h"
#include <QPointer>
VideoRAGBuildCoordinator::VideoRAGBuildCoordinator(VideoRAGBuildBackend *backend, VideoRAGStore *store,
                                                   OneShotVlmChannel *channel, QObject *parent)
    : VideoRAGBuildCoordinator(backend, store, parent) {
    QPointer<OneShotVlmChannel> guard(channel);
    m_modelSignature = [guard] { return guard ? guard->modelSignature() : QStringLiteral("unavailable"); };
    m_cancelModel = [guard](const QString &key) {
        if (guard)
            guard->cancelBackground(key);
    };
    m_modelRequest = [guard](const VideoBuildContext &context, const QString &system, const QString &text,
                             const QList<QImage> &images, std::function<void(QString)> done) {
        if (guard)
            guard->enqueue(system, text, images, OneShotVlmChannel::Priority::Background,
                           context.cancellationKey(), done);
        else
            done({});
    };
}

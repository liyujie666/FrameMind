#include "service/agent/unit_analysis_worker_pool.h"
#include "service/agent/one_shot_vlm_channel.h"
#include "service/agentservice.h"
#include "infrastructure/networkclient.h"
#include <QPointer>
#include <QThread>

struct UnitAnalysisWorkerPool::Worker {
    std::unique_ptr<NetworkClient> network;
    std::unique_ptr<AgentService> agent;
    std::unique_ptr<OneShotVlmChannel> channel;
    QString unitId, buildKey, requestId;
};

UnitAnalysisWorkerPool::UnitAnalysisWorkerPool(SettingsService* settings, LLMProviderService* providers,
                                             int capacity, QObject* parent, int timeoutMs)
    : QObject(parent) {
    for (int i = 0; i < qBound(1, capacity, 3); ++i) {
        auto w = std::make_unique<Worker>();
        w->network = std::make_unique<NetworkClient>();
        w->agent = std::make_unique<AgentService>(w->network.get(), settings, providers);
        w->channel = std::make_unique<OneShotVlmChannel>(w->agent.get(), nullptr, timeoutMs);
        m_workers.push_back(std::move(w));
    }
}
UnitAnalysisWorkerPool::~UnitAnalysisWorkerPool() {
    for (auto& w : m_workers)
        if (!w->requestId.isEmpty()) w->channel->cancelRequest(w->requestId);
}
int UnitAnalysisWorkerPool::capacity() const { return int(m_workers.size()); }
QString UnitAnalysisWorkerPool::modelSignature() const { return m_workers.front()->channel->modelSignature(); }
int UnitAnalysisWorkerPool::watchdogTimeoutMs(int id) const {
    return id >= 0 && id < capacity() ? m_workers[id]->channel->watchdogTimeoutMs() : 0;
}
void UnitAnalysisWorkerPool::submit(const UnitAnalysisRequest& r, const QString& system,
                                    const QString& text, const QList<QImage>& images,
                                    std::function<void(ModelReply)> done) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (r.context.isCancelled() || r.workerId < 0 || r.workerId >= capacity() || r.requestId.isEmpty()) {
        done({{}, "invalid_request: 单元请求身份或通道无效"});
        return;
    }
    auto& w = *m_workers[r.workerId];
    const auto key = r.context.cancellationKey();
    if (!w.requestId.isEmpty() || (!w.unitId.isEmpty() && (w.unitId != r.unitId || w.buildKey != key))) {
        done({{}, "worker_busy: 通道已分配给其他请求或单元"});
        return;
    }
    w.unitId = r.unitId;
    w.buildKey = key;
    w.requestId = r.requestId;
    QPointer<UnitAnalysisWorkerPool> guard(this);
    w.channel->enqueueRequest(r.requestId, system, text, images, OneShotVlmChannel::Priority::Background,
                              key, [guard, r, done = std::move(done)](ModelReply reply) {
        if (!guard) return;
        auto& w = *guard->m_workers[r.workerId];
        if (w.requestId == r.requestId) w.requestId.clear();
        done(std::move(reply));
    }, r.imageOptions, r.maxOutputTokens);
}
void UnitAnalysisWorkerPool::releaseUnit(int id, const QString& unitId) {
    if (id < 0 || id >= capacity()) return;
    auto& w = *m_workers[id];
    if (w.unitId == unitId && w.requestId.isEmpty()) { w.unitId.clear(); w.buildKey.clear(); }
}
void UnitAnalysisWorkerPool::cancelRequest(const QString& id) {
    for (auto& w : m_workers)
        if (w->requestId == id) {
            w->requestId.clear();
            w->channel->cancelRequest(id);
        }
}
void UnitAnalysisWorkerPool::cancelBuild(const QString& key) {
    for (auto& w : m_workers) {
        if (w->buildKey != key) continue;
        const auto id = w->requestId;
        w->unitId.clear(); w->buildKey.clear(); w->requestId.clear();
        if (!id.isEmpty()) w->channel->cancelRequest(id);
    }
}

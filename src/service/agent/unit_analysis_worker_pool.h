#pragma once
#include "model/model_reply.h"
#include "model/unit_analysis_request.h"
#include <QObject>
#include <QImage>
#include <functional>
#include <memory>
#include <vector>

class SettingsService;
class LLMProviderService;

// Lives on the coordinator thread. Each slot owns independent network/stream state.
class UnitAnalysisWorkerPool final : public QObject {
public:
    UnitAnalysisWorkerPool(SettingsService*, LLMProviderService*, int capacity = 3,
                           QObject* parent = nullptr, int requestTimeoutMs = 0);
    ~UnitAnalysisWorkerPool() override;
    int capacity() const;
    QString modelSignature() const;
    int watchdogTimeoutMs(int workerId) const;
    void submit(const UnitAnalysisRequest&, const QString& system, const QString& text,
                const QList<QImage>& images, std::function<void(ModelReply)> done);
    void releaseUnit(int workerId, const QString& unitId);
    void cancelRequest(const QString& requestId);
    void cancelBuild(const QString& cancellationKey);
private:
    struct Worker;
    std::vector<std::unique_ptr<Worker>> m_workers;
};

#pragma once
#include "service/rag/semantic_unit_builder.h"
#include "model/image_encoding_options.h"
#include <QImage>
#include <QRect>
#include <atomic>
#include <memory>

struct EvidenceGridConfig {
    QString version = QStringLiteral("evidence_grid_v1");
    int maxEdge = 2048, jpegQuality = 85, minCellShortEdge = 480, labelHeight = 32;
    qint64 maxEncodedBytes = 0;
    static EvidenceGridConfig fromPlan(const VideoRAGBuildPlan&);
    QString validationError() const;
};

struct GridCellMapping {
    QString label, sourceId;
    int64_t ptsMs = 0;
    int row = 0, column = 0;
    QRect cellRect, imageRect;
    QJsonObject toJson() const;
};

struct GridEvidenceImage {
    QImage image;
    QVector<GridCellMapping> cells;
    ImageEncodingOptions encoding;
    QString error;
    QSize finalEncodedSize{0, 0};
    qint64 encodedBytes = 0;
    qint64 readMs = 0, composeMs = 0, encodeMs = 0;
    QJsonObject diagnostics() const;
};

class EvidenceGridComposer {
public:
    // Header-only planning: establishes all splits/page identities before any model request.
    static QVector<UnitEvidencePage> planPages(const QVector<UnitEvidencePage>&, const EvidenceGridConfig&,
                                               const std::shared_ptr<std::atomic_bool>& cancelled);
    // Reads one source image at a time; raw evidence remains unchanged.
    static GridEvidenceImage compose(const UnitEvidencePage&, const EvidenceGridConfig&,
                                      const std::shared_ptr<std::atomic_bool>& cancelled);
};

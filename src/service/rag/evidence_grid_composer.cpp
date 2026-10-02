#include "service/rag/evidence_grid_composer.h"
#include <QBuffer>
#include <QElapsedTimer>
#include <QImageReader>
#include <QImageIOHandler>
#include <QPainter>
#include <QSet>
#include <algorithm>
#include <array>
#include <limits>
#include <QtMath>

namespace {
bool stopped(const std::shared_ptr<std::atomic_bool>& flag) { return flag && flag->load(); }
struct Layout { QSize size, cell; int columns = 1, rows = 1; bool readable = true; };
Layout layout(const QVector<UnitFrameEvidence>& frames, const EvidenceGridConfig& config) {
    Layout out;
    if (frames.isEmpty()) return out;
    out.columns = frames.size() == 1 ? 1 : 2;
    out.rows = (int(frames.size()) + out.columns - 1) / out.columns;
    // A configured byte cap also conservatively limits the uncompressed canvas.
    // Actual JPEG bytes are still checked after encoding; metadata cannot prove compression size.
    const int edge = config.maxEncodedBytes > 0
        ? qMin(config.maxEdge, int(qSqrt(double(config.maxEncodedBytes) / 4.0))) : config.maxEdge;
    const QSize available(edge / out.columns, edge / out.rows - config.labelHeight);
    if (available.width() <= 0 || available.height() <= 0) { out.readable = false; return out; }
    int width = qMin(available.width(), 320), height = 1;
    for (const auto& frame : frames) {
        if (!frame.imageSize.isValid()) { out.readable = false; continue; }
        auto fitted = frame.imageSize;
        if (fitted.width() > available.width() || fitted.height() > available.height())
            fitted.scale(available, Qt::KeepAspectRatio);
        width = qMax(width, fitted.width()); height = qMax(height, fitted.height());
        const int required = qMin(config.minCellShortEdge, qMin(frame.imageSize.width(), frame.imageSize.height()));
        if (qMin(fitted.width(), fitted.height()) < required) out.readable = false;
    }
    out.cell = QSize(width, height + config.labelHeight);
    out.size = QSize(out.cell.width() * out.columns, out.cell.height() * out.rows);
    return out;
}
QString timestamp(int64_t ms) {
    const auto seconds = qMax(int64_t(0), ms) / 1000;
    const auto minutes = seconds / 60;
    const auto prefix = minutes >= 60 ? QString("%1:").arg(minutes / 60, 2, 10, QChar('0')) : QString();
    return prefix + QString("%1:%2.%3").arg(minutes % 60, 2, 10, QChar('0'))
        .arg(seconds % 60, 2, 10, QChar('0')).arg(qMax(int64_t(0), ms) % 1000, 3, 10, QChar('0'));
}
// Deterministic bitmap labels work on background threads without a GUI/font database.
void drawLabel(QPainter& painter, const QRect& rect, const QString& text) {
    static const std::array<std::array<int, 7>, 10> digits{{
        {{14,17,19,21,25,17,14}}, {{4,12,4,4,4,4,14}}, {{14,17,1,2,4,8,31}},
        {{30,1,1,14,1,1,30}}, {{2,6,10,18,31,2,2}}, {{31,16,16,30,1,1,30}},
        {{14,16,16,30,17,17,14}}, {{31,1,2,4,8,8,8}}, {{14,17,17,14,17,17,14}},
        {{14,17,17,15,1,1,14}}
    }};
    const int scale = qMax(1, qMin((rect.height() - 8) / 7, (rect.width() - 12) / qMax(1, int(text.size()) * 6)));
    const int top = rect.top() + (rect.height() - 7 * scale) / 2;
    int left = rect.left() + 6;
    for (const auto ch : text) {
        std::array<int, 7> bits{};
        if (ch >= QChar('0') && ch <= QChar('9')) bits = digits[ch.unicode() - '0'];
        else if (ch == QChar('F')) bits = {{31,16,16,30,16,16,16}};
        else if (ch == QChar(':')) bits = {{0,4,4,0,4,4,0}};
        else if (ch == QChar('.')) bits = {{0,0,0,0,0,4,4}};
        else if (ch.unicode() == 0x00b7) bits = {{0,0,0,4,0,0,0}};
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (bits[row] & (1 << (4-col))) painter.fillRect(left + col * scale, top + row * scale,
                                                               scale, scale, Qt::black);
        left += 6 * scale;
    }
}
void normalize(UnitEvidencePage& page) {
    page.framePaths.clear(); page.framePtsMs.clear(); page.sourceIds.clear();
    for (const auto& frame : page.frames) { page.framePaths << frame.path; page.framePtsMs << frame.ptsMs; }
    for (const auto v : page.evidence) {
        const auto id = v.toObject()["source_id"].toString();
        if (!page.sourceIds.contains(id)) page.sourceIds << id;
    }
}
QString mappingError(const UnitEvidencePage& page) {
    if (page.frames.size() != page.framePaths.size() || page.frames.size() != page.framePtsMs.size())
        return QStringLiteral("invalid_grid_mapping: 原始帧结构不完整");
    QSet<QString> ids;
    int entries = 0;
    for (const auto v : page.evidence) if (v.toObject()["modality"] == "frame") ++entries;
    if (entries != page.frames.size()) return QStringLiteral("invalid_grid_mapping: 原始帧与证据数不一致");
    for (int i = 0; i < page.frames.size(); ++i) {
        const auto& frame = page.frames[i];
        if (frame.sourceId.isEmpty() || ids.contains(frame.sourceId) || frame.path != page.framePaths[i] ||
            frame.ptsMs != page.framePtsMs[i]) return QStringLiteral("invalid_grid_mapping: 来源或路径不一致");
        bool found = false;
        for (const auto v : page.evidence) {
            const auto e = v.toObject();
            if (e["modality"] == "frame" && e["source_id"].toString() == frame.sourceId &&
                e["pts_ms"].toVariant().toLongLong() == frame.ptsMs) found = true;
        }
        if (!found) return QStringLiteral("invalid_grid_mapping: 帧来源不存在 %1").arg(frame.sourceId);
        ids.insert(frame.sourceId);
    }
    return {};
}
}

EvidenceGridConfig EvidenceGridConfig::fromPlan(const VideoRAGBuildPlan& p) {
    return {p.gridVersion, p.gridMaxEdge, p.gridJpegQuality, p.gridMinCellShortEdge, p.gridLabelHeight,
            p.gridMaxEncodedBytes};
}
QString EvidenceGridConfig::validationError() const {
    if (version != "evidence_grid_v1" || maxEdge < 512 || maxEdge > 4096 || jpegQuality < 1 ||
        jpegQuality > 100 || minCellShortEdge < 64 || minCellShortEdge > 4096 || labelHeight < 24 ||
        labelHeight > 128 || maxEncodedBytes < 0 || maxEncodedBytes > 64 * 1024 * 1024)
        return QStringLiteral("invalid_grid_config: 网格版本、尺寸或编码参数无效");
    return {};
}
QJsonObject GridCellMapping::toJson() const {
    auto rectangle = [](const QRect& rect) { return QJsonObject{{"x", rect.x()}, {"y", rect.y()},
                                                {"width", rect.width()}, {"height", rect.height()}}; };
    return {{"label", label}, {"source_id", sourceId}, {"pts_ms", qint64(ptsMs)},
            {"row", row}, {"column", column}, {"cell_rect", rectangle(cellRect)},
            {"image_rect", rectangle(imageRect)}};
}
QJsonObject GridEvidenceImage::diagnostics() const {
    return {{"read_ms", readMs}, {"compose_ms", composeMs}, {"encode_ms", encodeMs},
            {"original_frames", cells.size()}, {"transmitted_images", image.isNull() ? 0 : 1},
            {"encoded_width", finalEncodedSize.width()}, {"encoded_height", finalEncodedSize.height()},
            {"encoded_bytes", encodedBytes}};
}

QVector<UnitEvidencePage> EvidenceGridComposer::planPages(const QVector<UnitEvidencePage>& input,
    const EvidenceGridConfig& config, const std::shared_ptr<std::atomic_bool>& cancelled) {
    QVector<UnitEvidencePage> out;
    for (auto page : input) {
        if (stopped(cancelled)) return {};
        page.preparationError = config.validationError();
        if (page.preparationError.isEmpty()) page.preparationError = mappingError(page);
        for (auto& frame : page.frames) {
            if (stopped(cancelled)) return {};
            QImageReader reader(frame.path); reader.setAutoTransform(true);
            frame.imageSize = reader.size();
            if (reader.transformation() & QImageIOHandler::TransformationRotate90) frame.imageSize.transpose();
            if (!frame.imageSize.isValid() && page.preparationError.isEmpty())
                page.preparationError = "missing_frames: 无法读取图片尺寸 " + frame.sourceId;
        }
        if (!page.preparationError.isEmpty() || page.frames.isEmpty()) { out << page; continue; }
        std::stable_sort(page.frames.begin(), page.frames.end(), [](const auto& a, const auto& b) {
            return a.ptsMs == b.ptsMs ? a.sourceOrdinal < b.sourceOrdinal : a.ptsMs < b.ptsMs;
        });
        QVector<UnitEvidencePage> parts;
        for (int offset = 0; offset < page.frames.size();) {
            int count = qMin(6, int(page.frames.size()) - offset);
            while (count > 1 && !layout(page.frames.mid(offset, count), config).readable) --count;
            UnitEvidencePage part; part.unitId = page.unitId;
            part.frames = page.frames.mid(offset, count);
            if (!layout(part.frames, config).readable)
                part.preparationError = "grid_resolution_insufficient: 单帧无法满足清晰度配置 " + part.frames[0].sourceId;
            parts << part; offset += count;
        }
        for (const auto v : page.evidence) {
            const auto entry = v.toObject();
            int target = 0;
            if (entry["modality"] == "frame") {
                for (int i = 0; i < parts.size(); ++i)
                    for (const auto& frame : parts[i].frames)
                        if (frame.sourceId == entry["source_id"].toString()) target = i;
            } else {
                const auto start = entry["start_ms"].toVariant().toLongLong();
                const auto end = entry["end_ms"].toVariant().toLongLong();
                const auto middle = start + (end - start) / 2;
                qint64 distance = std::numeric_limits<qint64>::max();
                for (int i = 0; i < parts.size(); ++i) {
                    const auto a = parts[i].frames.first().ptsMs, b = parts[i].frames.last().ptsMs;
                    const auto candidate = qAbs(middle - (a + (b - a) / 2));
                    if (candidate < distance) { distance = candidate; target = i; }
                }
            }
            parts[target].evidence.append(entry); // each original entry/offset belongs to exactly one subpage
        }
        for (auto& part : parts) { normalize(part); out << part; }
    }
    for (int i = 0; i < out.size(); ++i) out[i].pageId = out[i].unitId + ":page:" + QString::number(i);
    return out;
}

GridEvidenceImage EvidenceGridComposer::compose(const UnitEvidencePage& page, const EvidenceGridConfig& config,
    const std::shared_ptr<std::atomic_bool>& cancelled) {
    GridEvidenceImage out;
    out.encoding.maxEdge = config.maxEdge; out.encoding.jpegQuality = config.jpegQuality;
    out.encoding.requirePrepared = true;
    out.error = config.validationError();
    if (out.error.isEmpty()) out.error = page.preparationError;
    if (out.error.isEmpty()) out.error = mappingError(page);
    if (!out.error.isEmpty() || page.frames.isEmpty()) return out;
    if (stopped(cancelled)) { out.error = "cancelled: 网格准备已取消"; return out; }
    const auto arrangement = layout(page.frames, config);
    if (page.frames.size() > 6 || !arrangement.readable) { out.error = "invalid_grid_layout: 需要预先拆页"; return out; }
    QElapsedTimer clock; clock.start();
    out.image = QImage(arrangement.size, QImage::Format_RGB32);
    if (out.image.isNull()) { out.error = "grid_allocation_failed: 无法创建网格图片"; return out; }
    out.image.fill(Qt::white);
    QPainter painter(&out.image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    for (int i = 0; i < arrangement.columns * arrangement.rows; ++i) {
        const QRect cell((i % arrangement.columns) * arrangement.cell.width(),
                         (i / arrangement.columns) * arrangement.cell.height(),
                         arrangement.cell.width(), arrangement.cell.height());
        if (i >= page.frames.size()) {
            painter.fillRect(cell, QColor(235,235,235));
            painter.setPen(QPen(Qt::gray, 2)); painter.drawLine(cell.topLeft(), cell.bottomRight());
            painter.drawLine(cell.topRight(), cell.bottomLeft()); continue;
        }
        if (stopped(cancelled)) { out.error = "cancelled: 网格准备已取消"; break; }
        const auto& frame = page.frames[i];
        const auto before = clock.elapsed();
        if (qint64(frame.imageSize.width()) * frame.imageSize.height() > 64 * 1024 * 1024) {
            out.error = "frame_too_large: 原始图片超过解码预算 " + frame.sourceId; break;
        }
        QImageReader reader(frame.path); reader.setAutoTransform(true);
        const auto image = reader.read();
        out.readMs += clock.elapsed() - before;
        if (image.isNull() || image.size() != frame.imageSize) {
            out.error = "missing_or_changed_frame: 原始帧不可读或尺寸已变化 " + frame.sourceId; break;
        }
        const QRect area(cell.left(), cell.top() + config.labelHeight, cell.width(), cell.height() - config.labelHeight);
        auto scaled = image.size();
        if (scaled.width() > area.width() || scaled.height() > area.height()) scaled.scale(area.size(), Qt::KeepAspectRatio);
        const QRect target(area.left() + (area.width() - scaled.width()) / 2,
                           area.top() + (area.height() - scaled.height()) / 2, scaled.width(), scaled.height());
        painter.drawImage(target, image);
        const QString label = QString("F%1").arg(i + 1);
        drawLabel(painter, QRect(cell.left(), cell.top(), cell.width(), config.labelHeight),
                  label + QStringLiteral(" · ") + timestamp(frame.ptsMs));
        out.cells << GridCellMapping{label, frame.sourceId, frame.ptsMs, i / arrangement.columns,
                                     i % arrangement.columns, cell, target};
    }
    painter.end();
    out.composeMs = clock.elapsed() - out.readMs;
    if (!out.error.isEmpty()) { out.image = {}; out.cells.clear(); return out; }
    const auto before = clock.elapsed();
    QByteArray jpeg; QBuffer buffer(&jpeg); buffer.open(QIODevice::WriteOnly);
    out.finalEncodedSize = out.image.size();
    if (!out.image.save(&buffer, "JPEG", config.jpegQuality) || jpeg.isEmpty())
        out.error = "grid_encoding_failed: JPEG编码失败";
    else if (config.maxEncodedBytes > 0 && jpeg.size() > config.maxEncodedBytes)
        out.error = "grid_byte_limit: 编码图片超过配置的提供商字节限制";
    out.encodeMs = clock.elapsed() - before;
    out.encodedBytes = jpeg.size();
    if (!out.error.isEmpty()) { out.image = {}; return out; }
    out.encoding.preparedImages << EncodedRequestImage{jpeg, out.image.size()};
    return out;
}

#ifndef FRAMEMIND_VIDEOCARDDELEGATE_H
#define FRAMEMIND_VIDEOCARDDELEGATE_H

#include <QStyledItemDelegate>

/**
 * 视频缩略图卡片 delegate。
 *   ┌──────────────────┐
 *   │                  │  ← 16:9 缩略图区（若无缓存则渐变占位 + 大播放按钮）
 *   │       ▶          │
 *   │                  │
 *   ├──────────────────┤
 *   │ Filename.mp4     │  ← 单行截断的文件名
 *   │ 128 MB           │  ← 文件大小
 *   └──────────────────┘
 * 卡片圆角 12px，hover 时描边高亮，选中时描边 Primary。
 */
class VideoCardDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    static constexpr int kCardWidth  = 220;
    static constexpr int kCardHeight = 176;
    static constexpr int kThumbHeight = 124;  // 220 * 9/16 ≈ 123.75
    static constexpr int kRadius = 12;

    explicit VideoCardDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

private:
    static QString humanFileSize(qint64 bytes);
    static QColor placeholderTint(const QString& seed);
};

#endif // FRAMEMIND_VIDEOCARDDELEGATE_H

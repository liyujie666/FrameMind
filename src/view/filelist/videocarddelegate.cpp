#include "view/filelist/videocarddelegate.h"

#include "viewmodel/filelistviewmodel.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QPixmap>
#include <QFontMetrics>

VideoCardDelegate::VideoCardDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{}

QSize VideoCardDelegate::sizeHint(const QStyleOptionViewItem& /*option*/,
                                  const QModelIndex& /*index*/) const
{
    return QSize(kCardWidth, kCardHeight);
}

QString VideoCardDelegate::humanFileSize(qint64 bytes)
{
    static const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return QString::number(v, 'f', u == 0 ? 0 : 1) + QStringLiteral(" ") + QString::fromLatin1(units[u]);
}

QColor VideoCardDelegate::placeholderTint(const QString& seed)
{
    // 由文件名 hash 挑选一组柔和的暗色调，保证不同文件占位色不同
    static const QColor palette[] = {
        QColor("#3949AB"), QColor("#00838F"), QColor("#6A1B9A"),
        QColor("#00695C"), QColor("#4527A0"), QColor("#2E7D32"),
        QColor("#AD1457"), QColor("#EF6C00"), QColor("#37474F"),
    };
    const int n = int(sizeof(palette) / sizeof(palette[0]));
    uint h = qHash(seed);
    return palette[h % n];
}

void VideoCardDelegate::paint(QPainter* painter,
                              const QStyleOptionViewItem& option,
                              const QModelIndex& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

    // ---- 卡片外框（预留 8px 内边距用于 hover / selected 描边）----
    const int pad = 8;
    QRect card = option.rect.adjusted(pad, pad, -pad, -pad);

    // 卡片背景
    QPainterPath cardPath;
    cardPath.addRoundedRect(card, kRadius, kRadius);
    painter->fillPath(cardPath, QColor("#1E1E2E"));   // Dark.Surface

    // ---- 缩略图区 ----
    QRect thumbRect = card;
    thumbRect.setHeight(kThumbHeight);

    QPainterPath thumbPath;
    // 只对上圆角
    thumbPath.moveTo(thumbRect.left(), thumbRect.bottom());
    thumbPath.lineTo(thumbRect.left(), thumbRect.top() + kRadius);
    thumbPath.arcTo(QRectF(thumbRect.left(), thumbRect.top(),
                           2 * kRadius, 2 * kRadius), 180, -90);
    thumbPath.lineTo(thumbRect.right() - kRadius, thumbRect.top());
    thumbPath.arcTo(QRectF(thumbRect.right() - 2 * kRadius, thumbRect.top(),
                           2 * kRadius, 2 * kRadius), 90, -90);
    thumbPath.lineTo(thumbRect.right(), thumbRect.bottom());
    thumbPath.closeSubpath();

    const QString displayName =
        index.data(FileListViewModel::DisplayNameRole).toString();
    const QString thumbPath_ =
        index.data(FileListViewModel::ThumbnailPathRole).toString();

    painter->save();
    painter->setClipPath(thumbPath);

    QPixmap pix;
    if (!thumbPath_.isEmpty()) pix.load(thumbPath_);

    if (!pix.isNull()) {
        // 保持宽高比、居中裁剪填充
        const QSize target = thumbRect.size();
        QPixmap scaled = pix.scaled(target, Qt::KeepAspectRatioByExpanding,
                                    Qt::SmoothTransformation);
        const int dx = (scaled.width() - target.width()) / 2;
        const int dy = (scaled.height() - target.height()) / 2;
        painter->drawPixmap(thumbRect, scaled,
                            QRect(dx, dy, target.width(), target.height()));
    } else {
        // 无缩略图 → 渐变占位 + 中央播放按钮
        QLinearGradient g(thumbRect.topLeft(), thumbRect.bottomRight());
        const QColor base = placeholderTint(displayName);
        g.setColorAt(0.0, base.lighter(140));
        g.setColorAt(1.0, base.darker(140));
        painter->fillRect(thumbRect, g);

        // 半透明黑色蒙层
        painter->fillRect(thumbRect, QColor(0, 0, 0, 60));

        // 中央播放按钮（圆 + 三角）
        const QPointF c = thumbRect.center();
        const int r = 22;
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(255, 255, 255, 40));
        painter->drawEllipse(c, r + 4, r + 4);
        painter->setBrush(QColor(255, 255, 255, 210));
        painter->drawEllipse(c, r, r);

        painter->setBrush(QColor("#1E1E2E"));
        QPolygonF tri;
        tri << QPointF(c.x() - 6, c.y() - 9)
            << QPointF(c.x() - 6, c.y() + 9)
            << QPointF(c.x() + 9, c.y());
        painter->drawPolygon(tri);
    }
    painter->restore();

    // ---- 缩略图右下角时长徽标 ----
    qint64 durationMs = index.data(FileListViewModel::DurationRole).toLongLong();
    QString durationText;
    if (durationMs > 0) {
        const qint64 totalSec = durationMs / 1000;
        const qint64 h = totalSec / 3600;
        const qint64 m = (totalSec % 3600) / 60;
        const qint64 s = totalSec % 60;
        if (h > 0)
            durationText = QStringLiteral("%1:%2:%3")
                .arg(h)
                .arg(m, 2, 10, QLatin1Char('0'))
                .arg(s, 2, 10, QLatin1Char('0'));
        else
            durationText = QStringLiteral("%1:%2")
                .arg(m)
                .arg(s, 2, 10, QLatin1Char('0'));
    }

    if (!durationText.isEmpty()) {
        QFont badgeFont = option.font;
        badgeFont.setPixelSize(10);
        badgeFont.setBold(true);
        painter->setFont(badgeFont);
        QFontMetrics fm(badgeFont);
        const int dw = fm.horizontalAdvance(durationText) + 12;
        const int dh = 18;
        QRect dRect(thumbRect.right() - dw - 8,
                    thumbRect.bottom() - dh - 8, dw, dh);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0, 0, 0, 170));
        painter->drawRoundedRect(dRect, 4, 4);
        painter->setPen(QColor("#E0E0E0"));
        painter->drawText(dRect, Qt::AlignCenter, durationText);
    }

    // ---- 信息栏：单行 = 左 文件名 | 右 文件大小 ----
    QRect textRect = card;
    textRect.setTop(thumbRect.bottom() + 1);
    textRect = textRect.adjusted(12, 8, -12, -8);

    // 文件大小：先算好右侧宽度，便于文件名按剩余宽度 elide
    const qint64 sz = index.data(FileListViewModel::SizeRole).toLongLong();
    const QString sizeText = sz > 0 ? humanFileSize(sz) : QStringLiteral("—");

    QFont metaFont = option.font;
    metaFont.setPixelSize(11);
    painter->setFont(metaFont);
    QFontMetrics metaFm(metaFont);
    const int sizeW = metaFm.horizontalAdvance(sizeText);

    QRect nameRect = textRect;
    nameRect.setRight(nameRect.right() - sizeW - 8);

    QRect sizeRect = textRect;
    sizeRect.setLeft(sizeRect.right() - sizeW);

    painter->setPen(QColor("#E0E0E0"));
    const QString elidedName = metaFm.elidedText(
        displayName, Qt::ElideMiddle, nameRect.width());
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, elidedName);

    painter->setPen(QColor("#8B8B8B"));
    painter->drawText(sizeRect, Qt::AlignRight | Qt::AlignVCenter, sizeText);

    // ---- hover / selected 描边 ----
    QColor strokeColor;
    int strokeW = 0;
    if (option.state & QStyle::State_Selected) {
        strokeColor = QColor("#2979FF");
        strokeW = 2;
    } else if (option.state & QStyle::State_MouseOver) {
        strokeColor = QColor("#448AFF");
        strokeW = 1;
    }
    if (strokeW > 0) {
        QPen pen(strokeColor);
        pen.setWidth(strokeW);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(card, kRadius, kRadius);
    }

    painter->restore();
}

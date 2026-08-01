#include "view/chat/chatinputwidget.h"

#include <QTextEdit>
#include <QToolButton>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPixmap>
#include <QIcon>

#include "service/themeservice.h"

ChatInputWidget::ChatInputWidget(QWidget* parent)
    : QWidget(parent)
    , m_streaming(false)
{
    // 背景透明，跟随外层 ChatView 圆角卡片底色
    setAutoFillBackground(false);
    setAttribute(Qt::WA_StyledBackground, false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    // 快捷操作行
    auto* topRow = new QHBoxLayout();
    topRow->setSpacing(8);

    m_frameBtn = new QToolButton(this);
    m_frameBtn->setText(QStringLiteral("[📷] 当前帧"));
    m_frameBtn->setCheckable(false);
    m_frameBtn->setToolTip(tr("添加当前播放画面"));
    connect(m_frameBtn, &QToolButton::clicked, this, &ChatInputWidget::currentFrameRequested);
    topRow->addWidget(m_frameBtn);

    topRow->addStretch(1);
    layout->addLayout(topRow);

    // 帧预览容器（初始隐藏）
    m_framesContainer = new QWidget(this);
    m_framesContainer->setAutoFillBackground(false);
    m_framesContainer->setAttribute(Qt::WA_StyledBackground, false);
    m_framesContainer->hide();
    
    m_framesLayout = new QHBoxLayout(m_framesContainer);
    m_framesLayout->setContentsMargins(0, 0, 0, 0);
    m_framesLayout->setSpacing(8);
    m_framesLayout->addStretch(1);  // 左侧弹性空间，使帧靠左排列
    
    layout->addWidget(m_framesContainer);

    // 输入框
    m_edit = new QTextEdit(this);
    m_edit->setPlaceholderText(tr("输入问题…（Enter 发送，Shift+Enter 换行）"));
    m_edit->setFixedHeight(72);
    m_edit->installEventFilter(this);
    layout->addWidget(m_edit);

    // 发送/停止
    auto* bottomRow = new QHBoxLayout();
    bottomRow->addStretch(1);

    m_sendBtn = new QPushButton(tr("发送"), this);
    m_sendBtn->setMinimumWidth(72);
    m_sendBtn->setCursor(Qt::PointingHandCursor);
    connect(m_sendBtn, &QPushButton::clicked, this, [this]() {
        if (m_streaming) {
            emit stopRequested();
        } else {
            triggerSend();
        }
    });
    bottomRow->addWidget(m_sendBtn);
    layout->addLayout(bottomRow);

    // 应用默认颜色
    applyColors();
}

void ChatInputWidget::setThemeService(ThemeService* theme)
{
    m_theme = theme;
    applyColors();
}

void ChatInputWidget::applyColors()
{
    // 从 ThemeService 获取颜色，若无则使用暗色默认值
    const QColor border = m_theme
        ? m_theme->color(QStringLiteral("border"))
        : QColor("#2D2D3D");
    const QColor surface = m_theme
        ? m_theme->color(QStringLiteral("surfaceVariant"))
        : QColor("#252538");
    const QColor textSecondary = m_theme
        ? m_theme->color(QStringLiteral("textSecondary"))
        : QColor("#8B8B8B");
    const QColor textPrimary = m_theme
        ? m_theme->color(QStringLiteral("textPrimary"))
        : QColor("#E0E0E0");
    const QColor primary = m_theme
        ? m_theme->color(QStringLiteral("primary"))
        : QColor("#2979FF");
    const QColor primaryHover = m_theme
        ? m_theme->color(QStringLiteral("primaryHover"))
        : QColor("#448AFF");
    const QColor primaryPressed = m_theme
        ? m_theme->color(QStringLiteral("primaryPressed"))
        : QColor("#1565C0");
    const QColor inputBg = m_theme
        ? m_theme->color(QStringLiteral("inputBg"))
        : QColor("#1A1A2A");
    
    // 帧容器背景色（根据主题）
    const QColor frameBg = m_theme && !m_theme->isDark()
        ? QColor("#ffffff")   // 亮色主题：白色
        : QColor("#1e1e2e");  // 暗色主题：深色
    
    // ChatInputWidget 整体背景色
    QString widgetBg;
    if (m_theme) {
        if (m_theme->isDark()) {
            widgetBg = "#1e1e2e";  // 暗色模式
        } else {
            widgetBg = "#f5f5f5";  // 亮色模式
        }
    } else {
        widgetBg = "#1e1e2e";  // 默认暗色
    }
    
    // 设置 ChatInputWidget 的背景色
    setStyleSheet(QString("ChatInputWidget { background:%1; }").arg(widgetBg));

    m_frameBtn->setStyleSheet(QString(
        "QToolButton { border:1px solid %1; background:%2; color:%3; "
        "padding:4px 10px; border-radius:14px; font-size:12px; }"
        "QToolButton:hover { border-color:%4; color:%5; }")
        .arg(border.name(), surface.name(), textSecondary.name(),
             primary.name(), textPrimary.name()));

    m_edit->setStyleSheet(QString(
        "QTextEdit { background:%1; border:1px solid %2; border-radius:8px; "
        "color:%3; padding:8px 10px; font-size:13px; selection-background-color:%4; }"
        "QTextEdit:focus { border-color:%4; }"
        "QTextEdit:disabled { background:%5; color:%6; }")
        .arg(inputBg.name(), border.name(), textPrimary.name(),
             primary.name(), surface.name(), textSecondary.name()));

    m_sendBtn->setStyleSheet(QString(
        "QPushButton { background:%1; color:#FFFFFF; border:none; border-radius:6px; "
        "padding:8px 20px; font-size:13px; font-weight:500; }"
        "QPushButton:hover { background:%2; }"
        "QPushButton:pressed { background:%3; }"
        "QPushButton:disabled { background:%4; color:#8B8B8B; }")
        .arg(primary.name(), primaryHover.name(), primaryPressed.name(),
             primary.name() + "66"));
    
    // 为每个帧应用样式
    const bool dark = !m_theme || m_theme->isDark();
    const QString closeIconPath = dark
        ? QStringLiteral(":/icons/close_light.png")
        : QStringLiteral(":/icons/close_dark.png");
    
    for (auto& item : m_frames) {
        if (item.widget) {
            item.widget->setStyleSheet(QString(
                "QWidget { background:%1; border-radius:6px; }")
                .arg(frameBg.name()));
        }
        
        if (item.label) {
            item.label->setStyleSheet(QString(
                "QLabel { font-size:12px; color:%1; background:transparent; padding:0px; }")
                .arg(primary.name()));
        }
        
        if (item.deleteBtn) {
            item.deleteBtn->setIcon(QIcon(closeIconPath));
            item.deleteBtn->setIconSize(QSize(14, 14));
            item.deleteBtn->setStyleSheet(QString(
                "QToolButton { border:none; background:transparent; }"
                "QToolButton:hover { background:%1; border-radius:4px; }")
                .arg(surface.name()));
        }
    }
}

void ChatInputWidget::setStreaming(bool streaming)
{
    m_streaming = streaming;
    m_sendBtn->setText(streaming ? tr("停止") : tr("发送"));
    m_edit->setEnabled(!streaming);
    m_frameBtn->setEnabled(!streaming);
}

void ChatInputWidget::addFrame(const QImage& frame, int64_t timestampMs)
{
    if (frame.isNull()) return;
    
    FrameItem item;
    item.image = frame;
    item.timestamp = timestampMs;
    
    m_frames.append(item);
    createFramePreview(m_frames.size() - 1);
    
    m_framesContainer->show();
}

void ChatInputWidget::clearAllFrames()
{
    // 删除所有帧的 widget
    for (auto& item : m_frames) {
        if (item.widget) {
            m_framesLayout->removeWidget(item.widget);
            item.widget->deleteLater();
        }
    }
    
    m_frames.clear();
    m_framesContainer->hide();
}

void ChatInputWidget::createFramePreview(int index)
{
    if (index < 0 || index >= m_frames.size()) return;
    
    auto& item = m_frames[index];
    
    // 创建帧容器
    item.widget = new QWidget(m_framesContainer);
    item.widget->setAutoFillBackground(false);
    item.widget->setAttribute(Qt::WA_StyledBackground, false);
    
    auto* frameLayout = new QHBoxLayout(item.widget);
    frameLayout->setContentsMargins(8, 4, 8, 4);
    frameLayout->setSpacing(6);
    
    // 帧标签（文件名+时间戳）
    item.label = new QLabel(item.widget);
    const QString timeStr = formatTimestamp(item.timestamp);
    item.label->setText(QStringLiteral("image_%1").arg(timeStr));
    item.label->setCursor(Qt::PointingHandCursor);
    item.label->setProperty("frameIndex", index);
    item.label->installEventFilter(this);
    frameLayout->addWidget(item.label);
    
    // 删除按钮
    item.deleteBtn = new QToolButton(item.widget);
    item.deleteBtn->setToolTip(tr("取消发送此帧"));
    item.deleteBtn->setFixedSize(20, 20);
    item.deleteBtn->setCursor(Qt::PointingHandCursor);
    item.deleteBtn->setProperty("frameIndex", index);
    connect(item.deleteBtn, &QToolButton::clicked, this, [this, index]() {
        removeFrame(index);
    });
    frameLayout->addWidget(item.deleteBtn);
    
    // 插入到布局中（在 stretch 之前）
    m_framesLayout->insertWidget(m_framesLayout->count() - 1, item.widget);
    
    // 应用样式
    applyColors();
}

void ChatInputWidget::removeFrame(int index)
{
    if (index < 0 || index >= m_frames.size()) return;
    
    auto& item = m_frames[index];
    if (item.widget) {
        m_framesLayout->removeWidget(item.widget);
        item.widget->deleteLater();
    }
    
    m_frames.removeAt(index);
    
    // 更新剩余帧的索引
    for (int i = 0; i < m_frames.size(); ++i) {
        if (m_frames[i].label) {
            m_frames[i].label->setProperty("frameIndex", i);
        }
        if (m_frames[i].deleteBtn) {
            m_frames[i].deleteBtn->setProperty("frameIndex", i);
            disconnect(m_frames[i].deleteBtn, nullptr, this, nullptr);
            connect(m_frames[i].deleteBtn, &QToolButton::clicked, this, [this, i]() {
                removeFrame(i);
            });
        }
    }
    
    if (m_frames.isEmpty()) {
        m_framesContainer->hide();
    }
}

void ChatInputWidget::onViewFrame(int index)
{
    if (index < 0 || index >= m_frames.size()) return;
    
    const auto& item = m_frames[index];
    if (item.image.isNull()) return;
    
    // 创建一个简单的对话框显示图片
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle(tr("查看帧 - %1").arg(formatTimestamp(item.timestamp)));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->resize(800, 600);
    
    auto* layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(10, 10, 10, 10);
    
    auto* imageLabel = new QLabel(dlg);
    imageLabel->setAlignment(Qt::AlignCenter);
    
    // 缩放图片以适应对话框，保持宽高比
    QPixmap pixmap = QPixmap::fromImage(item.image);
    pixmap = pixmap.scaled(780, 520, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    imageLabel->setPixmap(pixmap);
    layout->addWidget(imageLabel, 1);
    
    auto* btnBox = new QDialogButtonBox(dlg);
    auto* closeBtn = btnBox->addButton(tr("关闭"), QDialogButtonBox::RejectRole);
    connect(btnBox, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    layout->addWidget(btnBox);
    
    dlg->exec();
}

QString ChatInputWidget::formatTimestamp(int64_t ms) const
{
    int totalSeconds = static_cast<int>(ms / 1000);
    int minutes = totalSeconds / 60;
    int seconds = totalSeconds % 60;
    return QString("%1:%2").arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0'));
}

void ChatInputWidget::triggerSend()
{
    const QString text = m_edit->toPlainText().trimmed();
    const bool hasFrames = !m_frames.isEmpty();
    if (text.isEmpty() && !hasFrames) return;
    emit sendRequested(text, hasFrames);
    m_edit->clear();
    clearAllFrames();
}

bool ChatInputWidget::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_edit && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)
            && !(ke->modifiers() & Qt::ShiftModifier)) {
            if (!m_streaming) triggerSend();
            return true;  // 拦截换行
        }
    }
    
    // 处理帧标签的点击（直接点击文本预览图片）
    for (int i = 0; i < m_frames.size(); ++i) {
        if (obj == m_frames[i].label && event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                onViewFrame(i);
                return true;
            }
        }
    }
    
    return QWidget::eventFilter(obj, event);
}

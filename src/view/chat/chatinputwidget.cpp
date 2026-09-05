#include "view/chat/chatinputwidget.h"

#include <QTextEdit>
#include <QToolButton>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPixmap>
#include <QIcon>
#include <QSize>
#include <QLabel>
#include <QMenu>
#include <QSpinBox>
#include <QFormLayout>
#include <utility>

#include "service/themeservice.h"

ChatInputWidget::ChatInputWidget(QWidget* parent)
    : QWidget(parent)
    , m_streaming(false)
    , m_currentTimestampMs(0)
    , m_videoDurationMs(0)
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

    m_frameButton = new QToolButton(this);
    m_frameButton->setText(QStringLiteral("📷 添加当前帧"));
    m_frameButton->setToolTip(tr("添加当前播放画面"));
    connect(m_frameButton, &QToolButton::clicked,
            this, &ChatInputWidget::currentFrameRequested);
    topRow->addWidget(m_frameButton);

    m_timeRangeButton = new QToolButton(this);
    m_timeRangeButton->setText(QStringLiteral("⏱ 时间段"));
    m_timeRangeButton->setToolTip(tr("选择分析时间范围"));
    connect(m_timeRangeButton, &QToolButton::clicked,
            this, &ChatInputWidget::showTimeRangeDialog);
    topRow->addWidget(m_timeRangeButton);

    m_templateButton = new QToolButton(this);
    m_templateButton->setText(QStringLiteral("💡 快速提问"));
    m_templateButton->setToolTip(tr("选择预设问题模板"));
    connect(m_templateButton, &QToolButton::clicked,
            this, &ChatInputWidget::showTemplateMenu);
    topRow->addWidget(m_templateButton);

    topRow->addStretch(1);
    layout->addLayout(topRow);

    m_framesContainer = new QWidget(this);
    m_framesContainer->hide();
    m_framesLayout = new QHBoxLayout(m_framesContainer);
    m_framesLayout->setContentsMargins(0, 0, 0, 0);
    m_framesLayout->setSpacing(8);
    m_framesLayout->addStretch(1);
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

    m_sendButton = new QPushButton(tr("发送"), this);
    m_sendButton->setMinimumWidth(72);
    m_sendButton->setCursor(Qt::PointingHandCursor);
    connect(m_sendButton, &QPushButton::clicked, this, [this]() {
        if (m_streaming) {
            emit stopRequested();
        } else {
            triggerSend();
        }
    });
    bottomRow->addWidget(m_sendButton);
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

    const QString buttonStyle = QString(
        "QToolButton { border:1px solid %1; background:%2; color:%3; "
        "padding:4px 10px; border-radius:14px; font-size:12px; }"
        "QToolButton:hover { border-color:%4; color:%5; }"
        "QToolButton:disabled { background:%6; color:%7; border-color:%1; }")
        .arg(border.name(), surface.name(), textSecondary.name(),
             primary.name(), textPrimary.name(), surface.name(), textSecondary.name());

    m_frameButton->setStyleSheet(buttonStyle);
    m_timeRangeButton->setStyleSheet(buttonStyle);
    m_templateButton->setStyleSheet(buttonStyle);

    m_edit->setStyleSheet(QString(
        "QTextEdit { background:%1; border:1px solid %2; border-radius:8px; "
        "color:%3; padding:8px 10px; font-size:13px; selection-background-color:%4; }"
        "QTextEdit:focus { border-color:%4; }"
        "QTextEdit:disabled { background:%5; color:%6; }")
        .arg(inputBg.name(), border.name(), textPrimary.name(),
             primary.name(), surface.name(), textSecondary.name()));

    m_sendButton->setStyleSheet(QString(
        "QPushButton { background:%1; color:#FFFFFF; border:none; border-radius:6px; "
        "padding:8px 20px; font-size:13px; font-weight:500; }"
        "QPushButton:hover { background:%2; }"
        "QPushButton:pressed { background:%3; }"
        "QPushButton:disabled { background:%4; color:#8B8B8B; }")
        .arg(primary.name(), primaryHover.name(), primaryPressed.name(),
             primary.name() + "66"));
}

void ChatInputWidget::setStreaming(bool streaming)
{
    m_streaming = streaming;
    m_sendButton->setText(streaming ? tr("停止") : tr("发送"));
    m_edit->setEnabled(!streaming);
    m_frameButton->setEnabled(!streaming);
    m_timeRangeButton->setEnabled(!streaming);
    m_templateButton->setEnabled(!streaming);
}

void ChatInputWidget::setCurrentTimestamp(int64_t timestampMs)
{
    m_currentTimestampMs = timestampMs;
}

void ChatInputWidget::setVideoDuration(int64_t durationMs)
{
    m_videoDurationMs = durationMs;
}

void ChatInputWidget::addFrame(const QImage& frame, int64_t timestampMs)
{
    if (frame.isNull()) return;
    FrameItem item;
    item.image = frame;
    item.timestampMs = timestampMs;
    m_frames.append(item);
    createFramePreview(m_frames.size() - 1);
    m_framesContainer->show();
}

void ChatInputWidget::clearAllFrames()
{
    for (FrameItem& item : m_frames) {
        if (item.widget) {
            m_framesLayout->removeWidget(item.widget);
            item.widget->deleteLater();
        }
    }
    m_frames.clear();
    m_framesContainer->hide();
    emit allFramesCleared();
}

void ChatInputWidget::createFramePreview(int index)
{
    if (index < 0 || index >= m_frames.size()) return;
    FrameItem& item = m_frames[index];
    item.widget = new QWidget(m_framesContainer);
    auto* frameLayout = new QHBoxLayout(item.widget);
    frameLayout->setContentsMargins(8, 4, 6, 4);
    frameLayout->setSpacing(5);
    item.label = new QLabel(formatTimestamp(item.timestampMs), item.widget);
    item.label->setCursor(Qt::PointingHandCursor);
    item.label->setProperty("frameIndex", index);
    item.label->installEventFilter(this);
    frameLayout->addWidget(item.label);
    item.deleteButton = new QToolButton(item.widget);
    item.deleteButton->setText(QStringLiteral("×"));
    item.deleteButton->setToolTip(tr("删除此帧"));
    item.deleteButton->setFixedSize(20, 20);
    connect(item.deleteButton, &QToolButton::clicked, this, [this, index]() {
        removeFrame(index);
    });
    frameLayout->addWidget(item.deleteButton);
    m_framesLayout->insertWidget(m_framesLayout->count() - 1, item.widget);
    applyColors();
}

void ChatInputWidget::removeFrame(int index)
{
    if (index < 0 || index >= m_frames.size()) return;
    if (m_frames[index].widget) {
        m_framesLayout->removeWidget(m_frames[index].widget);
        m_frames[index].widget->deleteLater();
    }
    m_frames.removeAt(index);
    emit frameRemoved(index);
    const QList<FrameItem> remaining = m_frames;
    clearAllFrames();
    for (const FrameItem& remainingItem : remaining)
        addFrame(remainingItem.image, remainingItem.timestampMs);
}

void ChatInputWidget::showFramePreview(int index)
{
    if (index < 0 || index >= m_frames.size()) return;
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("查看帧 %1").arg(formatTimestamp(m_frames[index].timestampMs)));
    dialog->resize(800, 600);
    auto* layout = new QVBoxLayout(dialog);
    auto* image = new QLabel(dialog);
    image->setAlignment(Qt::AlignCenter);
    image->setPixmap(QPixmap::fromImage(m_frames[index].image).scaled(
        760, 520, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    layout->addWidget(image, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog->exec();
}

QString ChatInputWidget::formatTimestamp(int64_t timestampMs) const
{
    const int totalSeconds = static_cast<int>(timestampMs / 1000);
    return QStringLiteral("image_%1:%2")
        .arg(totalSeconds / 60, 2, 10, QChar('0'))
        .arg(totalSeconds % 60, 2, 10, QChar('0'));
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
    if (obj != m_edit && event->type() == QEvent::MouseButtonPress) {
        for (int i = 0; i < m_frames.size(); ++i) {
            if (obj == m_frames[i].label) {
                showFramePreview(i);
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void ChatInputWidget::showTemplateMenu()
{
    if (!m_templateMenu) {
        m_templateMenu = new QMenu(this);
        
        // 根据主题设置菜单样式
        if (m_theme) {
            const QColor surface = m_theme->color(QStringLiteral("surface"));
            const QColor border = m_theme->color(QStringLiteral("border"));
            const QColor textPrimary = m_theme->color(QStringLiteral("textPrimary"));
            const QColor primary = m_theme->color(QStringLiteral("primary"));
            
            m_templateMenu->setStyleSheet(QString(
                "QMenu { background:%1; border:1px solid %2; border-radius:8px; padding:4px; }"
                "QMenu::item { color:%3; padding:8px 20px; border-radius:4px; }"
                "QMenu::item:selected { background:%4; color:#FFFFFF; }")
                .arg(surface.name(), border.name(), textPrimary.name(), primary.name()));
        }

        // 添加预设问题模板
        struct Template {
            QString icon;
            QString text;
        };
        
        const QList<Template> templates = {
            {QStringLiteral("📝"), tr("总结这个视频的主要内容")},
            {QStringLiteral("🎬"), tr("这一段发生了什么？")},
            {QStringLiteral("🔍"), tr("识别画面中的物体")},
            {QStringLiteral("📄"), tr("提取画面中的文字")},
            {QStringLiteral("🚶"), tr("分析画面中的人物动作")},
            {QStringLiteral("⚖️"), tr("对比这几帧的差异")}
        };
        
        for (const Template& tmpl : templates) {
            QAction* action = m_templateMenu->addAction(tmpl.icon + QStringLiteral("  ") + tmpl.text);
            connect(action, &QAction::triggered, this, [this, tmpl]() {
                insertTemplate(tmpl.text);
            });
        }
    }
    
    // 在按钮下方显示菜单
    QPoint pos = m_templateButton->mapToGlobal(QPoint(0, m_templateButton->height()));
    m_templateMenu->exec(pos);
}

void ChatInputWidget::showTimeRangeDialog()
{
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("选择分析时间范围"));
    dialog->setFixedWidth(400);
    
    auto* layout = new QVBoxLayout(dialog);
    layout->setSpacing(16);
    layout->setContentsMargins(20, 20, 20, 20);
    
    // 说明文本
    auto* descLabel = new QLabel(tr("选择要分析的视频时间段："), dialog);
    descLabel->setStyleSheet(QStringLiteral("QLabel { font-size:13px; }"));
    layout->addWidget(descLabel);
    
    // 时间输入表单
    auto* formLayout = new QFormLayout();
    formLayout->setSpacing(12);
    
    // 开始时间（秒）
    auto* startSpinBox = new QSpinBox(dialog);
    startSpinBox->setMinimum(0);
    startSpinBox->setMaximum(static_cast<int>(m_videoDurationMs / 1000));
    startSpinBox->setValue(static_cast<int>(m_currentTimestampMs / 1000));
    startSpinBox->setSuffix(tr(" 秒"));
    startSpinBox->setMinimumWidth(150);
    formLayout->addRow(tr("开始时间:"), startSpinBox);
    
    // 结束时间（秒）
    auto* endSpinBox = new QSpinBox(dialog);
    endSpinBox->setMinimum(0);
    endSpinBox->setMaximum(static_cast<int>(m_videoDurationMs / 1000));
    endSpinBox->setValue(qMin(static_cast<int>(m_currentTimestampMs / 1000) + 10,
                              static_cast<int>(m_videoDurationMs / 1000)));
    endSpinBox->setSuffix(tr(" 秒"));
    endSpinBox->setMinimumWidth(150);
    formLayout->addRow(tr("结束时间:"), endSpinBox);
    
    layout->addLayout(formLayout);
    
    // 快速选择按钮
    auto* quickRow = new QHBoxLayout();
    quickRow->setSpacing(8);
    
    auto* labelQuick = new QLabel(tr("快速选择："), dialog);
    labelQuick->setStyleSheet(QStringLiteral("QLabel { color:#8B8B8B; font-size:12px; }"));
    quickRow->addWidget(labelQuick);
    
    struct QuickOption {
        QString label;
        int seconds;
    };
    
    const QList<QuickOption> quickOptions = {
        {tr("前后5秒"), 5},
        {tr("前后10秒"), 10},
        {tr("前后30秒"), 30},
        {tr("前后60秒"), 60}
    };
    
    for (const QuickOption& opt : quickOptions) {
        auto* btn = new QPushButton(opt.label, dialog);
        btn->setStyleSheet(QStringLiteral(
            "QPushButton { border:1px solid #2D2D3D; background:#252538; "
            "padding:4px 8px; border-radius:4px; font-size:11px; }"
            "QPushButton:hover { border-color:#2979FF; }"));
        connect(btn, &QPushButton::clicked, [startSpinBox, endSpinBox, opt, this]() {
            const int current = static_cast<int>(m_currentTimestampMs / 1000);
            const int maxTime = static_cast<int>(m_videoDurationMs / 1000);
            startSpinBox->setValue(qMax(0, current - opt.seconds));
            endSpinBox->setValue(qMin(maxTime, current + opt.seconds));
        });
        quickRow->addWidget(btn);
    }
    
    quickRow->addStretch(1);
    layout->addLayout(quickRow);
    
    // 按钮
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    
    if (dialog->exec() == QDialog::Accepted) {
        const int64_t startMs = static_cast<int64_t>(startSpinBox->value()) * 1000;
        const int64_t endMs = static_cast<int64_t>(endSpinBox->value()) * 1000;
        
        if (startMs < endMs) {
            // 在输入框插入时间范围标记
            const QString rangeText = QString::fromUtf8("[时间段 %1-%2] ")
                .arg(formatTimestamp(startMs))
                .arg(formatTimestamp(endMs));
            m_edit->insertPlainText(rangeText);
            
            // 发出信号，让上层添加关键帧或其他处理
            emit timeRangeRequested(startMs, endMs);
        }
    }
}

void ChatInputWidget::insertTemplate(const QString& templateText)
{
    // 清空输入框并插入模板文本
    m_edit->clear();
    m_edit->setPlainText(templateText);
    m_edit->setFocus();
    
    // 将光标移到末尾
    QTextCursor cursor = m_edit->textCursor();
    cursor.movePosition(QTextCursor::End);
    m_edit->setTextCursor(cursor);
}

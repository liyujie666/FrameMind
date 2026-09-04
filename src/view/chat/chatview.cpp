#include "view/chat/chatview.h"

#include "view/chat/chatmessagelist.h"
#include "view/chat/chatinputwidget.h"
#include "viewmodel/chatviewmodel.h"
#include "viewmodel/chatmessagelistmodel.h"
#include "service/themeservice.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QLabel>
#include <QMenu>
#include <QRegularExpression>
#include <QScrollBar>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>

static constexpr int kPanelRadius = 14;

ChatView::ChatView(QWidget* parent)
    : QWidget(parent)
{
    // 顶层自绘圆角卡片，禁止子 widget 的样式表污染
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);

    // 默认暗色配色（未接入 ThemeService 时的 fallback）
    m_bgColor            = QColor("#1E1E2E");
    m_borderColor        = QColor("#2D2D3D");
    m_headerDividerColor = QColor("#2D2D3D");
    m_textPrimary        = QColor("#E0E0E0");
    m_textSecondary      = QColor("#8B8B8B");
    m_surfaceVariant     = QColor("#252538");
    m_primary            = QColor("#2979FF");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);   // 让边框可见
    layout->setSpacing(0);

    // ---- 顶部 header ----
    m_header = new QWidget(this);
    m_header->setFixedHeight(48);
    m_header->setAutoFillBackground(false);
    m_header->setAttribute(Qt::WA_StyledBackground, false);
    auto* hl = new QHBoxLayout(m_header);
    hl->setContentsMargins(16, 4, 12, 4);
    hl->setSpacing(6);

    m_titleLabel = new QLabel(tr("AI Chat"), m_header);
    hl->addWidget(m_titleLabel);

    m_convButton = new QToolButton(m_header);
    m_convButton->setToolTip(tr("切换会话"));
    m_convButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_convButton->setIconSize(QSize(16, 16));
    m_convButton->setFixedSize(24, 24);
    connect(m_convButton, &QToolButton::clicked,
            this, &ChatView::showConversationMenu);
    hl->addWidget(m_convButton);

    hl->addStretch(1);

    m_newButton = new QToolButton(m_header);
    m_newButton->setText(QStringLiteral("+"));
    m_newButton->setToolTip(tr("新建对话"));
    m_newButton->setFixedSize(28, 28);
    hl->addWidget(m_newButton);

    layout->addWidget(m_header);

    // ---- 消息列表 ----
    m_messageList = new ChatMessageList(this);
    m_messageList->setAttribute(Qt::WA_StyledBackground, false);
    if (m_messageList->viewport()) {
        m_messageList->viewport()->setAutoFillBackground(false);
        m_messageList->viewport()->setAttribute(Qt::WA_StyledBackground, false);
    }
    layout->addWidget(m_messageList, 1);
    connect(m_messageList, &ChatMessageList::linkActivated,
            this, &ChatView::onLinkActivated);

    // ---- 输入区 ----
    m_inputWidget = new ChatInputWidget(this);
    layout->addWidget(m_inputWidget);

    applyThemeColors();
}

void ChatView::setThemeService(ThemeService* theme)
{
    if (m_theme == theme) return;
    if (m_theme) disconnect(m_theme, nullptr, this, nullptr);
    m_theme = theme;

    // 传播到子组件
    if (m_messageList) m_messageList->setThemeService(theme);
    if (m_inputWidget) m_inputWidget->setThemeService(theme);

    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, &ChatView::onThemeChanged);
        applyThemeColors();
        update();
    }
}

void ChatView::onThemeChanged()
{
    applyThemeColors();

    // 批量刷新气泡颜色（不重建 widget，避免布局抖动）
    if (m_messageList) m_messageList->refreshBubbleColors();
    if (m_inputWidget) m_inputWidget->setThemeService(m_theme);

    update();
}

void ChatView::applyThemeColors()
{
    if (m_theme) {
        m_bgColor            = m_theme->color(QStringLiteral("surface"));
        m_borderColor        = m_theme->color(QStringLiteral("border"));
        m_headerDividerColor = m_theme->color(QStringLiteral("border"));
        m_textPrimary        = m_theme->color(QStringLiteral("textPrimary"));
        m_textSecondary      = m_theme->color(QStringLiteral("textSecondary"));
        m_surfaceVariant     = m_theme->color(QStringLiteral("surfaceVariant"));
        m_primary            = m_theme->color(QStringLiteral("primary"));
    }

    // 暂停本控件树的更新，合并多次 setStyleSheet 的重绘为一次
    setUpdatesEnabled(false);

    // 标题
    m_titleLabel->setStyleSheet(QString(
        "font-size:15px; font-weight:600; color:%1; background:transparent; border:none;")
        .arg(m_textPrimary.name()));

    // 下拉小箭头 — 图标跟随主题（亮色主题用 dark 图标，暗色主题用 light 图标）
    m_convButton->setStyleSheet(QString(
        "QToolButton { border:none; padding:2px; border-radius:4px; background:transparent; }"
        "QToolButton:hover { background:%1; }")
        .arg(m_surfaceVariant.name()));
    {
        const bool dark = !m_theme || m_theme->isDark();
        m_convButton->setIcon(QIcon(dark
            ? QStringLiteral(":/icons/down_light.png")
            : QStringLiteral(":/icons/down_dark.png")));
    }

    m_newButton->setStyleSheet(QString(
        "QToolButton { border:none; color:%1; border-radius:6px; font-size:18px; background:transparent; }"
        "QToolButton:hover { color:#FFFFFF; background:%2; }")
        .arg(m_textSecondary.name(), m_primary.name()));

    // 消息列表滚动条 - 现代简约风格
    const QColor scrollThumb = m_theme
        ? m_theme->color(QStringLiteral("scrollThumb"))
        : QColor("#3A3A4A");
    const QString thumbHover = scrollThumb.lighter(130).name();
    const QString thumbPressed = scrollThumb.lighter(160).name();
    m_messageList->setStyleSheet(QString(
        "QScrollArea { background:transparent; border:none; }"
        "QScrollArea > QWidget > QWidget { background:transparent; }"
        "QScrollBar:vertical {"
        "    background:transparent;"
        "    width:10px;"
        "    margin:4px 2px 4px 2px;"
        "    border:none;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }"
        "QScrollBar::handle:vertical {"
        "    background:%1;"
        "    border-radius:5px;"
        "    min-height:40px;"
        "    margin:0 2px;"
        "}"
        "QScrollBar::handle:vertical:hover { background:%2; }"
        "QScrollBar::handle:vertical:pressed { background:%3; }")
        .arg(scrollThumb.name(), thumbHover, thumbPressed));

    // 恢复更新，触发一次统一重绘
    setUpdatesEnabled(true);
}

void ChatView::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(r, kPanelRadius, kPanelRadius);
    p.fillPath(path, m_bgColor);

    QPen pen(m_borderColor);
    pen.setWidth(1);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    // header 底部分割线
    if (m_header) {
        const int y = m_header->y() + m_header->height();
        p.setPen(QPen(m_headerDividerColor, 1));
        p.drawLine(QPointF(kPanelRadius, y + 0.5),
                   QPointF(width() - kPanelRadius, y + 0.5));
    }
}

void ChatView::setViewModel(ChatViewModel* vm)
{
    m_vm = vm;
    if (!m_vm) return;

    m_messageList->setModel(m_vm->messageModel());

    connect(m_inputWidget, &ChatInputWidget::sendRequested, this,
            [this](const QString& text, bool withFrames) {
                if (withFrames) m_vm->sendMessageWithCachedFrame(text);
                else m_vm->sendMessage(text);
            });
    connect(m_inputWidget, &ChatInputWidget::currentFrameRequested,
            m_vm, &ChatViewModel::requestCurrentFrame);
    connect(m_inputWidget, &ChatInputWidget::frameRemoved,
            m_vm, &ChatViewModel::removeFrameFromCache);
    connect(m_inputWidget, &ChatInputWidget::allFramesCleared,
            m_vm, &ChatViewModel::clearAllCachedFrames);
    connect(m_vm, &ChatViewModel::currentFrameReady,
            this, [this](const QImage& frame, int64_t timestampMs) {
                m_inputWidget->addFrame(frame, timestampMs);
            });
    connect(m_inputWidget, &ChatInputWidget::stopRequested,
            m_vm, &ChatViewModel::stopGeneration);

    connect(m_vm, &ChatViewModel::streamingChanged,
            m_inputWidget, &ChatInputWidget::setStreaming);
    connect(m_vm, &ChatViewModel::conversationsChanged,
            this, &ChatView::refreshHeader);
    connect(m_vm, &ChatViewModel::conversationChanged,
            this, [this](const QString&) { refreshHeader(); });

    connect(m_newButton, &QToolButton::clicked,
            m_vm, &ChatViewModel::createNewConversation);

    refreshHeader();
}

void ChatView::refreshHeader()
{
    if (!m_vm) return;
    const QString curId = m_vm->currentConversationId();
    QString convTitle;
    const auto convs = m_vm->conversations();
    for (const auto& c : convs) {
        if (c.id == curId) { convTitle = c.title; break; }
    }
    m_titleLabel->setText(tr("AI Chat"));
    if (m_convButton) {
        m_convButton->setToolTip(convTitle.isEmpty()
                                     ? tr("切换会话")
                                     : convTitle);
    }
}

void ChatView::showConversationMenu()
{
    if (!m_vm) return;
    QMenu menu(this);
    const auto convs = m_vm->conversations();
    const QString curId = m_vm->currentConversationId();

    if (convs.isEmpty()) {
        menu.addAction(tr("（暂无历史会话）"))->setEnabled(false);
    } else {
        // 按视频ID分组
        QMap<QString, QList<Conversation>> groupedConvs;
        for (const auto& c : convs) {
            const QString videoKey = c.videoId.isEmpty() 
                ? QStringLiteral("__no_video__") 
                : c.videoId;
            groupedConvs[videoKey].append(c);
        }

        bool firstGroup = true;
        for (auto it = groupedConvs.begin(); it != groupedConvs.end(); ++it) {
            const QString& videoKey = it.key();
            const QList<Conversation>& videoConvs = it.value();
            
            if (!firstGroup) {
                menu.addSeparator();
            }
            firstGroup = false;

            // 分组标题（显示视频文件名）
            QString groupTitle;
            if (videoKey == QStringLiteral("__no_video__")) {
                groupTitle = tr("【无关联视频】");
            } else if (!videoConvs.isEmpty() && !videoConvs.first().videoFilePath.isEmpty()) {
                QFileInfo fi(videoConvs.first().videoFilePath);
                groupTitle = tr("【%1】").arg(fi.fileName());
            } else {
                groupTitle = tr("【视频 %1】").arg(videoKey.left(8));
            }
            
            QAction* groupHeader = menu.addAction(groupTitle);
            groupHeader->setEnabled(false);
            QFont headerFont = groupHeader->font();
            headerFont.setBold(true);
            groupHeader->setFont(headerFont);

            // 该视频的会话列表
            for (const auto& c : videoConvs) {
                QString displayText = c.title.isEmpty() ? tr("新对话") : c.title;
                
                // 添加时间戳
                if (c.updatedAt.isValid()) {
                    const QDateTime now = QDateTime::currentDateTime();
                    const qint64 secsDiff = c.updatedAt.secsTo(now);
                    
                    QString timeStr;
                    if (secsDiff < 60) {
                        timeStr = tr("刚刚");
                    } else if (secsDiff < 3600) {
                        timeStr = tr("%1分钟前").arg(secsDiff / 60);
                    } else if (secsDiff < 86400) {
                        timeStr = tr("%1小时前").arg(secsDiff / 3600);
                    } else if (secsDiff < 604800) {
                        timeStr = tr("%1天前").arg(secsDiff / 86400);
                    } else {
                        timeStr = c.updatedAt.toString(QStringLiteral("yyyy-MM-dd"));
                    }
                    displayText = QStringLiteral("  %1  (%2)").arg(displayText, timeStr);
                } else {
                    displayText = QStringLiteral("  %1").arg(displayText);
                }
                
                QAction* act = menu.addAction(displayText);
                act->setCheckable(true);
                act->setChecked(c.id == curId);
                const QString id = c.id;
                connect(act, &QAction::triggered, this,
                        [this, id]() { m_vm->switchConversation(id); });
            }
        }
    }
    
    menu.addSeparator();
    connect(menu.addAction(tr("➕ 新建对话")), &QAction::triggered,
            m_vm, &ChatViewModel::createNewConversation);
    if (!curId.isEmpty()) {
        connect(menu.addAction(tr("🗑 删除当前对话")), &QAction::triggered, this,
                [this, curId]() { m_vm->deleteConversation(curId); });
    }
    menu.exec(m_convButton->mapToGlobal(QPoint(0, m_convButton->height())));
}

void ChatView::onLinkActivated(const QString& href)
{
    static const QRegularExpression re(QStringLiteral("^ts://(\\d+)$"));
    const auto m = re.match(href);
    if (m.hasMatch() && m_vm) {
        m_vm->onTimestampClicked(m.captured(1).toLongLong());
    }
}

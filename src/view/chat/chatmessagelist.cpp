#include "view/chat/chatmessagelist.h"

#include "view/chat/chatbubblewidget.h"
#include "viewmodel/chatmessagelistmodel.h"
#include "service/themeservice.h"
#include "service/markdownrenderer.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QTimer>
#include <QLabel>
#include <QToolButton>
#include <QClipboard>
#include <QGuiApplication>
#include <QMovie>
#include <QDateTime>
#include <QPixmap>
#include <utility>

ChatMessageList::ChatMessageList(QWidget* parent)
    : QScrollArea(parent)
{
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    setBackgroundRole(QPalette::NoRole);
    // 背景透明：由外层 ChatView 圆角卡片提供底色
    setStyleSheet(QStringLiteral(
        "QScrollArea { background:transparent; border:none; }"
        "QWidget#chatMessageContainer { background:transparent; }"));

    m_container = new QWidget(this);
    m_container->setObjectName(QStringLiteral("chatMessageContainer"));
    m_container->setAutoFillBackground(false);
    m_container->setAttribute(Qt::WA_StyledBackground, false);
    m_layout = new QVBoxLayout(m_container);
    m_layout->setContentsMargins(16, 16, 16, 16);
    m_layout->setSpacing(16);
    m_layout->addStretch(1);   // 末尾弹簧，气泡插入其前
    setWidget(m_container);
}

void ChatMessageList::setThemeService(ThemeService* theme)
{
    m_theme = theme;
}

void ChatMessageList::setMarkdownRenderer(MarkdownRenderer* renderer)
{
    m_renderer = renderer;
}

void ChatMessageList::refreshBubbleColors()
{
    // 【性能优化】智能刷新：只更新可见的气泡，非可见的延迟刷新
    m_container->setUpdatesEnabled(false);
    
    for (int i = 0; i < m_bubbles.size(); ++i) {
        ChatBubbleWidget* bubble = m_bubbles[i];
        bubble->setThemeService(m_theme);
        bubble->setMarkdownRenderer(m_renderer);
        
        // 判断气泡是否在可见区域
        if (isBubbleVisible(bubble)) {
            // 可见气泡：立即完整刷新
            bubble->refreshColors();
        } else {
            // 不可见气泡：只更新颜色，延迟 HTML 刷新
            bubble->refreshColorsLazy();
        }
        
        // 【修复】更新操作栏的图标
        updateActionBarIcons(i);
    }
    
    m_container->setUpdatesEnabled(true);
    m_container->update();
    m_themeDirty = false;
}

void ChatMessageList::updateActionBarIcons(int row)
{
    if (row < 0 || row >= m_bubbles.size()) return;
    
    // 获取 wrapper widget（气泡的父容器的父容器）
    QWidget* bubbleParent = m_bubbles[row]->parentWidget();
    if (!bubbleParent) return;
    
    QWidget* wrapper = bubbleParent->parentWidget();
    if (!wrapper) return;
    
    // 查找操作栏中的所有按钮（递归查找，不限制只找直接子级）
    QList<QToolButton*> buttons = wrapper->findChildren<QToolButton*>();
    
    const bool isDark = !m_theme || m_theme->isDark();
    const QString iconSuffix = isDark ? QStringLiteral("_light.png") : QStringLiteral("_dark.png");
    
    for (QToolButton* btn : buttons) {
        const QString tooltip = btn->toolTip();
        if (tooltip.contains(tr("复制")) || tooltip.contains(QString::fromUtf8("复制"))) {
            btn->setIcon(QIcon(QStringLiteral(":/icons/copy") + iconSuffix));
        } else if (tooltip.contains(tr("重新生成")) || tooltip.contains(QString::fromUtf8("重新生成"))) {
            btn->setIcon(QIcon(QStringLiteral(":/icons/replay") + iconSuffix));
        }
    }
}

void ChatMessageList::refreshBubbleColorsProgressive()
{
    // 【性能优化】渐进式刷新：分批更新，避免一次性卡顿
    constexpr int BATCH_SIZE = 5;  // 每批处理 5 个气泡
    
    m_container->setUpdatesEnabled(false);
    
    // 第一批：立即更新可见的气泡
    QList<int> visibleIndices;
    QList<int> invisibleIndices;
    
    for (int i = 0; i < m_bubbles.size(); ++i) {
        if (isBubbleVisible(m_bubbles[i])) {
            visibleIndices.append(i);
        } else {
            invisibleIndices.append(i);
        }
    }
    
    // 立即更新可见气泡
    for (int idx : visibleIndices) {
        m_bubbles[idx]->setThemeService(m_theme);
        m_bubbles[idx]->setMarkdownRenderer(m_renderer);
        m_bubbles[idx]->refreshColors();
        updateActionBarIcons(idx);  // 【修复】更新图标
    }
    
    m_container->setUpdatesEnabled(true);
    m_container->update();
    
    // 分批延迟更新不可见气泡
    for (int batchStart = 0; batchStart < invisibleIndices.size(); batchStart += BATCH_SIZE) {
        const int delay = 16 * (batchStart / BATCH_SIZE + 1);  // 每批延迟 16ms
        
        QTimer::singleShot(delay, this, [this, invisibleIndices, batchStart]() {
            const int batchEnd = qMin(batchStart + BATCH_SIZE, invisibleIndices.size());
            
            m_container->setUpdatesEnabled(false);
            for (int i = batchStart; i < batchEnd; ++i) {
                const int idx = invisibleIndices[i];
                if (idx >= 0 && idx < m_bubbles.size()) {
                    m_bubbles[idx]->setThemeService(m_theme);
                    m_bubbles[idx]->setMarkdownRenderer(m_renderer);
                    m_bubbles[idx]->refreshColorsLazy();
                    updateActionBarIcons(idx);  // 【修复】更新图标
                }
            }
            m_container->setUpdatesEnabled(true);
            m_container->update();
        });
    }
}

bool ChatMessageList::isBubbleVisible(ChatBubbleWidget* bubble) const
{
    if (!bubble || !bubble->isVisible()) return false;
    
    const QRect bubbleRect = getBubbleViewportRect(bubble);
    const QRect viewportRect = viewport()->rect();
    
    return viewportRect.intersects(bubbleRect);
}

QRect ChatMessageList::getBubbleViewportRect(ChatBubbleWidget* bubble) const
{
    if (!bubble) return QRect();
    
    // 获取气泡在 viewport 坐标系中的位置
    QPoint bubblePos = bubble->mapTo(viewport(), QPoint(0, 0));
    return QRect(bubblePos, bubble->size());
}

void ChatMessageList::setModel(ChatMessageListModel* model)
{
    if (m_model) m_model->disconnect(this);
    m_model = model;
    if (!m_model) return;

    connect(m_model, &QAbstractItemModel::modelReset,
            this, &ChatMessageList::rebuildAll);
    connect(m_model, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex&, int first, int last) {
                for (int r = first; r <= last; ++r) appendRow(r);
            });
    connect(m_model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& tl, const QModelIndex& br,
                   const QList<int>&) {
                for (int r = tl.row(); r <= br.row(); ++r) updateRow(r);
            });

    rebuildAll();
}

bool ChatMessageList::isAtBottom() const
{
    auto* bar = verticalScrollBar();
    return bar->value() >= bar->maximum() - 8;
}

void ChatMessageList::scrollToBottom()
{
    // 延迟到布局完成后再滚动，确保 maximum 已更新
    QTimer::singleShot(0, this, [this]() {
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    });
}

void ChatMessageList::rebuildAll()
{
    // 清空现有气泡（保留末尾 stretch）
    // 从后往前删除，避免索引变化问题
    while (m_layout->count() > 1) {
        QLayoutItem* item = m_layout->takeAt(0);
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    m_bubbles.clear();
    
    // 清理所有计时器
    for (QTimer* timer : m_elapsedTimers) {
        timer->stop();
        timer->deleteLater();
    }
    m_elapsedTimers.clear();

    if (!m_model) return;
    for (int r = 0; r < m_model->rowCount(); ++r) appendRow(r);
    scrollToBottom();
}

void ChatMessageList::appendRow(int row)
{
    if (!m_model) return;
    const ChatMessage msg = m_model->messageAt(row);

    auto* wrapper = new QWidget(m_container);
    wrapper->setAttribute(Qt::WA_StyledBackground, false);
    auto* wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->setSpacing(4);

    // 时间戳 + 状态（在气泡外上方）
    if (msg.timestamp.isValid()) {
        auto* timeWrapper = new QWidget(wrapper);
        timeWrapper->setAttribute(Qt::WA_StyledBackground, false);
        auto* timeLayout = new QHBoxLayout(timeWrapper);
        timeLayout->setContentsMargins(0, 0, 0, 0);
        timeLayout->setSpacing(8);
        
        if (msg.role == ChatMessage::User) {
            timeLayout->addStretch(1);
        } else {
            timeLayout->addSpacing(12);  // 左边距
        }
        
        // 时间标签
        auto* timeLabel = new QLabel(timeWrapper);
        timeLabel->setText(msg.timestamp.toString(QStringLiteral("hh:mm:ss")));
        timeLabel->setStyleSheet(QStringLiteral(
            "QLabel { color:#8B8B8B; font-size:11px; background:transparent; }"));
        timeLayout->addWidget(timeLabel);
        
        // 状态图标（loading gif）- 仅 AI 消息显示
        QLabel* statusIcon = nullptr;
        QLabel* statusLabel = nullptr;
        QLabel* elapsedLabel = nullptr;
        if (msg.role == ChatMessage::Assistant) {
            statusIcon = new QLabel(timeWrapper);
            statusIcon->setFixedSize(12, 12);
            statusIcon->setScaledContents(true);
            statusIcon->setVisible(false);
            statusIcon->setObjectName(QStringLiteral("statusIcon_%1").arg(row));
            timeLayout->addWidget(statusIcon);
            
            // 状态文本
            statusLabel = new QLabel(timeWrapper);
            statusLabel->setStyleSheet(QStringLiteral(
                "QLabel { color:#8B8B8B; font-size:10px; background:transparent; }"));
            statusLabel->setVisible(false);
            statusLabel->setObjectName(QStringLiteral("statusLabel_%1").arg(row));
            timeLayout->addWidget(statusLabel);
            
            // 计时器标签
            elapsedLabel = new QLabel(timeWrapper);
            elapsedLabel->setStyleSheet(QStringLiteral(
                "QLabel { color:#8B8B8B; font-size:10px; background:transparent; }"));
            elapsedLabel->setVisible(false);
            elapsedLabel->setObjectName(QStringLiteral("elapsedLabel_%1").arg(row));
            timeLayout->addWidget(elapsedLabel);
        }
        
        if (msg.role == ChatMessage::Assistant) {
            timeLayout->addStretch(1);
        } else {
            timeLayout->addSpacing(12);  // 右边距
        }
        
        wrapperLayout->addWidget(timeWrapper);
        
        // 如果有状态，立即显示
        if (msg.role == ChatMessage::Assistant && !msg.agentStatus.isEmpty()) {
            if (statusLabel) {
                statusLabel->setText(msg.agentStatus);
                statusLabel->setVisible(true);
            }
            if (statusIcon) {
                const bool isDark = !m_theme || m_theme->isDark();
                const QString iconPath = isDark 
                    ? QStringLiteral(":/icons/loading_light.gif")
                    : QStringLiteral(":/icons/loading_dark.gif");
                
                QMovie* movie = new QMovie(iconPath, QByteArray(), statusIcon);
                if (movie->isValid()) {
                    movie->setScaledSize(QSize(12, 12));
                    statusIcon->setMovie(movie);
                    movie->start();
                    statusIcon->setVisible(true);
                } else {
                    delete movie;
                }
            }
        }
        
        // 如果正在流式传输，启动计时器
        if (msg.role == ChatMessage::Assistant && msg.isStreaming && msg.startTime.isValid()) {
            if (elapsedLabel) {
                elapsedLabel->setVisible(true);
                QTimer* timer = new QTimer(this);
                m_elapsedTimers[row] = timer;
                connect(timer, &QTimer::timeout, this, [this, row]() {
                    updateElapsedTime(row);
                });
                timer->start(100);  // 每100ms更新一次
                updateElapsedTime(row);  // 立即更新一次
            }
        } else if (msg.role == ChatMessage::Assistant && msg.elapsedMs > 0) {
            // 已完成的消息，显示总耗时（在操作栏）
            // 这里不显示，留到操作栏处理
        }
    }

    // 气泡容器（水平布局控制左右对齐）
    auto* bubbleWrapper = new QWidget(wrapper);
    bubbleWrapper->setAttribute(Qt::WA_StyledBackground, false);
    auto* h = new QHBoxLayout(bubbleWrapper);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);

    auto* bubble = new ChatBubbleWidget(bubbleWrapper);
    bubble->setThemeService(m_theme);
    bubble->setMarkdownRenderer(m_renderer);
    
    // 必须先设置最大宽度，再渲染消息，否则首次 updateHtml() 会读取到
    // QWIDGETSIZE_MAX 并将 AI 气泡错误地收缩到最小宽度。
    const int horizontalMargin = 32 + 60;
    const int maxBubbleWidth = qMax(400, viewport()->width() - horizontalMargin);
    bubble->setMaximumWidth(maxBubbleWidth);
    bubble->setMessage(msg);
    
    connect(bubble, &ChatBubbleWidget::linkActivated,
            this, &ChatMessageList::linkActivated);
    connect(bubble, &ChatBubbleWidget::copyRequested,
            this, &ChatMessageList::copyRequested);
    connect(bubble, &ChatBubbleWidget::regenerateRequested,
            this, &ChatMessageList::regenerateRequested);

    // 用户消息靠右，AI消息靠左，添加左右边距
    if (msg.role == ChatMessage::User) {
        h->addStretch(1);
        h->addWidget(bubble);
        h->addSpacing(12);  // 右边距
    } else {
        h->addSpacing(12);  // 左边距
        h->addWidget(bubble);
        h->addStretch(1);
    }

    wrapperLayout->addWidget(bubbleWrapper);

    // 操作栏（在气泡外下方）
    auto* actionBar = new QWidget(wrapper);
    actionBar->setAttribute(Qt::WA_StyledBackground, false);
    auto* actionLayout = new QHBoxLayout(actionBar);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(8);

    // 根据主题选择图标颜色（修正：浅色模式用 dark 图标，暗色模式用 light 图标）
    const bool isDark = !m_theme || m_theme->isDark();
    const QString iconSuffix = isDark ? QStringLiteral("_light.png") : QStringLiteral("_dark.png");

    auto* copyButton = new QToolButton(actionBar);
    copyButton->setIcon(QIcon(QStringLiteral(":/icons/copy") + iconSuffix));
    copyButton->setIconSize(QSize(16, 16));  // 缩小图标尺寸
    copyButton->setToolTip(tr("复制"));
    copyButton->setCursor(Qt::PointingHandCursor);
    copyButton->setFixedSize(28, 28);  // 缩小按钮尺寸
    copyButton->setStyleSheet(QStringLiteral(
        "QToolButton { background:transparent; border:none; border-radius:6px; }"
        "QToolButton:hover { background:rgba(128, 128, 128, 0.2); }"));
    connect(copyButton, &QToolButton::clicked, [this, msg]() {
        QGuiApplication::clipboard()->setText(msg.content);
        emit copyRequested(msg.content);
    });

    auto* regenerateButton = new QToolButton(actionBar);
    regenerateButton->setIcon(QIcon(QStringLiteral(":/icons/replay") + iconSuffix));
    regenerateButton->setIconSize(QSize(16, 16));  // 缩小图标尺寸
    regenerateButton->setToolTip(tr("重新生成"));
    regenerateButton->setCursor(Qt::PointingHandCursor);
    regenerateButton->setFixedSize(28, 28);  // 缩小按钮尺寸
    regenerateButton->setStyleSheet(QStringLiteral(
        "QToolButton { background:transparent; border:none; border-radius:6px; }"
        "QToolButton:hover { background:rgba(128, 128, 128, 0.2); }"));
    regenerateButton->setVisible(msg.role == ChatMessage::Assistant);
    connect(regenerateButton, &QToolButton::clicked,
            this, &ChatMessageList::regenerateRequested);

    if (msg.role == ChatMessage::User) {
        actionLayout->addStretch(1);
    } else {
        actionLayout->addSpacing(12);  // AI 消息左边距
    }
    actionLayout->addWidget(copyButton);
    actionLayout->addWidget(regenerateButton);
    
    // 如果有耗时数据，显示在操作栏右侧
    if (msg.role == ChatMessage::Assistant && msg.elapsedMs > 0) {
        auto* elapsedLabel = new QLabel(actionBar);
        const double seconds = msg.elapsedMs / 1000.0;
        
        // 根据主题选择timer图标
        const QString timerIcon = isDark 
            ? QStringLiteral(":/icons/timer_light.png")
            : QStringLiteral(":/icons/timer_dark.png");
        
        elapsedLabel->setPixmap(QPixmap(timerIcon).scaled(12, 12, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        elapsedLabel->setStyleSheet(QStringLiteral("QLabel { background:transparent; }"));
        actionLayout->addWidget(elapsedLabel);
        
        auto* elapsedText = new QLabel(actionBar);
        elapsedText->setText(QString::fromUtf8("%1秒").arg(seconds, 0, 'f', 1));
        elapsedText->setStyleSheet(QStringLiteral(
            "QLabel { color:#8B8B8B; font-size:10px; background:transparent; }"));
        elapsedText->setObjectName(QStringLiteral("finalElapsed_%1").arg(row));
        actionLayout->addWidget(elapsedText);
    }
    
    if (msg.role == ChatMessage::Assistant) {
        actionLayout->addStretch(1);
    } else {
        actionLayout->addSpacing(12);  // 用户消息右边距
    }

    wrapperLayout->addWidget(actionBar);

    const bool atBottom = isAtBottom();
    m_layout->insertWidget(m_layout->count() - 1, wrapper);
    m_bubbles.append(bubble);

    if (atBottom) scrollToBottom();
}

void ChatMessageList::updateRow(int row)
{
    if (row < 0 || row >= m_bubbles.size() || !m_model) return;
    const bool atBottom = isAtBottom();
    const ChatMessage msg = m_model->messageAt(row);
    
    // 更新气泡内容
    m_bubbles[row]->updateContent(msg.content);
    
    // 更新状态显示（在时间戳行中查找状态标签和图标）
    if (msg.role == ChatMessage::Assistant) {
        // 获取 wrapper widget（气泡的父容器的父容器）
        QWidget* bubbleParent = m_bubbles[row]->parentWidget();
        if (bubbleParent) {
            QWidget* wrapper = bubbleParent->parentWidget();
            if (wrapper) {
                // 查找状态图标和标签
                QLabel* statusIcon = wrapper->findChild<QLabel*>(
                    QStringLiteral("statusIcon_%1").arg(row));
                QLabel* statusLabel = wrapper->findChild<QLabel*>(
                    QStringLiteral("statusLabel_%1").arg(row));
                QLabel* elapsedLabel = wrapper->findChild<QLabel*>(
                    QStringLiteral("elapsedLabel_%1").arg(row));
                
                if (statusLabel && statusIcon) {
                    if (msg.agentStatus.isEmpty()) {
                        statusLabel->setVisible(false);
                        statusIcon->setVisible(false);
                    } else {
                        statusLabel->setText(msg.agentStatus);
                        statusLabel->setVisible(true);
                        
                        // 更新 loading 图标
                        const bool isDark = !m_theme || m_theme->isDark();
                        const QString iconPath = isDark 
                            ? QStringLiteral(":/icons/loading_light.gif")
                            : QStringLiteral(":/icons/loading_dark.gif");
                        
                        QMovie* movie = new QMovie(iconPath, QByteArray(), statusIcon);
                        if (movie->isValid()) {
                            movie->setScaledSize(QSize(12, 12));
                            statusIcon->setMovie(movie);
                            movie->start();
                            statusIcon->setVisible(true);
                        } else {
                            delete movie;
                            statusIcon->setVisible(false);
                        }
                    }
                }
                
                // 处理计时器
                if (!msg.isStreaming) {
                    // 停止计时器
                    if (m_elapsedTimers.contains(row)) {
                        m_elapsedTimers[row]->stop();
                        m_elapsedTimers[row]->deleteLater();
                        m_elapsedTimers.remove(row);
                    }
                    if (elapsedLabel) {
                        elapsedLabel->setVisible(false);
                    }
                    
                    // 更新操作栏的最终耗时
                    if (msg.elapsedMs > 0) {
                        QLabel* finalLabel = wrapper->findChild<QLabel*>(
                            QStringLiteral("finalElapsed_%1").arg(row));
                        if (finalLabel) {
                            const double seconds = msg.elapsedMs / 1000.0;
                            finalLabel->setText(QString::fromUtf8("⏱ %1秒").arg(seconds, 0, 'f', 1));
                            finalLabel->setVisible(true);
                        } else {
                            // 标签不存在，需要动态创建（在操作栏中）
                            QWidget* actionBar = nullptr;
                            const QList<QWidget*> widgets = wrapper->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
                            for (QWidget* w : widgets) {
                                if (w->layout() && w->layout()->count() > 0) {
                                    // 检查是否包含复制按钮（操作栏的标志）
                                    QHBoxLayout* hbox = qobject_cast<QHBoxLayout*>(w->layout());
                                    if (hbox) {
                                        for (int i = 0; i < hbox->count(); ++i) {
                                            QToolButton* btn = qobject_cast<QToolButton*>(hbox->itemAt(i)->widget());
                                            if (btn && btn->toolTip().contains(QString::fromUtf8("复制"))) {
                                                actionBar = w;
                                                break;
                                            }
                                        }
                                    }
                                }
                                if (actionBar) break;
                            }
                            
                            if (actionBar && actionBar->layout()) {
                                QHBoxLayout* actionLayout = qobject_cast<QHBoxLayout*>(actionBar->layout());
                                if (actionLayout) {
                                    // 在最后一个stretch之前插入耗时图标和文本
                                    const bool isDark = !m_theme || m_theme->isDark();
                                    const QString timerIcon = isDark 
                                        ? QStringLiteral(":/icons/timer_light.png")
                                        : QStringLiteral(":/icons/timer_dark.png");
                                    
                                    auto* iconLabel = new QLabel(actionBar);
                                    iconLabel->setPixmap(QPixmap(timerIcon).scaled(12, 12, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                                    iconLabel->setStyleSheet(QStringLiteral("QLabel { background:transparent; }"));
                                    iconLabel->setObjectName(QStringLiteral("finalElapsedIcon_%1").arg(row));
                                    actionLayout->insertWidget(actionLayout->count() - 1, iconLabel);
                                    
                                    auto* newElapsedLabel = new QLabel(actionBar);
                                    const double seconds = msg.elapsedMs / 1000.0;
                                    newElapsedLabel->setText(QString::fromUtf8("%1秒").arg(seconds, 0, 'f', 1));
                                    newElapsedLabel->setStyleSheet(QStringLiteral(
                                        "QLabel { color:#8B8B8B; font-size:10px; background:transparent; }"));
                                    newElapsedLabel->setObjectName(QStringLiteral("finalElapsed_%1").arg(row));
                                    actionLayout->insertWidget(actionLayout->count() - 1, newElapsedLabel);
                                }
                            }
                        }
                    }
                } else if (msg.startTime.isValid() && !m_elapsedTimers.contains(row)) {
                    // 启动计时器
                    if (elapsedLabel) {
                        elapsedLabel->setVisible(true);
                        QTimer* timer = new QTimer(this);
                        m_elapsedTimers[row] = timer;
                        connect(timer, &QTimer::timeout, this, [this, row]() {
                            updateElapsedTime(row);
                        });
                        timer->start(1000);
                        updateElapsedTime(row);
                    }
                }
            }
        }
    }
    
    if (atBottom) scrollToBottom();
}

void ChatMessageList::updateElapsedTime(int row)
{
    if (row < 0 || row >= m_bubbles.size() || !m_model) return;
    
    const ChatMessage msg = m_model->messageAt(row);
    if (!msg.startTime.isValid()) return;
    
    const qint64 elapsed = msg.startTime.msecsTo(QDateTime::currentDateTime());
    const int totalSeconds = static_cast<int>(elapsed / 1000);
    
    QString timeText;
    if (totalSeconds < 60) {
        // 小于1分钟，显示秒数（不带小数）
        timeText = QString::fromUtf8("(%1秒)").arg(totalSeconds);
    } else {
        // 大于1分钟，显示 XmYs 格式
        const int minutes = totalSeconds / 60;
        const int seconds = totalSeconds % 60;
        timeText = QString::fromUtf8("(%1m%2s)").arg(minutes).arg(seconds);
    }
    
    QWidget* bubbleParent = m_bubbles[row]->parentWidget();
    if (bubbleParent) {
        QWidget* wrapper = bubbleParent->parentWidget();
        if (wrapper) {
            QLabel* elapsedLabel = wrapper->findChild<QLabel*>(
                QStringLiteral("elapsedLabel_%1").arg(row));
            if (elapsedLabel) {
                elapsedLabel->setText(timeText);
            }
        }
    }
}

void ChatMessageList::resizeEvent(QResizeEvent* event)
{
    QScrollArea::resizeEvent(event);
    
    // 当容器大小改变时，更新所有气泡的最大宽度
    const int horizontalMargin = 32 + 60;
    const int maxBubbleWidth = qMax(400, viewport()->width() - horizontalMargin);
    
    for (ChatBubbleWidget* bubble : std::as_const(m_bubbles)) {
        bubble->setMaximumWidth(maxBubbleWidth);
        bubble->refreshLayout();
    }
}

void ChatMessageList::showEvent(QShowEvent* event)
{
    QScrollArea::showEvent(event);
    
    // 【性能优化】当视图显示时，如果主题变过但没刷新，现在刷新
    if (m_themeDirty) {
        refreshBubbleColorsProgressive();
    }
}

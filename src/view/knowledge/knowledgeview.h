#ifndef FRAMEMIND_KNOWLEDGEVIEW_H
#define FRAMEMIND_KNOWLEDGEVIEW_H

#include <QWidget>
#include <QVector>
#include "viewmodel/knowledgeviewmodel.h"

class QScrollArea;
class QVBoxLayout;
class QStackedWidget;
class QLabel;
class QToolButton;
class QLineEdit;
class QSplitter;
class ThemeService;
class ChunkBrowserWidget;
class SearchPreviewWidget;
class VideoIndexCard;

/**
 * 知识库页面（MainWindow pageStack index 2）。
 *
 * 布局：
 *   左栏 (260px)  — 已索引视频列表，搜索框 + 清理按钮
 *   右侧主区      — SegmentedControl (知识详情 / 检索测试)
 *                    Tab 0: ChunkBrowserWidget
 *                    Tab 1: SearchPreviewWidget
 *
 * 无视频被选中时右侧显示空状态提示。
 */
class KnowledgeView : public QWidget {
    Q_OBJECT
public:
    explicit KnowledgeView(QWidget* parent = nullptr);

    void setViewModel(KnowledgeViewModel* vm);
    void setThemeService(ThemeService* theme);

    /// 外部触发刷新（如从文件页导航过来时）
    void refresh();

private slots:
    void onIndexedVideosChanged(const QVector<KnowledgeViewModel::VideoIndexSummary>& videos);
    void onVideoIndexRemoved(const QString& videoId);
    void onStatusMessage(const QString& msg, bool isError);
    void onStaleIndexCleaned(int count);
    void onCleanupRequested();
    void onThemeChanged();

private:
    void buildLeftPanel(QWidget* parent);
    void buildRightPanel(QWidget* parent);
    void updateEmptyState();
    void applyTheme();
    void updateListFilter(const QString& filterText);

    KnowledgeViewModel* m_vm    = nullptr;
    ThemeService*       m_theme = nullptr;

    // Left panel
    QScrollArea*  m_videoListArea  = nullptr;
    QVBoxLayout*  m_videoListLayout= nullptr;
    QWidget*      m_videoListHost  = nullptr;
    QLineEdit*    m_filterEdit     = nullptr;
    QLabel*       m_videosCountLabel = nullptr;
    QToolButton*  m_cleanupButton  = nullptr;
    QToolButton*  m_refreshButton  = nullptr;

    // Right panel
    QStackedWidget* m_rightStack   = nullptr;
    QWidget*        m_emptyState   = nullptr;
    QWidget*        m_contentWidget= nullptr;
    QStackedWidget* m_tabStack     = nullptr;

    // Tab buttons (manual segmented control)
    QToolButton*    m_detailTabBtn = nullptr;
    QToolButton*    m_searchTabBtn = nullptr;

    // Right panel video title
    QLabel*         m_videoTitleLabel = nullptr;

    ChunkBrowserWidget*  m_chunkBrowser  = nullptr;
    SearchPreviewWidget* m_searchPreview = nullptr;

    // Status bar
    QLabel*         m_statusBar    = nullptr;

    QVector<VideoIndexCard*> m_cards;
    QString m_selectedVideoId;
};

#endif // FRAMEMIND_KNOWLEDGEVIEW_H

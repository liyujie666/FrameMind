#ifndef FRAMEMIND_SUMMARYTABWIDGET_H
#define FRAMEMIND_SUMMARYTABWIDGET_H

#include <QWidget>

class ThemeService;
class VideoAnalysisViewModel;
class QLabel;
class QProgressBar;
class QScrollArea;
class QTextBrowser;
class QVBoxLayout;

/**
 * 总结 Tab：展示全视频 AI 摘要 + 索引进度。
 *
 * 状态机：
 *   - 未打开视频：空状态提示
 *   - 正在索引（Level 0/1/2）：进度条 + 阶段文字
 *   - 摘要就绪：折叠进度区域，显示摘要文本
 *
 * 同时在摘要下方列出各场景的 VLM 描述（随描述完成逐行追加）。
 */
class SummaryTabWidget : public QWidget {
    Q_OBJECT
public:
    explicit SummaryTabWidget(QWidget* parent = nullptr);

    void setThemeService(ThemeService* theme);
    void setViewModel(VideoAnalysisViewModel* vm);

private slots:
    void onProgressChanged(int percent, const QString& label);
    void onIndexingChanged(bool isIndexing);
    void onSummaryReady(const QString& summary);
    void onSceneDescribed(int sceneId, const QString& description);
    void onThemeChanged();
    void resetContent();

private:
    void applyStyles();
    void applyScrollStyle();
    void addSceneDescEntry(int sceneId, const QString& description);

    ThemeService*            m_theme     = nullptr;
    VideoAnalysisViewModel*  m_vm        = nullptr;

    // 进度区域
    QWidget*      m_progressArea  = nullptr;
    QProgressBar* m_progressBar   = nullptr;
    QLabel*       m_progressLabel = nullptr;

    // 摘要区域
    QScrollArea*  m_scroll        = nullptr;
    QWidget*      m_scrollContent = nullptr;
    QVBoxLayout*  m_contentLayout = nullptr;
    QLabel*       m_emptyLabel    = nullptr;
    QTextBrowser* m_summaryBrowser = nullptr;
    QWidget*      m_scenesSection = nullptr;
    QVBoxLayout*  m_scenesLayout  = nullptr;

    int m_renderedSceneCount = 0;
};

#endif // FRAMEMIND_SUMMARYTABWIDGET_H

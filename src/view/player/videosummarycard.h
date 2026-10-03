#ifndef FRAMEMIND_VIDEOSUMMARYCARD_H
#define FRAMEMIND_VIDEOSUMMARYCARD_H

#include <QString>
#include <QWidget>

class QLabel;
class QTextBrowser;
class QToolButton;
class ThemeService;

/// 视频概览阅读卡片：原生标题栏 + 随内容展开的纯文本正文。
class VideoSummaryCard : public QWidget {
    Q_OBJECT
public:
    explicit VideoSummaryCard(QWidget* parent = nullptr);
    ~VideoSummaryCard() override;

    void setThemeService(ThemeService* theme);
    void setSummary(const QString& summary);
    void clear();

private:
    void applyTheme();
    void renderSummary();

    ThemeService* m_theme = nullptr;
    QString m_summary;
    QString m_displayBody;
    QLabel* m_title = nullptr;
    QToolButton* m_copyButton = nullptr;
    QTextBrowser* m_browser = nullptr;
};

#endif // FRAMEMIND_VIDEOSUMMARYCARD_H

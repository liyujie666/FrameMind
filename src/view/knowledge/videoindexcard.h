#ifndef FRAMEMIND_VIDEOINDEXCARD_H
#define FRAMEMIND_VIDEOINDEXCARD_H

#include <QWidget>
#include "viewmodel/knowledgeviewmodel.h"

class QLabel;
class QToolButton;
class ThemeService;

/**
 * 已索引视频的卡片 widget。
 *
 * 展示：文件名 / 索引级别标签（L0/L1/L2）/ chunk 统计 / 删除按钮。
 * 点击整体区域发出 selected 信号；删除按钮发出 removeRequested 信号。
 */
class VideoIndexCard : public QWidget {
    Q_OBJECT
public:
    explicit VideoIndexCard(const KnowledgeViewModel::VideoIndexSummary& summary,
                            QWidget* parent = nullptr);

    void setThemeService(ThemeService* theme);
    void setSelected(bool selected);
    void updateSummary(const KnowledgeViewModel::VideoIndexSummary& summary);

    QString videoId() const { return m_summary.videoId; }

signals:
    void selected(const QString& videoId);
    void removeRequested(const QString& videoId);
    void openInExplorerRequested(const QString& filePath);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void applyTheme();
    QString levelText(int level) const;
    QColor  levelColor(int level) const;

    KnowledgeViewModel::VideoIndexSummary m_summary;
    ThemeService* m_theme    = nullptr;
    bool          m_selected = false;
    bool          m_hovered  = false;

    QLabel*      m_nameLabel    = nullptr;
    QLabel*      m_levelBadge   = nullptr;
    QLabel*      m_statsLabel   = nullptr;
    QLabel*      m_dateLabel    = nullptr;
    QToolButton* m_deleteButton = nullptr;
};

#endif // FRAMEMIND_VIDEOINDEXCARD_H

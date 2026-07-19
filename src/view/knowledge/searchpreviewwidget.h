#ifndef FRAMEMIND_SEARCHPREVIEWWIDGET_H
#define FRAMEMIND_SEARCHPREVIEWWIDGET_H

#include <QWidget>
#include <QVector>
#include "model/retrieval_result.h"

class QLineEdit;
class QToolButton;
class QListWidget;
class QLabel;
class ThemeService;
class KnowledgeViewModel;

/**
 * 检索测试面板。
 *
 * 在当前选中视频的知识库中执行 RAG 检索，展示召回结果：
 *   相关度分 / 命中路径 / 时间区间 / 内容摘要。
 * 主要用途：验证 RAG 效果 + 让用户理解知识库内容。
 */
class SearchPreviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit SearchPreviewWidget(QWidget* parent = nullptr);

    void setThemeService(ThemeService* theme);
    void setViewModel(KnowledgeViewModel* vm);

private slots:
    void onSearchClicked();
    void onSearchResultsReady(const QVector<RetrievalResult>& results);
    void onSearchingChanged(bool searching);
    void applyTheme();

private:
    void buildResultItem(const RetrievalResult& r);
    static QString hitPathLabel(const QString& path);
    static QString hitPathColor(const QString& path);
    static QString formatMs(int64_t ms);

    ThemeService*        m_theme = nullptr;
    KnowledgeViewModel*  m_vm    = nullptr;

    QLineEdit*   m_queryEdit    = nullptr;
    QToolButton* m_searchButton = nullptr;
    QLabel*      m_statusLabel  = nullptr;
    QListWidget* m_resultList   = nullptr;
};

#endif // FRAMEMIND_SEARCHPREVIEWWIDGET_H

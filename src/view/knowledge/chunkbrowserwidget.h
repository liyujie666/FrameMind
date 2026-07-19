#ifndef FRAMEMIND_CHUNKBROWSERWIDGET_H
#define FRAMEMIND_CHUNKBROWSERWIDGET_H

#include <QWidget>
#include <QVector>
#include "model/retrieval_result.h"

class QListWidget;
class QComboBox;
class QLabel;
class ThemeService;
class KnowledgeViewModel;

/**
 * Chunk 浏览器。
 *
 * 顶部：集合选择下拉框 + chunk 数量提示。
 * 主体：QListWidget，每行显示一个 VideoChunk 的时间戳、类型、内容摘要。
 * 支持主题切换。
 */
class ChunkBrowserWidget : public QWidget {
    Q_OBJECT
public:
    explicit ChunkBrowserWidget(QWidget* parent = nullptr);

    void setThemeService(ThemeService* theme);
    void setViewModel(KnowledgeViewModel* vm);

private slots:
    void onChunksChanged(const QVector<VideoChunk>& chunks);
    void onCollectionChanged(int index);
    void applyTheme();

private:
    void buildItem(const VideoChunk& chunk);
    static QString chunkTypeName(VideoChunk::ChunkType t);
    static QString chunkTypeColor(VideoChunk::ChunkType t);
    static QString formatMs(int64_t ms);

    ThemeService*        m_theme = nullptr;
    KnowledgeViewModel*  m_vm    = nullptr;

    QComboBox*   m_collectionCombo = nullptr;
    QLabel*      m_countLabel      = nullptr;
    QListWidget* m_list            = nullptr;
};

#endif // FRAMEMIND_CHUNKBROWSERWIDGET_H

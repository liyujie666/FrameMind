#pragma once
#include <QWidget>
#include <QPointer>
#include <QHash>
#include "model/video_presentation_types.h"

class ThemeService;
class QVBoxLayout;
class QPushButton;

// Both regions use the same plain-text entry renderer; their contracts and
// states remain independent. Requests carry identity only, never generated text.
class VideoContentSectionWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoContentSectionWidget(QWidget* parent = nullptr);
    void setThemeService(ThemeService*);
    void setSnapshot(const QString& videoId, const QString& buildId, const VideoPresentation&);
    void setQuestionBusy(bool);
    void setIndexReady(bool);
    void clear();
signals:
    void reviewRequested(const QString& videoId, const QString& buildId, const QString& anchorId);
    void questionRequested(const QString& videoId, const QString& buildId, const QString& questionId);
protected:
    VideoContentSectionWidget(bool explore, QWidget* parent);
private:
    void render();
    void applyTheme();
    bool m_explore = false, m_busy = false;
    bool m_indexReady = true;
    quint64 m_renderGeneration = 0;
    QString m_videoId, m_buildId;
    VideoPresentation m_presentation;
    QPointer<ThemeService> m_theme;
    QVBoxLayout* m_layout = nullptr;
    QVector<QPointer<QPushButton>> m_questions;
    QSet<QString> m_expandedEntries;
    bool m_expansionInitialized = false;
    QHash<QString, QWidget*> m_rowsById;
    QHash<QString, QByteArray> m_rowPayloads;
};

class VideoExploreSectionWidget final : public VideoContentSectionWidget {
    Q_OBJECT
public:
    explicit VideoExploreSectionWidget(QWidget* parent = nullptr) : VideoContentSectionWidget(true, parent) {}
};

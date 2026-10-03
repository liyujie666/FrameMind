#ifndef FRAMEMIND_SUMMARYTABWIDGET_H
#define FRAMEMIND_SUMMARYTABWIDGET_H

#include <QWidget>
#include <QPointer>
#include "model/video_presentation_types.h"

class ThemeService;
class VideoAnalysisViewModel;
class QLabel;
class QScrollArea;
class VideoSummaryCard;
class QVBoxLayout;
class QComboBox;
class QToolButton;
class QGridLayout;
class QResizeEvent;
class VideoContentSectionWidget;
class VideoExploreSectionWidget;

class SummaryTabWidget : public QWidget {
    Q_OBJECT
public:
    explicit SummaryTabWidget(QWidget* parent = nullptr);

    void setThemeService(ThemeService* theme);
    void setViewModel(VideoAnalysisViewModel* vm);
    void setQuestionBusy(bool);
    void showInteractionMessage(const QString&);
    void refreshSnapshot();
signals:
    void reviewRequested(const QString& videoId, const QString& buildId, const QString& anchorId);
    void questionRequested(const QString& videoId, const QString& buildId, const QString& questionId);

private slots:
    void onIndexingChanged(bool isIndexing);
    void onSummaryReady(const QString& summary);
    void onThemeChanged();
    void resetContent();

protected:
    void resizeEvent(QResizeEvent*) override;

private:
    void applyStyles();
    void applyBuildStateStyle();
    void applyScrollStyle();
    void syncProfileAndState();
    void renderPresentation(const QString&, const QString&, const VideoPresentation&);
    void updateOverviewIdentity();
    void layoutControls(bool compact);
    void preserveReadingPosition();

    ThemeService*            m_theme     = nullptr;
    QPointer<VideoAnalysisViewModel> m_vm;

    // 摘要区域
    QScrollArea*  m_scroll        = nullptr;
    QWidget*      m_scrollContent = nullptr;
    QVBoxLayout*  m_contentLayout = nullptr;
    QLabel*       m_emptyLabel    = nullptr;
    VideoSummaryCard* m_summaryCard = nullptr;
    QLabel*       m_previousOverviewLabel = nullptr;
    VideoContentSectionWidget* m_contentSection = nullptr;
    VideoExploreSectionWidget* m_exploreSection = nullptr;
    QLabel* m_interactionMessage = nullptr;
    QComboBox* m_typeSelector=nullptr;
    QLabel* m_buildState=nullptr;
    QLabel* m_typeBadge = nullptr;
    QToolButton* m_rebuildButton = nullptr;
    QToolButton* m_cancelButton = nullptr;
    QWidget* m_controlsArea = nullptr;
    QGridLayout* m_controlsLayout = nullptr;
    bool m_compactControls = false;
    quint64 m_refreshRevision = 0;

    bool m_questionBusy = false;
};

#endif // FRAMEMIND_SUMMARYTABWIDGET_H

#include "viewmodel/videoanalysisviewmodel.h"

#include "service/agent/video_analysis_service.h"
#include "service/agent/video_indexer.h"
#include <QPointer>
#include <QTimer>
#include "util/video_file_identity.h"

namespace {
bool sameScenes(const QVector<Scene>& a, const QVector<Scene>& b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].startMs != b[i].startMs || a[i].endMs != b[i].endMs ||
            a[i].keyframePath != b[i].keyframePath || a[i].description != b[i].description ||
            a[i].fusedDescription != b[i].fusedDescription) return false;
    return true;
}
bool sameSpeech(const QVector<SpeechSegment>& a, const QVector<SpeechSegment>& b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (a[i].startMs != b[i].startMs || a[i].endMs != b[i].endMs || a[i].text != b[i].text) return false;
    return true;
}
}

VideoAnalysisViewModel::VideoAnalysisViewModel(VideoAnalysisService* analysisService,
                                               VideoIndexer*         indexer,
                                               QObject*              parent)
    : QObject(parent)
    , m_analysis(analysisService)
    , m_indexer(indexer)
{
    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true); m_previewTimer->setInterval(75);
    connect(m_previewTimer, &QTimer::timeout, this, &VideoAnalysisViewModel::flushPreview);
    connectServices();
}

void VideoAnalysisViewModel::connectServices()
{
    if (!m_analysis) return;
    connect(m_analysis, &VideoAnalysisService::buildStarted, this,
            &VideoAnalysisViewModel::startRun);
    connect(m_analysis, &VideoAnalysisService::buildProgress, this,
            [this](const VideoBuildContext &context, int percent, const QString &label) {
        if (!matchesRun(context)) return;
        m_indexPercent = percent;
        m_indexStageLabel = label;
        emit progressChanged(percent, label);
    });
    connect(m_analysis, &VideoAnalysisService::buildTerminated, this,
            [this](const VideoBuildContext &context, const VideoBuildManifest &result) {
        if (!matchesRun(context)) return;
        QPointer<VideoAnalysisViewModel> terminalGuard(this);
        flushPreview();
        if (!terminalGuard || !matchesRun(context)) return;
        if (isPreview()) {
            if (m_activeRepresentation) {
                applyRepresentation(*m_activeRepresentation);
            } else {
                auto snapshot = *m_repr;
                snapshot.build.state = result.state;
                snapshot.build.diagnostics = result.diagnostics;
                auto& p = snapshot.build.presentation;
                if (p.chaptersState == ArtifactState::Running || p.chaptersState == ArtifactState::Pending)
                    p.chaptersState = p.chapters.isEmpty() ? result.state : ArtifactState::Partial;
                if (snapshot.build.overviewState == ArtifactState::Running || snapshot.build.overviewState == ArtifactState::Pending)
                    snapshot.build.overviewState = result.state;
                for (auto* section : {&p.primarySection, &p.secondarySection})
                    if (section->state == ArtifactState::Running || section->state == ArtifactState::Pending)
                        section->state = !section->entries.isEmpty() || !section->questions.isEmpty()
                            ? ArtifactState::Partial : result.state;
                applyRepresentation(snapshot);
            }
            if (!terminalGuard || !matchesRun(context)) return;
        }
        if ((result.state == ArtifactState::Ready || result.state == ArtifactState::Partial) &&
            result.buildId == displayedBuildId()) m_overviewTargetBuildId = result.buildId;
        m_buildState = result;
        m_runningBuild.reset();
        m_isIndexing = false;
        QPointer<VideoAnalysisViewModel> guard(this);
        emit indexingChanged(false);
        if (guard && m_lastGeneration == context.taskGeneration && !m_runningBuild) emit buildStateChanged(result);
    });
    connect(m_analysis, &VideoAnalysisService::buildProfileReady, this,
            [this](const VideoBuildContext &context, const VideoContentProfile &profile) {
        if (!matchesRun(context)) return;
        m_profile = profile;
        m_buildState.profile = profile;
        emit contentProfileReady(profile);
    });
    connect(m_analysis, &VideoAnalysisService::representationReady, this,
            [this](const VideoBuildContext &context, const VideoRepresentation &representation) {
        if (!matchesRun(context) || representation.metadata.filePath != m_currentPath ||
            (representation.build.buildId != context.buildId &&
             representation.build.buildId != context.expectedActiveBuildId)) return;
        m_previewTimer->stop(); m_pendingPreview.reset(); m_previewContext.reset();
        m_activeRepresentation = QSharedPointer<VideoRepresentation>::create(representation);
        applyRepresentation(representation);
    });
    connect(m_analysis, &VideoAnalysisService::previewReady, this, &VideoAnalysisViewModel::queuePreview);
}

void VideoAnalysisViewModel::queuePreview(const VideoBuildContext& context, const VideoRepresentation& snapshot) {
    if (!matchesRun(context) || context.isCancelled() || snapshot.build.buildId != context.buildId ||
        snapshot.metadata.filePath != m_currentPath || !snapshot.isValid()) return;
    m_pendingPreview = QSharedPointer<VideoRepresentation>::create(snapshot);
    m_previewContext = context;
    if (!m_previewTimer->isActive()) m_previewTimer->start();
}

void VideoAnalysisViewModel::flushPreview() {
    m_previewTimer->stop();
    auto snapshot = m_pendingPreview;
    const auto context = m_previewContext;
    m_pendingPreview.reset(); m_previewContext.reset();
    if (!snapshot || !context || !matchesRun(*context)) return;
    // Existing complete content stays readable until new chapters/overview are
    // available; raw subtitles can already refresh independently.
    if (m_activeRepresentation && !isPreview() && snapshot->build.presentation.chapters.isEmpty()
        && snapshot->videoSummary.isEmpty()) {
        m_speechSegments = snapshot->speechSegments;
        emit speechSegmentsReady(m_speechSegments);
        return;
    }
    applyRepresentation(*snapshot);
}

bool VideoAnalysisViewModel::matchesRun(const VideoBuildContext &context) const {
    return m_runningBuild && context.filePath == m_currentPath &&
        context.buildId == m_runningBuild->buildId &&
        context.taskGeneration == m_runningBuild->taskGeneration;
}

void VideoAnalysisViewModel::startRun(const VideoBuildContext &context) {
    if (context.filePath != m_currentPath || context.buildId.isEmpty() || matchesRun(context) ||
        context.taskGeneration < m_lastGeneration) return;
    // Reopening a video can adopt the same still-running task after temporarily
    // displaying another video. An equal generation is valid only in that case.
    if (context.taskGeneration == m_lastGeneration) {
        const auto active = m_analysis ? m_analysis->runningBuildContext() : std::nullopt;
        if (!active || active->buildId != context.buildId ||
            active->taskGeneration != context.taskGeneration || context.isCancelled()) return;
    }
    m_lastGeneration = context.taskGeneration;
    m_runningBuild = context;
    m_previewTimer->stop(); m_pendingPreview.reset(); m_previewContext.reset();
    m_overviewTargetBuildId = context.buildId;
    m_isIndexing = true;
    m_indexPercent = m_analysis ? m_analysis->runningBuildPercent() : 0;
    m_indexStageLabel = m_analysis ? m_analysis->runningBuildStage() : QString{};
    if (m_indexStageLabel.isEmpty()) m_indexStageLabel = tr("准备构建...");
    m_buildState = {};
    m_buildState.buildId = context.buildId; m_buildState.videoId = context.videoId;
    if (m_analysis && m_analysis->runningBuildProfile()) m_profile = *m_analysis->runningBuildProfile();
    m_buildState.profile = m_profile; m_buildState.state = ArtifactState::Running;
    QPointer<VideoAnalysisViewModel> guard(this);
    emit progressChanged(m_indexPercent, m_indexStageLabel);
    if (!guard || !matchesRun(context)) return;
    emit contentProfileReady(m_profile);
    if (guard && matchesRun(context)) emit indexingChanged(true);
    if (!guard || !matchesRun(context)) return;
    const auto preview = m_analysis ? m_analysis->buildPreview() : nullptr;
    if (preview && preview->build.buildId == context.buildId) queuePreview(context, *preview);
}

void VideoAnalysisViewModel::applyRepresentation(const VideoRepresentation &representation) {
    const auto previous = m_repr;
    m_repr = QSharedPointer<VideoRepresentation>::create(representation);
    m_scenes = representation.scenes;
    m_speechSegments = representation.speechSegments;
    m_videoSummary = representation.videoSummary;
    m_profile = representation.build.profile;
    if (!m_runningBuild || m_runningBuild->buildId == representation.build.buildId) m_buildState = representation.build;
    QPointer<VideoAnalysisViewModel> guard(this);
    const auto snapshot = m_repr;
    const auto stillDisplayed = [guard, snapshot] { return guard && guard->m_repr == snapshot; };
    if (!previous || previous->videoId != snapshot->videoId || !sameScenes(previous->scenes, snapshot->scenes) ||
        previous->sceneDescriptions != snapshot->sceneDescriptions || previous->build.buildId != snapshot->build.buildId)
        emit scenesReady(snapshot->scenes);
    if (!stillDisplayed()) return;
    if (!previous || previous->videoId != snapshot->videoId || !sameSpeech(previous->speechSegments, snapshot->speechSegments) ||
        previous->build.buildId != snapshot->build.buildId)
        emit speechSegmentsReady(snapshot->speechSegments);
    if (!stillDisplayed()) return;
    emit contentProfileReady(snapshot->build.profile);
    if (!stillDisplayed()) return;
    emit semanticUnitsReady(snapshot->semanticUnits);
    if (!stillDisplayed()) return;
    if (!previous || previous->build.buildId != snapshot->build.buildId ||
        previous->build.presentation.toJson() != snapshot->build.presentation.toJson())
        emit presentationReady(snapshot->videoId, snapshot->build.buildId, snapshot->build.presentation);
    if (!stillDisplayed()) return;
    if (!previous || previous->build.buildId != snapshot->build.buildId || previous->videoSummary != snapshot->videoSummary)
        emit summaryReady(snapshot->videoSummary);
}

void VideoAnalysisViewModel::onVideoOpened(const QString& videoPath)
{
    if (m_currentPath == videoPath) return;
    m_currentPath = videoPath;
    m_overviewTargetBuildId.clear();
    m_runningBuild.reset();
    m_previewTimer->stop(); m_pendingPreview.reset(); m_previewContext.reset(); m_activeRepresentation.reset();

    m_scenes.clear();
    m_profile = {};
    m_buildState = {};
    m_speechSegments.clear();
    m_videoSummary.clear();
    m_repr.reset();
    m_indexPercent   = 0;
    m_indexStageLabel = tr("准备中...");
    m_isIndexing     = false;

    QPointer<VideoAnalysisViewModel> guard(this);
    const auto stillCurrent = [guard, videoPath] { return guard && guard->m_currentPath == videoPath; };
    emit contentProfileReady(m_profile);
    if (!stillCurrent()) return;
    emit semanticUnitsReady({});
    if (!stillCurrent()) return;
    emit presentationReady({}, {}, {});
    if (!stillCurrent()) return;
    emit scenesReady(m_scenes);
    if (!stillCurrent()) return;
    emit speechSegmentsReady(m_speechSegments);
    if (!stillCurrent()) return;
    emit summaryReady({});
    if (!stillCurrent()) return;
    emit progressChanged(0, m_indexStageLabel);
    if (!stillCurrent()) return;
    emit indexingChanged(false);
    if (!stillCurrent()) return;

    if (videoPath.isEmpty()) return;
    // Opening can happen after the service has synchronously started/restored a build.
    auto representation = m_analysis ? m_analysis->representation(videoPath)
                                    : m_indexer ? m_indexer->representation(videoPath) : nullptr;
    if (representation && representation->level >= VideoRepresentation::Level0) {
        if (!representation->build.buildId.isEmpty() &&
            (representation->build.state == ArtifactState::Ready || representation->build.state == ArtifactState::Partial))
            m_activeRepresentation = representation;
        applyRepresentation(*representation);
        if (!stillCurrent()) return;
        m_indexPercent = 100;
        m_indexStageLabel = tr("已加载持久化视频索引");
    }
    m_isIndexing = false;
    emit progressChanged(m_indexPercent, m_indexStageLabel);
    if (!stillCurrent()) return;
    emit indexingChanged(false);
    if (!stillCurrent()) return;
    if (m_analysis) {
        auto run = m_analysis->runningBuildContext();
        if (run && run->filePath == videoPath) startRun(*run);
    }
}

bool VideoAnalysisViewModel::refreshActiveRepresentation() {
    if (m_currentPath.isEmpty()) return false;
    if (isPreview()) return m_repr->metadata.filePath == m_currentPath &&
        !m_repr->build.fileFingerprint.isEmpty() &&
        m_repr->build.fileFingerprint == VideoFileIdentity::fingerprint(m_currentPath);
    const auto path = m_currentPath;
    const auto value = m_analysis ? m_analysis->representation(path) : m_indexer ? m_indexer->representation(path) : nullptr;
    if (!value || !value->isValid() || value->metadata.filePath != path || value->build.buildId.isEmpty()) return false;
    QPointer<VideoAnalysisViewModel> guard(this);
    if (!m_repr || m_repr->build.toJson() != value->build.toJson()) {
        m_activeRepresentation = value;
        applyRepresentation(*value);
    }
    return guard && guard->m_currentPath == path && guard->displayedBuildId() == value->build.buildId;
}

void VideoAnalysisViewModel::changeType(VideoContentType type) {
    if (!m_analysis || m_currentPath.isEmpty()) return;
    m_analysis->changeType(m_currentPath, type);
}
void VideoAnalysisViewModel::rebuild(bool automatic) {
    if (!m_analysis || m_currentPath.isEmpty()) return;
    if (automatic) m_analysis->analyzeAutomatically(m_currentPath);
    else m_analysis->analyzeVideo(m_currentPath);
}
void VideoAnalysisViewModel::cancelBuild() {
    if (m_analysis) m_analysis->cancelBuild();
}

qint64 VideoAnalysisViewModel::buildElapsedMs() const {
    return m_analysis ? m_analysis->buildElapsedMs(m_buildState.buildId) : -1;
}

QString VideoAnalysisViewModel::sceneDescription(int sceneId) const
{
    if (!m_repr) return {};
    return m_repr->sceneDescriptions.value(sceneId);
}

SceneFusion VideoAnalysisViewModel::sceneFusion(int sceneId) const
{
    if (!m_repr) return {};
    return m_repr->sceneFusions.value(sceneId);
}

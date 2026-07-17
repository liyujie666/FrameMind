#include "app/dicontainer.h"

#include "infrastructure/eventbus.h"
#include "infrastructure/databasemanager.h"
#include "infrastructure/networkclient.h"
#include "service/settingsservice.h"
#include "service/themeservice.h"
#include "service/playerservice.h"
#include "service/agentservice.h"
#include "service/llmproviderservice.h"
#include "service/conversationservice.h"
#include "service/filemanagerservice.h"
#include "service/scene_detector.h"
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
#include "service/clip_service.h"
#include "service/embedding_service.h"
#endif
#ifdef FRAMEMIND_HAS_WHISPER
#include "service/whisper_service.h"
#endif
#include "viewmodel/playerviewmodel.h"
#include "viewmodel/chatviewmodel.h"
#include "viewmodel/filelistviewmodel.h"

#include <QStandardPaths>
#include <QDir>

DIContainer::DIContainer() = default;
DIContainer::~DIContainer() = default;

void DIContainer::initialize()
{
    // Infrastructure
    m_eventBus = EventBus::instance();

    const QString appData =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(appData);
    m_db = DatabaseManager::instance();
    m_db->initialize(appData + QStringLiteral("/agent.db"));

    m_network = std::make_unique<NetworkClient>();

    // Services
    m_settingsService = std::make_unique<SettingsService>(m_db);
    m_themeService    = std::make_unique<ThemeService>(m_settingsService.get());
    m_playerService   = std::make_unique<PlayerService>();
    m_providerService = std::make_unique<LLMProviderService>(m_settingsService.get());
    m_providerService->setNetworkClient(m_network.get());
    m_agentService   = std::make_unique<AgentService>(m_network.get(),
                                                      m_settingsService.get(),
                                                      m_providerService.get());
    m_convService    = std::make_unique<ConversationService>(m_db);
    m_fileService    = std::make_unique<FileManagerService>(m_db);

    // ---- Video RAG 小模型服务 ----

    // 模型目录：AppData/models/
    const QString modelsDir = appData + QStringLiteral("/models");
    QDir().mkpath(modelsDir);

    // SceneDetector（直方图差异，无需外部模型，始终可用）
    m_sceneDetector = std::make_unique<SceneDetector>();

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    // CLIP 视觉/文本 embedding
    m_clipService = std::make_unique<ClipService>();
    m_clipService->initialize(
        modelsDir + QStringLiteral("/clip_visual.onnx"),
        modelsDir + QStringLiteral("/clip_text.onnx"));

    // BGE 文本 embedding
    m_embeddingService = std::make_unique<EmbeddingService>();
    m_embeddingService->initialize(
        modelsDir + QStringLiteral("/bge-small-zh.onnx"));
#endif

#ifdef FRAMEMIND_HAS_WHISPER
    // Whisper 语音转写
    m_whisperService = std::make_unique<WhisperService>();
    m_whisperService->initialize(
        modelsDir + QStringLiteral("/ggml-small.bin"));
#endif

    // ViewModels
    m_playerVM = std::make_unique<PlayerViewModel>(m_playerService.get(),
                                                  m_eventBus);
    m_chatVM   = std::make_unique<ChatViewModel>(m_agentService.get(),
                                                m_convService.get(),
                                                m_eventBus);
    m_chatVM->setPlayerViewModel(m_playerVM.get());
    m_fileListVM = std::make_unique<FileListViewModel>(m_fileService.get(),
                                                      m_eventBus,
                                                      m_playerService.get());
}

#include "service/agent/video_analysis_service.h"

#include "service/agent/one_shot_vlm_channel.h"
#include "service/playerservice.h"
#include "service/agent/video_indexer.h"
#include "service/rag/video_rag_store.h"
#include "service/rag/audio_visual_aligner.h"
#include "infrastructure/databasemanager.h"
#include "infrastructure/databasemanager.h"

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
#  include "service/embedding_service.h"
#endif

#include <QUuid>
#include <QPointer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <algorithm>

namespace {

// ============================================================
// 视频类型枚举
// ============================================================
enum class VideoContentType {
    Unknown,        // 未知类型
    Educational,    // 教学/讲座/课程
    Interview,      // 访谈/对话/讨论
    Documentary,    // 纪录片/专题片
    Drama,          // 剧情/影视
    Vlog,           // Vlog/生活记录
    News,           // 新闻/资讯
    Tutorial,       // 操作教程/演示
    Presentation    // 演讲/报告/发布会
};

// ============================================================
// Prompt 模板：agent-core-design.md §3.2
// ============================================================

// 类型检测提示词（首个场景分析时使用）
const char* kVideoTypeDetectionPrompt = R"PROMPT(
你是视频内容分类专家。请根据提供的视频帧，判断这个视频最可能属于哪种类型。

## 可选类型
1. **educational** - 教学/讲座/课程（有讲师、PPT、板书、教学内容）
2. **interview** - 访谈/对话/讨论（多人对话、演播厅、话题讨论）
3. **documentary** - 纪录片/专题片（叙事性强、场景多样、旁白解说）
4. **drama** - 剧情/影视（故事情节、角色扮演、艺术化场景）
5. **vlog** - Vlog/生活记录（个人视角、日常场景、随拍风格）
6. **news** - 新闻/资讯（主播、字幕条、新闻演播室）
7. **tutorial** - 操作教程/演示（屏幕录制、软件界面、操作步骤）
8. **presentation** - 演讲/报告/发布会（台上演讲者、观众席、正式场合）

## 判断依据
- 画面构图和拍摄风格
- 是否有教学/演示元素（PPT、白板、图表）
- 人物数量和互动方式
- 环境特征（演播厅、教室、生活场景等）
- 屏幕文字和UI元素

## 输出格式
只输出一个 JSON 对象：
{
  "type": "类型英文代码",
  "confidence": 0.0-1.0,
  "reasoning": "1-2句判断理由"
}
)PROMPT";

// 通用场景描述提示词（作为后备）
const char* kSceneDescPrompt = R"PROMPT(
你是一个专业的视频内容分析师。请根据按时间顺序提供的视频帧，理解并描述这个场景的核心内容。

## 分析目标
用户希望通过场景摘要快速了解"这段视频在讲什么"，以便决定是否深入观看或做笔记。

## 描述原则
1. **内容优先**：优先提炼场景传达的信息、知识点、话题或事件，而非罗列画面元素
2. **用户视角**：像给朋友推荐视频一样，说"这段讲了XX"，而不是"画面中有XX"
3. **结合视觉线索**：
   - 如果是教学/讲座：关注屏幕文字、PPT内容、演示物品，提炼知识点
   - 如果是对话/访谈：关注人物互动、表情动作，理解讨论的话题
   - 如果是剧情/纪录片：关注事件进展、场景变化，概括正在发生什么
4. **事实基础**：基于可见证据（画面内容、屏幕文字、人物动作），但要提炼其含义

## 输出格式
只输出一个 JSON 对象，不要 Markdown 代码块或额外说明：
{
  "visual_description": "2-4句话，描述这个场景的核心内容/主题/知识点。像给用户写笔记大纲一样。",
  "visible_text": ["画面中清晰可见的文字，如PPT标题、字幕、标语等"],
  "actions": ["关键动作或事件，如'演示实验'、'展示图表'、'讨论问题'"],
  "uncertain": ["不确定的推测，需要音频确认的部分"],
  "confidence": 0.0
}

## 示例对比
❌ 不好："画面中有一位穿西装的男士站在白板前，背景是办公室，白板上有文字和图表。"
✅ 很好："讲解者介绍敏捷开发流程，白板展示Sprint周期和看板方法的关键步骤。"

❌ 不好："两位主持人坐在演播厅，背景有LED屏，桌上有话筒和文件。"
✅ 很好："主持人讨论人工智能伦理问题，LED屏显示'AI技术的社会责任'主题。"
)PROMPT";

// ============================================================
// 针对不同视频类型的专用场景描述提示词
// ============================================================

const char* kEducationalScenePrompt = R"PROMPT(
你是教学内容分析师。这是一个教学/讲座视频的场景，请提炼其教学内容。

## 分析重点
1. **知识点识别**：这段在讲解什么概念、原理、方法？
2. **教学手段**：使用了什么教学方式（板书、PPT、演示、举例、图表等）
3. **内容定位**：这部分在课程中的作用（引入、详解、总结、练习等）

## 描述框架
推荐格式："讲解[知识点/概念]，通过[教学手段]说明[核心要点]"

## 关注线索
- PPT/白板上的标题和关键词
- 讲师的手势和指向（说明重点）
- 图表、公式、代码等教学素材
- 演示实验或实物展示

只输出 JSON：
{
  "visual_description": "2-4句，突出知识点和教学内容",
  "visible_text": ["PPT标题、板书、关键术语"],
  "actions": ["讲解XX概念"、"演示YY实验"、"推导ZZ公式"],
  "uncertain": [],
  "confidence": 0.0
}
)PROMPT";

const char* kInterviewScenePrompt = R"PROMPT(
你是访谈内容分析师。这是一个访谈/对话视频的场景，请提炼讨论的话题。

## 分析重点
1. **话题识别**：正在讨论什么议题、问题或观点？
2. **互动方式**：提问、回答、辩论、补充？
3. **观点提炼**：核心论点或关键信息是什么？

## 描述框架
推荐格式："[角色]讨论[话题]，提出[观点/论点]"

## 关注线索
- 屏幕字幕和话题标题
- 人物的表情和手势（强调重点）
- 辅助展示物（图表、照片、实物）
- 对话的情绪和节奏

只输出 JSON：
{
  "visual_description": "2-4句，突出讨论话题和核心观点",
  "visible_text": ["话题标题、字幕、图表标注"],
  "actions": ["提问XX问题"、"阐述YY观点"、"展示ZZ案例"],
  "uncertain": [],
  "confidence": 0.0
}
)PROMPT";

const char* kTutorialScenePrompt = R"PROMPT(
你是操作教程分析师。这是一个教程/演示视频的场景，请提炼操作步骤。

## 分析重点
1. **操作目标**：这一步要实现什么功能或效果？
2. **操作步骤**：具体在做什么操作（点击、输入、拖拽等）
3. **关键提示**：有什么注意事项或技巧？

## 描述框架
推荐格式："演示[操作目标]，通过[具体步骤]实现[效果]"

## 关注线索
- 软件界面和菜单选项
- 鼠标光标的位置和操作
- 屏幕标注和箭头指示
- 操作前后的界面变化

只输出 JSON：
{
  "visual_description": "2-4句，突出操作步骤和目标",
  "visible_text": ["菜单项、按钮文字、设置选项"],
  "actions": ["点击XX菜单"、"输入YY参数"、"调整ZZ设置"],
  "uncertain": [],
  "confidence": 0.0
}
)PROMPT";

const char* kDocumentaryScenePrompt = R"PROMPT(
你是纪录片内容分析师。这是一个纪录片/专题片的场景，请提炼事件或信息。

## 分析重点
1. **场景内容**：展示了什么地点、人物、事件？
2. **叙事进展**：故事或主题推进到哪里？
3. **信息价值**：这段传达了什么关键信息或观点？

## 描述框架
推荐格式："展示[场景/事件]，呈现[核心信息/主题]"

## 关注线索
- 场景环境和地理特征
- 人物活动和事件发展
- 字幕标注的时间、地点、人物
- 镜头语言（特写、全景、运动）

只输出 JSON：
{
  "visual_description": "2-4句，突出场景内容和叙事信息",
  "visible_text": ["地点标注、时间字幕、人物介绍"],
  "actions": ["展示XX场景"、"记录YY事件"、"访问ZZ人物"],
  "uncertain": [],
  "confidence": 0.0
}
)PROMPT";

const char* kPresentationScenePrompt = R"PROMPT(
你是演讲内容分析师。这是一个演讲/报告/发布会的场景，请提炼演讲内容。

## 分析重点
1. **演讲主题**：这部分在讲什么论点、产品、方案？
2. **支撑材料**：使用了什么证据、案例、数据？
3. **说服策略**：如何论证或展示观点？

## 描述框架
推荐格式："演讲者介绍[主题/产品]，通过[材料/演示]说明[核心观点]"

## 关注线索
- 演示文稿的标题和要点
- 产品展示和功能演示
- 数据图表和对比分析
- 演讲者的手势和强调

只输出 JSON：
{
  "visual_description": "2-4句，突出演讲主题和核心观点",
  "visible_text": ["幻灯片标题、产品名称、关键数据"],
  "actions": ["介绍XX产品"、"展示YY功能"、"论证ZZ观点"],
  "uncertain": [],
  "confidence": 0.0
}
)PROMPT";

// 通用场景描述（作为后备）
const char* kGenericScenePrompt = R"PROMPT(
你是一个专业的视频内容分析师。请根据按时间顺序提供的视频帧，理解并描述这个场景的核心内容。

## 分析目标
用户希望通过场景摘要快速了解"这段视频在讲什么"，以便决定是否深入观看或做笔记。

## 描述原则
1. **内容优先**：优先提炼场景传达的信息、知识点、话题或事件，而非罗列画面元素
2. **用户视角**：像给朋友推荐视频一样，说"这段讲了XX"，而不是"画面中有XX"
3. **结合视觉线索**：
   - 如果是教学/讲座：关注屏幕文字、PPT内容、演示物品，提炼知识点
   - 如果是对话/访谈：关注人物互动、表情动作，理解讨论的话题
   - 如果是剧情/纪录片：关注事件进展、场景变化，概括正在发生什么
4. **事实基础**：基于可见证据（画面内容、屏幕文字、人物动作），但要提炼其含义

## 输出格式
只输出一个 JSON 对象，不要 Markdown 代码块或额外说明：
{
  "visual_description": "2-4句话，描述这个场景的核心内容/主题/知识点。像给用户写笔记大纲一样。",
  "visible_text": ["画面中清晰可见的文字，如PPT标题、字幕、标语等"],
  "actions": ["关键动作或事件，如'演示实验'、'展示图表'、'讨论问题'"],
  "uncertain": ["不确定的推测，需要音频确认的部分"],
  "confidence": 0.0
}

## 示例对比
❌ 不好："画面中有一位穿西装的男士站在白板前，背景是办公室，白板上有文字和图表。"
✅ 很好："讲解者介绍敏捷开发流程，白板展示Sprint周期和看板方法的关键步骤。"

❌ 不好："两位主持人坐在演播厅，背景有LED屏，桌上有话筒和文件。"
✅ 很好："主持人讨论人工智能伦理问题，LED屏显示'AI技术的社会责任'主题。"
)PROMPT";

/**
 * 根据视频类型获取专用的场景描述提示词
 */
QString getScenePromptForType(VideoContentType type)
{
    switch (type) {
    case VideoContentType::Educational:
        return QString::fromUtf8(kEducationalScenePrompt);
    case VideoContentType::Interview:
        return QString::fromUtf8(kInterviewScenePrompt);
    case VideoContentType::Tutorial:
        return QString::fromUtf8(kTutorialScenePrompt);
    case VideoContentType::Documentary:
        return QString::fromUtf8(kDocumentaryScenePrompt);
    case VideoContentType::Presentation:
        return QString::fromUtf8(kPresentationScenePrompt);
    default:
        return QString::fromUtf8(kGenericScenePrompt);
    }
}

/**
 * 从字符串解析视频类型
 */
VideoContentType parseVideoType(const QString& typeStr)
{
    const QString lower = typeStr.toLower();
    if (lower == QLatin1String("educational")) return VideoContentType::Educational;
    if (lower == QLatin1String("interview")) return VideoContentType::Interview;
    if (lower == QLatin1String("tutorial")) return VideoContentType::Tutorial;
    if (lower == QLatin1String("documentary")) return VideoContentType::Documentary;
    if (lower == QLatin1String("drama")) return VideoContentType::Drama;
    if (lower == QLatin1String("vlog")) return VideoContentType::Vlog;
    if (lower == QLatin1String("news")) return VideoContentType::News;
    if (lower == QLatin1String("presentation")) return VideoContentType::Presentation;
    return VideoContentType::Unknown;
}

/**
 * 视频类型转字符串（用于日志和调试）
 */
QString videoTypeToString(VideoContentType type)
{
    switch (type) {
    case VideoContentType::Educational: return QStringLiteral("教学/讲座");
    case VideoContentType::Interview: return QStringLiteral("访谈/对话");
    case VideoContentType::Tutorial: return QStringLiteral("操作教程");
    case VideoContentType::Documentary: return QStringLiteral("纪录片");
    case VideoContentType::Drama: return QStringLiteral("剧情/影视");
    case VideoContentType::Vlog: return QStringLiteral("Vlog");
    case VideoContentType::News: return QStringLiteral("新闻");
    case VideoContentType::Presentation: return QStringLiteral("演讲/发布会");
    default: return QStringLiteral("未知");
    }
}

// 阶段二：保守融合。归因约束是这个 prompt 的核心，不能放松。
const char* kSceneFusionPrompt = R"PROMPT(
你是影视内容分析师。你会收到同一时间段的两份独立证据：
  A. 视觉描述（由画面生成）
  B. 同期音频转写（可能是对白、旁白、讲解，也可能与画面无关）
另外会给你程序计算出的关联度信号（时间覆盖率、语义相似度等）以及候选关系，供你参考。

## 你的任务
1. **判断音画关系**（从以下四类中选一个）：
   - strong：音频直接围绕画面中可见的对象、人物或正在发生的事件（如讲解者解释屏幕上的内容）
   - contextual：音频提供与画面相关的背景信息，但不是画面中直接可见的事实（如旁白补充历史背景）
   - independent：音频与画面主题不同（如独立解说、无关话题、背景音乐、电视/广播内容）
   - unknown：信息不足，无法判断关联

2. **生成融合描述**：
   - **目标**：帮助用户理解"这段视频在讲什么"，方便做笔记和快速定位内容
   - **内容优先**：优先提炼知识点、话题、事件，而非罗列画面元素
   - **音画协同**：
     * 当 strong 关系时：结合视觉和音频，提炼完整的内容主题（如"讲解者介绍XX原理，演示YY步骤"）
     * 当 contextual 关系时：以视觉为主，音频作为补充背景（如"展示XX场景，旁白补充了YY历史"）
     * 当 independent 关系时：分别描述，明确标注（如"画面展示XX；背景播放YY音乐"）

## 归因约束（必须严格遵守）
1. 不得因为台词中出现某个姓名，就确认画面中某张脸或某个人物的身份
2. 不得把画外音、旁白归属给画面中可见的人物
3. 旁白必须表述为"旁白介绍…"、"解说提到…"
4. 电视、广播等场景内媒体的声音必须表述为"电视/广播中提到…"
5. 音画无关（independent）时，必须分别描述，不得强行合并
6. 无法确认的对应关系要明确写出"当前证据不足以确认"

## 输出
只输出一个 JSON 对象，不要 Markdown 代码块，不要额外解释：
{
  "relation": "strong|contextual|independent|unknown",
  "confidence": 0.0~1.0,
  "audio_type": "dialogue|narration|background_media|ambient|unknown",
  "audio_summary": "音频内容的客观摘要，1-2句",
  "fused_description": "融合后的场景描述，2-4句，面向用户做笔记的场景，突出内容主题而非画面元素"
}

## 示例对比
### 场景：教学视频
视觉："白板上展示流程图，讲解者指向关键节点"
音频："首先我们看Sprint计划会议，这是敏捷开发的核心环节..."
❌ 不好："画面中讲解者站在白板前，白板上有流程图。音频中提到敏捷开发和Sprint。"
✅ 很好："讲解者介绍敏捷开发的Sprint计划会议流程，通过白板流程图演示关键环节和步骤。"

### 场景：访谈节目
视觉："两位嘉宾坐在演播厅对话，LED屏显示'AI伦理'主题"
音频："我认为人工智能的发展必须建立在伦理框架之上，否则会带来社会风险..."
❌ 不好："画面中有两位嘉宾和LED屏。音频讨论AI伦理问题。"
✅ 很好："嘉宾讨论人工智能发展的伦理框架问题，强调技术进步需要建立在社会责任基础上。"
)PROMPT";

const char* kVideoSummaryPrompt = R"PROMPT(
你是视频内容分析师。我将提供一部视频的所有场景描述（按时间顺序排列），请综合生成一段连贯的全视频摘要。

## 用户需求
用户希望通过摘要快速了解：
1. 这个视频主要讲了什么（主题/知识点/话题）
2. 内容如何组织和展开（结构和逻辑）
3. 是否值得完整观看或哪些部分需要重点关注

## 摘要要求
### 内容维度
- **主题提炼**：开篇明确视频的核心主题或目的（如"本视频介绍XX原理"、"讨论YY问题"）
- **知识结构**：如果是教学/讲座，提炼知识框架和关键概念
- **叙事线索**：如果是剧情/纪录片，概括事件发展和情节转折
- **话题演进**：如果是对话/访谈，总结讨论的主要论点和观点

### 形式要求
- **长度**：300-500字
- **格式**：使用 Markdown，包含适当的段落和小标题
- **结构建议**：
  ```
  ## 视频概览
  [1-2句话总结核心主题]
  
  ## 主要内容
  [分段描述内容展开，突出知识点/话题/事件]
  
  ## 关键亮点（可选）
  [值得关注的重点片段或核心观点]
  ```

### 禁止事项
- ❌ 不要逐场景罗列："第一个场景讲了XX，第二个场景讲了YY..."
- ❌ 不要堆砌形式描述："画面中有主持人、嘉宾、LED屏..."
- ❌ 不要虚构场景描述中未出现的信息
- ❌ 不要使用"该视频"、"本段视频"等生硬的机器用语

### 语言风格
- ✅ 自然流畅，像给朋友推荐视频
- ✅ 叙述连贯，体现内容之间的逻辑关系
- ✅ 突出信息价值，帮助用户快速决策
- ✅ 使用中文

## 示例对比
### ❌ 不好的摘要（堆砌形式）
```
本视频共15个场景，时长约20分钟。画面中有主持人、嘉宾和LED屏幕。
第1-3场景展示了演播厅环境，主持人和嘉宾坐在桌前。
第4-6场景出现了PPT，上面有文字和图表。
第7-10场景继续讨论，背景有装饰灯光...
```

### ✅ 很好的摘要（突出内容）
```
## 视频概览
这是一期关于人工智能伦理的深度访谈节目，探讨AI技术发展中的社会责任问题。

## 主要内容
节目开篇提出核心问题：AI技术的快速发展是否超出了伦理框架的约束？嘉宾从三个维度展开讨论：

**算法公平性**：通过真实案例分析招聘算法中的性别偏见，强调训练数据的代表性至关重要。

**隐私保护**：讨论人脸识别技术的边界问题，认为技术便利不应以牺牲个人隐私为代价。

**责任归属**：当AI系统做出错误决策时，谁应该承担责任？嘉宾主张建立清晰的问责机制。

## 关键亮点
节目最后提出建设性建议：技术公司应建立伦理审查委员会，在产品开发早期就引入伦理评估。
```
)PROMPT";

/**
 * 从模型回复中抽出第一个 JSON 对象。
 *
 * 模型经常会包裹 ```json 代码块或在前后添加说明文字，
 * 这里按第一个 '{' 到最后一个 '}' 截取后再解析。
 */
QJsonObject extractJsonObject(const QString& raw)
{
    const int begin = raw.indexOf(QLatin1Char('{'));
    const int end   = raw.lastIndexOf(QLatin1Char('}'));
    if (begin < 0 || end <= begin) return {};

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(
        raw.mid(begin, end - begin + 1).toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return {};
    return doc.object();
}

QStringList jsonStringList(const QJsonObject& object, const QString& key, int maxItems = 12)
{
    QStringList result;
    const QJsonArray values = object.value(key).toArray();
    for (const QJsonValue& value : values) {
        const QString text = value.toString().simplified();
        if (!text.isEmpty() && !result.contains(text)) result.append(text);
        if (result.size() >= maxItems) break;
    }
    return result;
}

QString msToTimeLabel(int64_t ms)
{
    const int h = static_cast<int>(ms / 3600000);
    const int m = static_cast<int>((ms % 3600000) / 60000);
    const int sec = static_cast<int>((ms % 60000) / 1000);
    if (h > 0)
        return QStringLiteral("%1:%2:%3")
                   .arg(h)
                   .arg(m, 2, 10, QChar('0'))
                   .arg(sec, 2, 10, QChar('0'));
    return QStringLiteral("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
}

const char* kFrameDescPrompt = R"PROMPT(
描述这一帧画面。要求：
- 具体到画面中的人物/物体/场景/文字
- 如有动作请说明
- 客观描述，不做主观推断
)PROMPT";
} // namespace

VideoAnalysisService::VideoAnalysisService(OneShotVlmChannel* vlmChannel,
                                           VideoIndexer*      indexer,
                                           VideoRAGStore*     ragStore,
                                           PlayerService*     player,
                                           DatabaseManager*   db,
                                           QObject*           parent)
    : QObject(parent)
    , m_vlmChannel(vlmChannel)
    , m_indexer(indexer)
    , m_ragStore(ragStore)
    , m_player(player)
    , m_db(db)
{
    if (m_indexer) {
        connect(m_indexer, &VideoIndexer::levelReady,
                this, [this](int level, QSharedPointer<VideoRepresentation> repr) {
            emit analysisProgress(30 + level * 20,
                                  tr("Level %1 索引完成").arg(level));
            if (level == 1) {
                // 缓存命中：repr 已有场景描述和摘要，直接重放信号，不重复调 VLM
                if (!repr->videoSummary.isEmpty()) {
                    qDebug() << "[VideoAnalysisService] 使用缓存的分析结果 | videoId:" << repr->videoId;
                    for (auto it = repr->sceneDescriptions.constBegin();
                         it != repr->sceneDescriptions.constEnd(); ++it) {
                        emit sceneDescribed(it.key(), it.value());
                    }
                    emit summaryReady(repr->videoSummary);
                    return;
                }

                if (!repr->scenes.isEmpty()) {
                    startDescribeAllScenes(repr);
                }
            }
        });
    }
}

// ============================================================
// 统筹入口
// ============================================================

void VideoAnalysisService::onVideoOpened(const QString& videoPath)
{
    const QString videoId = VideoIndexer::computeVideoId(videoPath);
    if (m_vlmChannel && !m_backgroundVideoId.isEmpty()
        && m_backgroundVideoId != videoId) {
        m_vlmChannel->cancelBackground(m_backgroundVideoId);
    }
    m_backgroundVideoId = videoId;
    if (m_ragStore) {
        // 先尝试加载已有索引；QA 缓存只能在其原始证据也仍存在时复用。
        m_ragStore->loadVideo(videoId);
        if (m_ragStore->hasIndexedContent(videoId)) {
            qDebug() << "[VideoAnalysisService] 检测到已有RAG索引，跳过重复构建 | videoId:" << videoId;
            emit analysisProgress(100, tr("已加载持久化视频索引"));
            return;
        }
    }
    if (m_indexer) {
        qDebug() << "[VideoAnalysisService] 未找到已有索引，开始构建RAG | videoId:" << videoId;
        m_indexer->startIndex(videoPath);
    }
}

void VideoAnalysisService::analyzeVideo(const QString& videoPath)
{
    onVideoOpened(videoPath);
    // 后续 Level 2 由 levelReady 触发
}

QSharedPointer<VideoRepresentation> VideoAnalysisService::representation(
    const QString& videoPath) const
{
    return m_indexer ? m_indexer->representation(videoPath) : nullptr;
}

// ============================================================
// 串行描述所有场景，全部完成后触发全视频摘要
// （AgentService 是单流设计，不支持并发请求，必须串行）
// ============================================================

void VideoAnalysisService::startDescribeAllScenes(
    QSharedPointer<VideoRepresentation> repr)
{
    const int total = repr->scenes.size();
    if (total <= 0) return;

    qDebug() << "[VideoAnalysisService] 开始描述全部场景，共" << total << "个";
    emit analysisProgress(50, tr("分析场景内容（0/%1）").arg(total));

    QPointer<VideoAnalysisService> guard(this);

    // 第一步：检测视频类型（只在首次或缓存未命中时执行）
    auto startDescribeWithType = [guard, repr, total]() {
        if (!guard) return;
        
        // 递归串行：完成一个再触发下一个，保证 AgentService 单流不冲突
        auto describeNext = QSharedPointer<std::function<void(int)>>::create();

        *describeNext = [guard, repr, total, describeNext](int idx) mutable {
            if (!guard) return;

            // 所有场景处理完毕，触发摘要
            if (idx >= total) {
                qDebug() << "[VideoAnalysisService] 全部场景描述完成，触发 summarizeVideo:"
                         << repr->metadata.fileName;
                guard->summarizeVideo(repr);
                return;
            }

            const int sceneId = repr->scenes[idx].id;

            // 只有完整融合结果存在时才跳过；旧版 sceneDescriptions 缓存仍需补做融合
            if (repr->sceneFusions.contains(sceneId)
                && repr->sceneFusions.value(sceneId).isValid()) {
                emit guard->analysisProgress(
                    50 + (idx + 1) * 40 / total,
                    tr("分析场景内容（%1/%2）").arg(idx + 1).arg(total));
                (*describeNext)(idx + 1);
                return;
            }

            guard->doDescribeSceneWithCallback(
                sceneId, repr,
                [guard, total, idx, describeNext](int /*sceneId*/) mutable {
                    if (!guard) return;
                    emit guard->analysisProgress(
                        50 + (idx + 1) * 40 / total,
                        tr("分析场景内容（%1/%2）").arg(idx + 1).arg(total));
                    (*describeNext)(idx + 1);
                });
        };

        (*describeNext)(0);
    };

    // 检测视频类型（如果未缓存）
    if (!m_videoTypeCache.contains(repr->videoId)) {
        detectVideoType(repr, startDescribeWithType);
    } else {
        qDebug() << "[VideoAnalysisService] 使用缓存的视频类型:"
                 << m_videoTypeCache.value(repr->videoId);
        startDescribeWithType();
    }
}

// ============================================================
// 视频类型检测
// ============================================================

void VideoAnalysisService::detectVideoType(
    QSharedPointer<VideoRepresentation> repr,
    std::function<void()> onDone)
{
    if (!repr || repr->scenes.isEmpty()) {
        if (onDone) onDone();
        return;
    }

    emit analysisProgress(48, tr("正在检测视频类型..."));

    // 使用前3个场景的关键帧来判断类型（更准确）
    QList<QImage> sampleFrames;
    const int sampleCount = qMin(3, repr->scenes.size());
    
    for (int i = 0; i < sampleCount; ++i) {
        const Scene& scene = repr->scenes[i];
        if (!scene.keyframe.isNull()) {
            sampleFrames.append(scene.keyframe);
        } else if (!scene.keyframePath.isEmpty()) {
            QImage img(scene.keyframePath);
            if (!img.isNull()) sampleFrames.append(img);
        }
    }

    if (sampleFrames.isEmpty()) {
        qWarning() << "[VideoAnalysisService] 无法获取样本帧，跳过类型检测";
        m_videoTypeCache.insert(repr->videoId, QStringLiteral("unknown"));
        if (onDone) onDone();
        return;
    }

    const QString userText = tr("这是视频的前几个场景的画面，请判断视频类型。");
    
    QPointer<VideoAnalysisService> guard(this);
    oneShotVLM(QString::fromUtf8(kVideoTypeDetectionPrompt),
               userText,
               sampleFrames,
               false,
               repr->videoId,
               [guard, repr, onDone](const QString& reply) {
        if (!guard) return;
        
        const QJsonObject obj = extractJsonObject(reply);
        QString detectedType = obj.value(QStringLiteral("type")).toString();
        const double confidence = obj.value(QStringLiteral("confidence")).toDouble(0.5);
        const QString reasoning = obj.value(QStringLiteral("reasoning")).toString();
        
        if (detectedType.isEmpty()) {
            detectedType = QStringLiteral("unknown");
        }
        
        guard->m_videoTypeCache.insert(repr->videoId, detectedType);
        
        const VideoContentType type = parseVideoType(detectedType);
        const QString typeLabel = videoTypeToString(type);
        
        // 详细的日志输出
        qDebug() << "========================================";
        qDebug() << "[VideoAnalysisService] 视频类型检测完成";
        qDebug() << "  视频ID:" << repr->videoId;
        qDebug() << "  文件名:" << repr->metadata.fileName;
        qDebug() << "  检测类型:" << typeLabel << "(" << detectedType << ")";
        qDebug() << "  置信度:" << QString::number(confidence * 100, 'f', 1) << "%";
        qDebug() << "  判断理由:" << reasoning;
        qDebug() << "========================================";
        
        // 发送带类型标签的进度信息
        emit guard->analysisProgress(
            49, 
            tr("视频类型：%1 | 开始分析场景...").arg(typeLabel)
        );
        
        // 保存到数据库
        if (guard->m_db) {
            // 将类型信息保存到 video_metadata 表
            // TODO: 需要扩展数据库表结构
            qDebug() << "[VideoAnalysisService] 视频类型将在摘要中保存";
        }
        
        if (onDone) onDone();
    });
}

// ============================================================
// Level 2: 场景描述
// ============================================================

void VideoAnalysisService::describeScene(int sceneId,
                                          QSharedPointer<VideoRepresentation> repr)
{
    if (!repr || sceneId < 0 || sceneId >= repr->scenes.size()) return;
    if (repr->sceneFusions.contains(sceneId)
        && repr->sceneFusions.value(sceneId).isValid()) return;

    doDescribeSceneWithCallback(sceneId, repr, nullptr);
}

void VideoAnalysisService::doDescribeSceneWithCallback(
    int sceneId,
    QSharedPointer<VideoRepresentation> repr,
    std::function<void(int)> onDone)
{
    if (!repr || sceneId < 0 || sceneId >= repr->scenes.size()) {
        if (onDone) onDone(sceneId);
        return;
    }

    const Scene& s = repr->scenes[sceneId];
    QList<QImage> frames;
    for (const SceneFrame& representative : s.representativeFrames) {
        QImage image = representative.image;
        if (image.isNull() && !representative.imagePath.isEmpty()) {
            image.load(representative.imagePath);
        }
        if (!image.isNull()) frames.append(image);
    }
    if (frames.isEmpty() && !s.keyframe.isNull()) frames.append(s.keyframe);
    if (frames.isEmpty() && !s.keyframePath.isEmpty()) {
        const QImage persisted(s.keyframePath);
        if (!persisted.isNull()) frames.append(persisted);
    }

    // 若持久化代表帧也缺失，退化为对绑定视频的场景关键时间点截帧
    if (frames.isEmpty() && m_player) {
        const int64_t ts = s.keyframeMs;
        auto fut = m_player->captureFrameAt(repr->metadata.filePath, ts, 2000);
        fut.waitForFinished();
        if (fut.resultCount() > 0) {
            const QImage img = fut.result();
            if (!img.isNull()) frames.append(img);
        }
    }

    // 关键帧缺失时也发出完成回调，避免批次计数死锁
    if (frames.isEmpty()) {
        qWarning() << "[VideoAnalysisService] 场景" << sceneId << "无关键帧，跳过描述";
        if (onDone) onDone(sceneId);
        return;
    }

    // 转换时间戳为可读格式供 LM 参考
    const auto msToTime = [](int64_t ms) -> QString {
        const int h = static_cast<int>(ms / 3600000);
        const int m = static_cast<int>((ms % 3600000) / 60000);
        const int sec = static_cast<int>((ms % 60000) / 1000);
        if (h > 0)
            return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
        return QString("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
    };

    const QString userText = tr("这是视频中 [%1 - %2]（即 %3ms - %4ms）的场景。提供的帧按时间顺序覆盖开始、中间和结束状态；请描述可见内容及状态变化。")
                                 .arg(msToTime(s.startMs)).arg(msToTime(s.endMs))
                                 .arg(s.startMs).arg(s.endMs);

    // 根据视频类型选择合适的提示词
    const QString cachedType = m_videoTypeCache.value(repr->videoId, QStringLiteral("unknown"));
    const VideoContentType videoType = parseVideoType(cachedType);
    const QString scenePrompt = getScenePromptForType(videoType);
    
    qDebug() << "[VideoAnalysisService] 描述场景" << sceneId 
             << "| 类型:" << videoTypeToString(videoType)
             << "| 时间:" << msToTime(s.startMs) << "-" << msToTime(s.endMs);

    QPointer<VideoAnalysisService> guard(this);
    oneShotVLM(scenePrompt,
               userText,
               frames,
               false,
               repr->videoId,
               [guard, sceneId, repr, onDone](const QString& reply) {
        if (!guard) return;

        // 阶段一产物：优先解析结构化纯视觉证据；旧模型或异常输出时保留原文回退。
        const QJsonObject object = extractJsonObject(reply);
        const QString visualDesc = object.value(QStringLiteral("visual_description"))
            .toString().trimmed().isEmpty() ? reply.trimmed()
            : object.value(QStringLiteral("visual_description")).toString().trimmed();
        repr->sceneVisualDescriptions.insert(sceneId, visualDesc);
        if (sceneId >= 0 && sceneId < repr->scenes.size()) {
            Scene& scene = repr->scenes[sceneId];
            scene.visualDescription = visualDesc;
            scene.visibleTexts = jsonStringList(object, QStringLiteral("visible_text"));
            scene.visibleActions = jsonStringList(object, QStringLiteral("actions"));
        }

        if (visualDesc.isEmpty()) {
            qWarning() << "[VideoAnalysisService] 场景" << sceneId
                       << "视觉描述为空，跳过融合";
            if (onDone) onDone(sceneId);
            return;
        }

        // 阶段二：同期音频对齐 + 语义门控 + 保守融合
        guard->fuseSceneAudio(sceneId, visualDesc, repr, onDone);
    });
}

// ============================================================
// 阶段二：同期音频对齐 + 语义门控 + 保守融合
// ============================================================

void VideoAnalysisService::fuseSceneAudio(
    int sceneId,
    const QString& visualDescription,
    QSharedPointer<VideoRepresentation> repr,
    std::function<void(int)> onDone)
{
    SceneFusion fusion;
    fusion.sceneId = sceneId;
    fusion.visualDescription = visualDescription;
    fusion.fusedDescription  = visualDescription;
    fusion.relation  = AudioVisualRelation::Unknown;
    fusion.audioType = SceneAudioType::None;

    const Scene& scene = repr->scenes[sceneId];

    // 对齐器缺失或无转写数据：直接以纯视觉描述收尾，不调用模型
    if (!m_aligner || repr->speechSegments.isEmpty()) {
        commitSceneFusion(fusion, repr, std::move(onDone));
        return;
    }

    fusion.speechSegments = m_aligner->overlappingSpeechSegments(
        scene, repr->speechSegments);
    if (fusion.speechSegments.isEmpty()) {
        commitSceneFusion(fusion, repr, std::move(onDone));
        return;
    }

    fusion.gate = m_aligner->gate(visualDescription, fusion.speechSegments,
                                  scene, repr->scenes);

    // 先把程序判定填进去，模型解析失败时作为回退结果
    fusion.relation   = fusion.gate.candidate;
    fusion.confidence = fusion.gate.candidateConfidence;

    const QString transcript =
        AudioVisualAligner::formatTranscript(fusion.speechSegments);

    QString gateSignals;
    gateSignals += tr("- 同期语音时间覆盖率：%1\n")
                       .arg(fusion.gate.timeCoverage, 0, 'f', 2);
    if (fusion.gate.hasSemanticSimilarity()) {
        gateSignals += tr("- 视觉描述与转写的语义相似度：%1\n")
                           .arg(fusion.gate.semanticSimilarity, 0, 'f', 2);
    } else {
        gateSignals += tr("- 视觉描述与转写的语义相似度：未计算\n");
    }
    gateSignals += tr("- 关键词交集比例：%1\n")
                       .arg(fusion.gate.keywordOverlap, 0, 'f', 2);
    gateSignals += tr("- 该段音频横跨其他场景的比例：%1\n")
                       .arg(fusion.gate.crossSceneSpan, 0, 'f', 2);
    if (!fusion.gate.sharedKeywords.isEmpty()) {
        gateSignals += tr("- 共现关键词：%1\n")
                           .arg(fusion.gate.sharedKeywords.join(QStringLiteral("、")));
    }
    gateSignals += tr("- 程序候选关系：%1（置信度 %2）\n")
                       .arg(SceneFusion::relationToString(fusion.gate.candidate))
                       .arg(fusion.gate.candidateConfidence, 0, 'f', 2);

    const QString userText =
        tr("## 时间段\n%1 - %2\n\n"
           "## A. 纯视觉描述（已确认的视觉事实）\n%3\n\n"
           "## B. 同期音频转写\n%4\n"
           "## C. 程序计算的关联度信号\n%5")
            .arg(msToTimeLabel(scene.startMs), msToTimeLabel(scene.endMs),
                 visualDescription, transcript, gateSignals);

    QPointer<VideoAnalysisService> guard(this);
    oneShotVLM(QString::fromUtf8(kSceneFusionPrompt),
               userText,
               {},
               false,
               repr->videoId,
               [guard, fusion, repr, onDone](const QString& reply) mutable {
        if (!guard) return;

        const QJsonObject obj = extractJsonObject(reply);
        if (obj.isEmpty()) {
            // 模型输出不可解析：保留程序判定，融合描述降级为分述
            qWarning() << "[VideoAnalysisService] 场景" << fusion.sceneId
                       << "融合结果解析失败，回退到程序判定";
            fusion.audioSummary =
                AudioVisualAligner::plainTranscript(fusion.speechSegments);
            fusion.fusedDescription = guard->tr(
                "视觉内容：%1\n同期音频：%2（与画面的关联未经确认）")
                    .arg(fusion.visualDescription, fusion.audioSummary);
            guard->commitSceneFusion(fusion, repr, onDone);
            return;
        }

        fusion.fromModel = true;
        fusion.relation = SceneFusion::relationFromString(
            obj.value(QStringLiteral("relation")).toString());
        fusion.audioType = SceneFusion::audioTypeFromString(
            obj.value(QStringLiteral("audio_type")).toString());
        fusion.audioSummary =
            obj.value(QStringLiteral("audio_summary")).toString().trimmed();

        const QString fused =
            obj.value(QStringLiteral("fused_description")).toString().trimmed();
        if (!fused.isEmpty()) fusion.fusedDescription = fused;

        // 模型置信度与程序候选一致时取较高值，冲突时取较低值（保守）
        const float modelConf = static_cast<float>(
            obj.value(QStringLiteral("confidence")).toDouble(0.0));
        const bool agrees = (fusion.relation == fusion.gate.candidate);
        fusion.confidence = agrees
            ? std::max(modelConf, fusion.gate.candidateConfidence)
            : std::min(std::max(modelConf, 0.3f), 0.6f);

        if (fusion.audioSummary.isEmpty()) {
            fusion.audioSummary =
                AudioVisualAligner::plainTranscript(fusion.speechSegments);
        }

        guard->commitSceneFusion(fusion, repr, onDone);
    });
}

void VideoAnalysisService::commitSceneFusion(
    const SceneFusion& fusion,
    QSharedPointer<VideoRepresentation> repr,
    std::function<void(int)> onDone)
{
    const int sceneId = fusion.sceneId;

    repr->sceneFusions.insert(sceneId, fusion);
    repr->sceneVisualDescriptions.insert(sceneId, fusion.visualDescription);
    // sceneDescriptions 存放最终（融合后）描述，供摘要 / UI / 既有调用方复用
    repr->sceneDescriptions.insert(sceneId, fusion.fusedDescription);

    if (sceneId >= 0 && sceneId < repr->scenes.size()) {
        Scene& scene = repr->scenes[sceneId];
        scene.visualDescription       = fusion.visualDescription;
        scene.audioSummary            = fusion.audioSummary;
        scene.fusedDescription        = fusion.fusedDescription;
        scene.description             = fusion.fusedDescription;
        scene.audioRelation           = fusion.relation;
        scene.audioRelationConfidence = fusion.confidence;
        scene.audioType               = fusion.audioType;
    }

    // 三类证据分别入库，融合描述不覆盖纯视觉事实
    writeSceneEvidence(fusion, repr, VideoChunk::SceneSummary,
                       QStringLiteral("visual"), fusion.visualDescription);
    if (!fusion.audioSummary.isEmpty()) {
        writeSceneEvidence(fusion, repr, VideoChunk::SceneAudio,
                           QStringLiteral("audio"), fusion.audioSummary);
    }
    // 可见文字独立入库，供字幕、招牌、PPT 等精确文字问题走文本检索；
    // 当前来源是 VLM 可见文字转录，未来专用 OCR 可作为同类型补充来源。
    if (sceneId >= 0 && sceneId < repr->scenes.size()
        && !repr->scenes[sceneId].visibleTexts.isEmpty()) {
        writeSceneEvidence(fusion, repr, VideoChunk::Event,
                           QStringLiteral("visible_text"),
                           repr->scenes[sceneId].visibleTexts.join(QStringLiteral("\n")));
    }
    // 无音频时融合描述与视觉描述相同，不重复占用一条 chunk
    if (fusion.hasAudio() && fusion.fusedDescription != fusion.visualDescription) {
        writeSceneEvidence(fusion, repr, VideoChunk::SceneFused,
                           QStringLiteral("fused"), fusion.fusedDescription);
    }

    qDebug() << "[VideoAnalysisService] 场景" << sceneId << "融合完成"
             << "| relation:" << SceneFusion::relationToString(fusion.relation)
             << "| conf:" << fusion.confidence
             << "| 语音段:" << fusion.speechSegments.size()
             << "| 来自模型:" << fusion.fromModel;

    // 保存场景描述到数据库
    if (m_db) {
        m_db->saveSceneDescription(
            repr->videoId,
            sceneId,
            fusion.fusedDescription,
            fusion.visualDescription
        );
    }

    emit sceneFused(sceneId, fusion);
    emit sceneDescribed(sceneId, fusion.fusedDescription);
    if (onDone) onDone(sceneId);
}

void VideoAnalysisService::writeSceneEvidence(
    const SceneFusion& fusion,
    QSharedPointer<VideoRepresentation> repr,
    VideoChunk::ChunkType chunkType,
    const QString& evidenceType,
    const QString& text)
{
    if (!m_ragStore || text.isEmpty()) return;
    if (fusion.sceneId < 0 || fusion.sceneId >= repr->scenes.size()) return;

    const Scene& scene = repr->scenes[fusion.sceneId];

    VideoChunk c;
    c.chunkId = VideoIndexer::makeChunkId(
        repr->videoId, chunkType, scene.startMs, scene.endMs,
        QString::number(fusion.sceneId));
    c.videoId     = repr->videoId;
    c.startMs     = scene.startMs;
    c.endMs       = scene.endMs;
    c.chunkType   = chunkType;
    c.textContent = text;
    c.keyframePath = scene.keyframePath;

    c.metadata.insert(QStringLiteral("scene_id"), fusion.sceneId);
    c.metadata.insert(QStringLiteral("keyframe_ms"),
                      static_cast<qlonglong>(scene.keyframeMs));
    c.metadata.insert(QStringLiteral("file_path"), repr->metadata.filePath);
    c.metadata.insert(QStringLiteral("evidence_type"), evidenceType);
    if (evidenceType == QLatin1String("visible_text")) {
        c.metadata.insert(QStringLiteral("source"), QStringLiteral("vlm_visible_text"));
    }
    c.metadata.insert(QStringLiteral("audio_relation"),
                      SceneFusion::relationToString(fusion.relation));
    c.metadata.insert(QStringLiteral("relation_confidence"), fusion.confidence);
    c.metadata.insert(QStringLiteral("audio_type"),
                      SceneFusion::audioTypeToString(fusion.audioType));
    c.metadata.insert(QStringLiteral("has_speech"), fusion.hasAudio());
    c.metadata.insert(QStringLiteral("visible_texts"), scene.visibleTexts);
    c.metadata.insert(QStringLiteral("visible_actions"), scene.visibleActions);
    c.metadata.insert(QStringLiteral("embedding_model_id"), QStringLiteral("bge_text"));
    c.metadata.insert(QStringLiteral("embedding_version"), QStringLiteral("passage_v2"));
    c.metadata.insert(QStringLiteral("index_version"), 2);

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    if (m_embedder && m_embedder->isReady()) {
        c.textEmbedding = m_embedder->embedPassage(text);
        c.metadata.insert(QStringLiteral("embedding_dimension"),
                          static_cast<int>(c.textEmbedding.size()));
    }
#endif
    m_ragStore->insertChunk(VideoRAGStore::TextSegments, c);
}

// ============================================================
// Level 2: 全视频摘要
// ============================================================

void VideoAnalysisService::summarizeVideo(QSharedPointer<VideoRepresentation> repr)
{
    if (!repr) return;

    emit analysisProgress(92, tr("生成视频摘要..."));

    // 把时间戳转成可读的 mm:ss 格式
    const auto msToTime = [](int64_t ms) -> QString {
        const int h = static_cast<int>(ms / 3600000);
        const int m = static_cast<int>((ms % 3600000) / 60000);
        const int sec = static_cast<int>((ms % 60000) / 1000);
        if (h > 0)
            return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
        return QString("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
    };

    // 组装场景描述列表，包含时间戳便于 LM 生成连贯叙述
    QString scenesText;
    int describedCount = 0;
    for (const Scene& s : repr->scenes) {
        const QString desc = repr->sceneDescriptions.value(s.id);
        if (desc.isEmpty()) continue;
        scenesText += tr("【场景 %1，时间 %2-%3】\n%4\n\n")
                          .arg(s.id + 1)
                          .arg(msToTime(s.startMs))
                          .arg(msToTime(s.endMs))
                          .arg(desc);
        ++describedCount;
    }

    if (scenesText.isEmpty()) {
        qWarning() << "[VideoAnalysisService] 没有任何场景描述，跳过摘要";
        return;
    }

    qDebug() << "[VideoAnalysisService] summarizeVideo: 共"
             << describedCount << "/" << repr->scenes.size() << "个场景有描述";

    // 附加基本元信息辅助 LM 理解上下文
    const QString metaInfo = tr("视频文件：%1，总时长：%2\n\n")
                                 .arg(repr->metadata.fileName)
                                 .arg(msToTime(repr->metadata.durationMs));
    
    // 获取并添加视频类型标签
    QString typeLabel = tr("未知类型");
    if (m_videoTypeCache.contains(repr->videoId)) {
        const QString cachedType = m_videoTypeCache.value(repr->videoId);
        const VideoContentType type = parseVideoType(cachedType);
        typeLabel = videoTypeToString(type);
    }
    
    const QString typeInfo = tr("视频类型：%1\n\n").arg(typeLabel);
    const QString userText = metaInfo + typeInfo + scenesText;

    QPointer<VideoAnalysisService> guard(this);
    qDebug() << "[VideoAnalysisService] 开始请求全视频摘要生成...";
    oneShotVLM(QString::fromUtf8(kVideoSummaryPrompt),
               userText,
               {},
               false,
               repr->videoId,
               [guard, repr](const QString& summary) {
        qDebug() << "[VideoAnalysisService] 摘要生成回调被调用，内容长度:" << summary.length();
        if (!guard) {
            qWarning() << "[VideoAnalysisService] guard 对象已失效，无法处理摘要";
            return;
        }
        if (summary.trimmed().isEmpty()) {
            qWarning() << "[VideoAnalysisService] 摘要生成为空（可能 API 错误或上下文超长）";
            emit guard->analysisProgress(100, tr("分析完成（摘要生成失败）"));
            return;
        }
        repr->videoSummary = summary;
        repr->level = VideoRepresentation::Level2;
        
        // 在摘要前添加视频类型标签
        QString typeLabel = tr("未知类型");
        if (guard->m_videoTypeCache.contains(repr->videoId)) {
            const QString cachedType = guard->m_videoTypeCache.value(repr->videoId);
            const VideoContentType type = parseVideoType(cachedType);
            typeLabel = videoTypeToString(type);
        }
        
        const QString summaryWithType = tr("📌 视频类型：%1\n\n%2")
                                            .arg(typeLabel)
                                            .arg(summary);
        
        qDebug() << "[VideoAnalysisService] 摘要已生成，准备保存到数据库";
        // 保存摘要到数据库
        if (guard->m_db) {
            bool saved = guard->m_db->saveVideoMetadata(
                repr->videoId,
                repr->metadata.filePath,
                summaryWithType,  // 保存带类型标签的摘要
                static_cast<int>(VideoRepresentation::Level2)
            );
            qDebug() << "[VideoAnalysisService] 数据库保存结果:" << (saved ? "成功" : "失败");
        }
        
        qDebug() << "[VideoAnalysisService] 发送完成信号";
        emit guard->analysisProgress(100, tr("分析完成"));
        emit guard->summaryReady(summaryWithType);  // 发送带类型标签的摘要
        
        // 确保通知索引完成状态
        qDebug() << "[VideoAnalysisService] 即将发送 indexingChanged(false) 信号";
    });
}

// ============================================================
// Level 3: 单帧 / 区间
// ============================================================

void VideoAnalysisService::describeFrame(const QImage& frame,
                                          int64_t timestampMs,
                                          const QString& focus,
                                          std::function<void(const QString&)> onDone)
{
    if (frame.isNull()) {
        if (onDone) onDone(tr("<空帧>"));
        return;
    }
    const QString userText = focus.isEmpty()
        ? tr("请描述这一帧画面（时间戳 %1ms）。").arg(timestampMs)
        : tr("请描述这一帧画面（时间戳 %1ms），特别关注：%2").arg(timestampMs).arg(focus);

    oneShotVLM(QString::fromUtf8(kFrameDescPrompt),
               userText, { frame }, true, {}, std::move(onDone));
}

void VideoAnalysisService::analyzeTimeRange(int64_t startMs, int64_t endMs,
                                             const QString& focus,
                                             int sampleCount,
                                             std::function<void(const QString&)> onDone)
{
    if (!m_player || sampleCount <= 0) {
        if (onDone) onDone(tr("<无法采样>"));
        return;
    }
    sampleCount = qBound(2, sampleCount, 10);
    const int64_t step = (endMs - startMs) / qMax(1, sampleCount - 1);

    QList<QImage> frames;
    frames.reserve(sampleCount);
    for (int i = 0; i < sampleCount; ++i) {
        const int64_t ts = startMs + step * i;
        auto fut = m_player->captureFrameAt(ts, 2000);
        fut.waitForFinished();
        if (fut.resultCount() > 0) {
            const QImage img = fut.result();
            if (!img.isNull()) frames.append(img);
        }
    }
    if (frames.isEmpty()) {
        if (onDone) onDone(tr("<截取帧失败>"));
        return;
    }

    const QString userText = tr(
        "以下是视频中 [%1ms - %2ms] 内按时间顺序采样的 %3 帧。请综合分析该段的过程。"
        "%4").arg(startMs).arg(endMs).arg(frames.size())
              .arg(focus.isEmpty() ? QString{} : tr("关注：%1").arg(focus));

    // System prompt 参考 SEQUENCE_ANALYSIS_PROMPT
    const QString sysPrompt = tr(
        "你正在分析一段视频的连续帧序列，请理解帧间的时间关系与运动变化，"
        "综合所有帧回答问题，而不是逐帧独立描述。");

    oneShotVLM(sysPrompt, userText, frames, true, {}, std::move(onDone));
}

// ============================================================
// VideoContext 组装
// ============================================================

VideoContext VideoAnalysisService::buildVideoContext(
    QSharedPointer<VideoRepresentation> repr) const
{
    VideoContext ctx;
    if (!repr) return ctx;

    ctx.videoId     = repr->metadata.filePath;  // P1修复：使用filePath作为唯一ID
    ctx.fileName    = repr->metadata.fileName;
    ctx.durationMs  = repr->metadata.durationMs;
    ctx.width       = repr->metadata.width;
    ctx.height      = repr->metadata.height;
    ctx.fps         = repr->metadata.frameRate;
    ctx.hasAudio    = repr->metadata.hasAudio;

    // 场景概览：ID + 时间区间 + 简短标题
    const auto msToTime = [](int64_t ms) -> QString {
        const int h = static_cast<int>(ms / 3600000);
        const int m = static_cast<int>((ms % 3600000) / 60000);
        const int sec = static_cast<int>((ms % 60000) / 1000);
        if (h > 0)
            return QStringLiteral("%1:%2:%3")
                       .arg(h)
                       .arg(m, 2, 10, QChar('0'))
                       .arg(sec, 2, 10, QChar('0'));
        return QStringLiteral("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
    };

    QString overview;
    const int maxScenes = qMin(15, repr->scenes.size());
    for (int i = 0; i < maxScenes; ++i) {
        const Scene& s = repr->scenes[i];
        const QString desc = repr->sceneDescriptions.value(s.id);
        overview += QString::fromUtf8("- [%1-%2] 场景%3%4\n")
                        .arg(msToTime(s.startMs))
                        .arg(msToTime(s.endMs))
                        .arg(s.id)
                        .arg(desc.isEmpty() ? QString{} : QStringLiteral(": ") + desc.left(60));
    }
    if (repr->scenes.size() > maxScenes) {
        overview += tr("...（共 %1 个场景）\n").arg(repr->scenes.size());
    }
    ctx.sceneOverview = overview;
    ctx.videoSummary  = repr->videoSummary;
    return ctx;
}

// ============================================================
// oneShotVLM
// ============================================================

void VideoAnalysisService::oneShotVLM(const QString& sysPrompt,
                                       const QString& userText,
                                       const QList<QImage>& frames,
                                       bool interactive,
                                       const QString& cancellationKey,
                                       std::function<void(const QString&)> onDone)
{
    if (!m_vlmChannel) {
        if (onDone) onDone({});
        return;
    }
    m_vlmChannel->enqueue(sysPrompt, userText, frames,
                           interactive ? OneShotVlmChannel::Priority::Interactive
                                       : OneShotVlmChannel::Priority::Background,
                           cancellationKey, std::move(onDone));
}

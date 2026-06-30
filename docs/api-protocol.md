# API 通信协议 — 视频分析 AI Agent

> 客户端与 AI 后端的通信协议约定。采用 **OpenAI Compatible 格式**，确保可灵活切换不同的大模型服务。

---

## 一、协议概览

### 1.1 设计原则

1. **兼容 OpenAI**：直接复用业界标准，不发明新协议
2. **流式优先**：所有对话请求默认使用 SSE 流式返回
3. **多模态支持**：文本 + 图片帧（base64 编码）
4. **Tool Calling**：标准的 function calling 机制
5. **错误透明**：错误信息要让用户能理解

### 1.2 支持的后端

| 后端 | Endpoint 示例 | 备注 |
|------|--------------|------|
| OpenAI | `https://api.openai.com/v1` | 标准 |
| 通义千问 | `https://dashscope.aliyuncs.com/compatible-mode/v1` | 支持 Qwen-VL |
| DeepSeek | `https://api.deepseek.com/v1` | 支持 vision |
| 智谱 | `https://open.bigmodel.cn/api/paas/v4` | 支持 GLM-4V |
| Ollama | `http://localhost:11434/v1` | 本地部署 |
| 自部署 vLLM | 自定义 | 兼容 OpenAI 协议 |

### 1.3 通用约定

- 所有请求路径相对于 `base_url`（如 `/chat/completions`）
- 鉴权：`Authorization: Bearer <API_KEY>` Header
- Content-Type: `application/json`
- 编码: UTF-8
- 时间字段统一使用毫秒时间戳

---

## 二、核心接口：对话流式生成

### 2.1 请求格式

**Endpoint**: `POST {base_url}/chat/completions`

```json
{
  "model": "gpt-4o",
  "stream": true,
  "temperature": 0.7,
  "max_tokens": 2048,
  "messages": [
    {
      "role": "system",
      "content": "你是一个专业的视频内容分析智能体..."
    },
    {
      "role": "user",
      "content": [
        {
          "type": "text",
          "text": "视频里那个人在做什么？"
        },
        {
          "type": "image_url",
          "image_url": {
            "url": "data:image/jpeg;base64,/9j/4AAQSkZJRgABA...",
            "detail": "auto"
          }
        }
      ]
    }
  ],
  "tools": [...]
}
```

### 2.2 消息格式说明

#### 文本消息

```json
{
  "role": "user",
  "content": "什么时候出现了红色的车？"
}
```

#### 多模态消息（文本 + 单帧）

```json
{
  "role": "user",
  "content": [
    { "type": "text", "text": "画面里有几个人？" },
    {
      "type": "image_url",
      "image_url": {
        "url": "data:image/jpeg;base64,...",
        "detail": "low"
      }
    }
  ]
}
```

#### 多模态消息（多帧时序）

```json
{
  "role": "user",
  "content": [
    { "type": "text", "text": "以下是视频中按时间顺序的5帧（时间戳标注在每帧后）：" },
    { "type": "image_url", "image_url": { "url": "data:image/jpeg;base64,..." } },
    { "type": "text", "text": "[00:15]" },
    { "type": "image_url", "image_url": { "url": "data:image/jpeg;base64,..." } },
    { "type": "text", "text": "[00:18]" },
    { "type": "image_url", "image_url": { "url": "data:image/jpeg;base64,..." } },
    { "type": "text", "text": "[00:21]" },
    { "type": "text", "text": "请分析这个人物的动作过程。" }
  ]
}
```

#### Assistant 历史回复

```json
{
  "role": "assistant",
  "content": "在 [00:15] 至 [00:21] 期间，画面中的人物完成了从坐姿站起的动作..."
}
```

### 2.3 图片编码规范

| 项 | 说明 |
|----|------|
| 格式 | JPEG（推荐）或 PNG |
| 编码 | base64，前缀 `data:image/jpeg;base64,` |
| 分辨率 | 推荐 ≤ 1024px（按比例缩放） |
| 质量 | 75-85（jpeg quality） |
| 单帧大小 | ≤ 300KB（base64 后） |
| 单次请求总图片数 | ≤ 10 帧 |
| `detail` 字段 | `"low"`/`"auto"`/`"high"`，控制处理精度和成本 |

**客户端处理流程**：
```
原始帧 (1920x1080 RGB)
  ↓ 缩放
1024x576 RGB
  ↓ JPEG 编码 (quality=80)
~80KB
  ↓ base64
~108KB 字符串
  ↓ 拼接前缀
data:image/jpeg;base64,...
```

### 2.4 流式响应格式（SSE）

**HTTP Headers**:
```
Content-Type: text/event-stream
Cache-Control: no-cache
Connection: keep-alive
```

**响应体格式**：每行以 `data: ` 开头的 JSON，以 `\n\n` 分隔。

```
data: {"id":"chatcmpl-xxx","object":"chat.completion.chunk","created":1716800000,"model":"gpt-4o","choices":[{"index":0,"delta":{"role":"assistant","content":""},"finish_reason":null}]}

data: {"id":"chatcmpl-xxx","object":"chat.completion.chunk","created":1716800000,"model":"gpt-4o","choices":[{"index":0,"delta":{"content":"视频"},"finish_reason":null}]}

data: {"id":"chatcmpl-xxx","object":"chat.completion.chunk","created":1716800000,"model":"gpt-4o","choices":[{"index":0,"delta":{"content":"中的人物"},"finish_reason":null}]}

data: {"id":"chatcmpl-xxx","object":"chat.completion.chunk","created":1716800000,"model":"gpt-4o","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}

data: [DONE]
```

**关键字段**：
- `choices[0].delta.content`: 本次增量文本（拼接得到完整回复）
- `choices[0].delta.tool_calls`: 工具调用增量（见 Tool Calling 章节）
- `choices[0].finish_reason`: `null`（继续）/ `"stop"`（结束）/ `"tool_calls"`（要调工具）/ `"length"`（达到最大 token）
- `data: [DONE]`: 流式结束标记

### 2.5 客户端 SSE 解析伪代码

```cpp
void NetworkClient::parseSSEChunk(const QByteArray& chunk) {
    m_buffer.append(chunk);

    int eventEnd;
    while ((eventEnd = m_buffer.indexOf("\n\n")) != -1) {
        QByteArray event = m_buffer.left(eventEnd);
        m_buffer.remove(0, eventEnd + 2);

        // 解析 data: 行
        for (const QByteArray& line : event.split('\n')) {
            // SSE 协议中以 ':' 开头的行是注释/心跳（如 OpenAI 偶尔会发 ": OPENROUTER PROCESSING"），
            // 这里直接忽略。其他非 "data: " 前缀的行也按协议忽略。
            if (!line.startsWith("data: ")) continue;
            
            QByteArray payload = line.mid(6);
            if (payload == "[DONE]") {
                emit streamFinished();
                return;
            }
            
            // 解析 JSON
            QJsonDocument doc = QJsonDocument::fromJson(payload);
            QJsonObject obj = doc.object();
            QString delta = obj["choices"].toArray()[0].toObject()
                          ["delta"].toObject()["content"].toString();
            if (!delta.isEmpty()) {
                emit contentDelta(delta);
            }
            
            // 检查 tool_calls
            QJsonArray toolCalls = obj["choices"].toArray()[0].toObject()
                                  ["delta"].toObject()["tool_calls"].toArray();
            if (!toolCalls.isEmpty()) {
                emit toolCallDelta(toolCalls);
            }
        }
    }
}
```

---

## 三、Tool Calling 协议

### 3.1 工具定义（请求中携带）

```json
{
  "model": "gpt-4o",
  "messages": [...],
  "tools": [
    {
      "type": "function",
      "function": {
        "name": "seek_and_analyze",
        "description": "跳转到视频指定时间点，截取画面并进行视觉分析",
        "parameters": {
          "type": "object",
          "properties": {
            "timestamp_ms": {
              "type": "integer",
              "description": "目标时间点（毫秒）"
            },
            "focus": {
              "type": "string",
              "description": "分析关注点，如'关注画面中的人物动作'"
            }
          },
          "required": ["timestamp_ms"]
        }
      }
    },
    {
      "type": "function",
      "function": {
        "name": "analyze_time_range",
        "description": "分析一个时间区间内的视频内容，采样多帧",
        "parameters": {
          "type": "object",
          "properties": {
            "start_ms": { "type": "integer" },
            "end_ms": { "type": "integer" },
            "sample_count": { "type": "integer", "default": 5 },
            "focus": { "type": "string" }
          },
          "required": ["start_ms", "end_ms"]
        }
      }
    },
    {
      "type": "function",
      "function": {
        "name": "search_video_content",
        "description": "在视频中搜索符合描述的画面，返回匹配的时间点列表",
        "parameters": {
          "type": "object",
          "properties": {
            "query": { "type": "string" },
            "top_k": { "type": "integer", "default": 5 },
            "time_range": {
              "type": "array",
              "items": { "type": "integer" },
              "description": "可选的时间范围 [start_ms, end_ms]"
            }
          },
          "required": ["query"]
        }
      }
    },
    {
      "type": "function",
      "function": {
        "name": "get_transcript",
        "description": "获取指定时间区间的语音转文字内容",
        "parameters": {
          "type": "object",
          "properties": {
            "start_ms": { "type": "integer" },
            "end_ms": { "type": "integer" }
          },
          "required": ["start_ms", "end_ms"]
        }
      }
    },
    {
      "type": "function",
      "function": {
        "name": "get_scene_info",
        "description": "获取指定场景或时间点所在场景的详细信息（场景边界、关键帧描述、相关实体等）。",
        "parameters": {
          "type": "object",
          "properties": {
            "scene_id": {
              "type": "integer",
              "description": "场景 ID；若为 null 则用 timestamp_ms 定位所在场景"
            },
            "timestamp_ms": {
              "type": "integer",
              "description": "时间点（毫秒），用于按时间定位所在场景"
            }
          }
        }
      }
    },
    {
      "type": "function",
      "function": {
        "name": "control_player",
        "description": "控制播放器的播放/暂停/跳转，用于向用户展示特定时间点（与 seek_and_analyze 不同，本工具不会触发分析）。",
        "parameters": {
          "type": "object",
          "properties": {
            "action": {
              "type": "string",
              "enum": ["seek", "play", "pause"],
              "description": "要执行的动作"
            },
            "timestamp_ms": {
              "type": "integer",
              "description": "当 action=seek 时的目标时间点（毫秒）"
            }
          },
          "required": ["action"]
        }
      }
    }
  ],
  "tool_choice": "auto"
}
```

> 工具集与 `agent-core-design.md` §7.1 完全一致，共 6 个。任何新增/修改必须同步两处文档和 `development-plan.md` M4 任务列表。

### 3.2 工具调用响应（流式）

模型决定调工具时，流式响应中会逐步出现 `tool_calls`：

```
data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_abc123","type":"function","function":{"name":"seek_and_analyze","arguments":""}}]}}]}

data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"timestamp_ms\":"}}]}}]}

data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"120000,\"focus\""}}]}}]}

data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":":\"人物动作\"}"}}]}}]}

data: {"choices":[{"delta":{},"finish_reason":"tool_calls"}]}

data: [DONE]
```

客户端需要拼接 `arguments` 字段（每次是增量片段），最终得到完整的 JSON 参数。

### 3.3 工具结果回传

客户端执行工具后，将结果作为 `tool` 角色消息追加，再次请求：

```json
{
  "model": "gpt-4o",
  "stream": true,
  "messages": [
    { "role": "system", "content": "..." },
    { "role": "user", "content": "视频后半段那个穿蓝色衣服的人做了什么？" },
    {
      "role": "assistant",
      "content": null,
      "tool_calls": [
        {
          "id": "call_abc123",
          "type": "function",
          "function": {
            "name": "search_video_content",
            "arguments": "{\"query\":\"穿蓝色衣服的人\",\"time_range\":[180000,360000]}"
          }
        }
      ]
    },
    {
      "role": "tool",
      "tool_call_id": "call_abc123",
      "content": "[{\"timestamp_ms\":200000,\"description\":\"穿蓝色衬衫的男性走进办公室\"},{\"timestamp_ms\":255000,\"description\":\"该男性坐在桌前打电话\"}]"
    }
  ],
  "tools": [...]
}
```

### 3.4 客户端 Tool 执行循环（伪代码）

```cpp
async function agentLoop(question) {
    messages = [systemPrompt, userMessage(question)];
    
    for (round = 0; round < MAX_ROUNDS; round++) {
        response = await streamChat(messages, tools);
        messages.append(response.assistantMessage);
        
        if (response.finishReason == "stop") {
            return response.content;  // 生成完成
        }
        
        if (response.finishReason == "tool_calls") {
            for (toolCall of response.toolCalls) {
                result = await executeTool(toolCall.name, toolCall.arguments);
                messages.append({
                    role: "tool",
                    tool_call_id: toolCall.id,
                    content: JSON.stringify(result)
                });
            }
            continue;  // 进入下一轮，让模型基于工具结果生成
        }
    }
    
    // 超过最大轮数，强制要求生成回答
    return forceGenerateAnswer(messages);
}
```

---

## 四、System Prompt 规范

### 4.1 完整 System Prompt 模板

```
你是一个专业的视频内容分析智能体。你能够理解视频的视觉内容、音频内容、
时间结构和语义关系。

# 当前视频信息
- 文件名: {file_name}
- 总时长: {duration_str}
- 分辨率: {width}x{height}
- 帧率: {fps} fps
- 是否有音频: {has_audio}

# 视频结构概览
{scene_overview}

# 已生成的视频摘要
{video_summary}

# 能力边界
- 你的视频理解基于结构化的场景描述和关键帧分析
- 当用户问及画面细节时，使用工具查看具体帧
- 当只有文字描述时，基于这些描述进行推理

# 推理原则
1. 区分确定与推测：明确标注哪些是直接观察到的事实，哪些是推断
2. 时间意识：始终关注事件的时间顺序和持续时长
3. 多模态融合：综合视觉信息和语音信息做判断
4. 主动求证：信息不足时使用工具查询，不要凭空猜测

# 回答格式
- 用 Markdown 格式
- 引用时间点使用 [mm:ss] 或 [hh:mm:ss] 格式（如 [01:23]、[01:23:45]）
- 客户端会自动将时间戳渲染为可点击的跳转链接
- 不确定的内容明确标注"推测"或"可能"

# 工具使用指南
- 用户问及具体画面内容时 → 用 seek_and_analyze 或 analyze_time_range
- 用户搜索某种画面时 → 用 search_video_content
- 用户问及对话/语音内容时 → 用 get_transcript
- 信息已充分时直接回答，不要冗余调用工具
- 单次回答最多使用 3 次工具调用
```

### 4.2 时间戳格式约定

为了让客户端能识别并渲染为可点击链接，模型必须遵守：

| 视频时长 | 格式 | 示例 |
|---------|------|------|
| < 1 小时 | `[mm:ss]` | `[03:45]` |
| ≥ 1 小时 | `[hh:mm:ss]` | `[01:23:45]` |
| 时间区间 | `[mm:ss] - [mm:ss]` | `[01:20] - [02:15]` |

客户端用正则识别：`\[(\d{1,2}:)?\d{1,2}:\d{2}\]`

---

## 五、Embedding API 协议（M4 阶段）

如果使用云端 embedding 服务而非本地模型：

### 5.1 文本 Embedding

**Endpoint**: `POST {base_url}/embeddings`

```json
{
  "model": "text-embedding-3-small",
  "input": [
    "一个男人在厨房做饭",
    "两个人在公园散步"
  ],
  "encoding_format": "float"
}
```

**响应**:
```json
{
  "object": "list",
  "data": [
    {
      "object": "embedding",
      "embedding": [-0.0023, 0.0148, ...],
      "index": 0
    },
    {
      "object": "embedding",
      "embedding": [0.0089, -0.0231, ...],
      "index": 1
    }
  ],
  "model": "text-embedding-3-small",
  "usage": { "prompt_tokens": 12, "total_tokens": 12 }
}
```

### 5.2 视觉 Embedding

本地用 CLIP ONNX 实现，无 API 协议（详见 `agent-core-design.md`）。

---

## 六、错误处理

### 6.1 HTTP 状态码

| 状态码 | 含义 | 客户端处理 |
|--------|------|-----------|
| 200 | 成功 | 正常处理流式响应 |
| 400 | 请求格式错误 | 显示具体错误，可能是模型不支持该请求 |
| 401 | API Key 错误 | 提示用户检查 API 配置 |
| 403 | 权限不足 | 提示用户检查账号权限/余额 |
| 404 | 模型不存在 | 提示用户检查模型名称 |
| 429 | 速率限制 | 退避重试（指数退避，最多3次） |
| 500/502/503 | 服务端错误 | 自动重试1次，仍失败提示用户 |
| 504 | 超时 | 提示用户网络问题 |

### 6.2 错误响应格式

```json
{
  "error": {
    "message": "Incorrect API key provided",
    "type": "invalid_request_error",
    "param": null,
    "code": "invalid_api_key"
  }
}
```

### 6.3 流式中断处理

```
场景: SSE 流传输到一半连接断开

客户端行为:
1. 检测到 readyRead 长时间无数据（超时 30s）→ 主动断开
2. 检测到连接异常关闭 → 标记当前消息为"已中断"
3. 在 UI 上显示"消息已中断，是否重新生成？"
4. 已接收的部分内容保留显示
5. 不要自动重试（避免重复扣费）
```

### 6.4 客户端错误展示规范

| 错误类型 | UI 展示 |
|---------|--------|
| 网络断开 | 红色提示条："网络连接失败，请检查网络" |
| API Key 错误 | 弹窗：跳转到设置页修改 |
| 模型超载 (429) | 黄色提示："服务繁忙，正在重试..." |
| 视频文件错误 | 中央提示："无法打开此视频文件" |
| 帧分析失败 | 在消息气泡内显示："图像分析失败，已基于文字回答" |

---

## 七、客户端实现要点

### 7.1 QNetworkAccessManager 配置

```cpp
m_nam = new QNetworkAccessManager(this);

QNetworkRequest req(endpoint);
req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
// 安全：trim() 防止用户从网页复制时带入空白/回车导致构造非法 header（CRLF 注入）
const QString cleanKey = apiKey.trimmed();
req.setRawHeader("Authorization", QString("Bearer %1").arg(cleanKey).toUtf8());
req.setRawHeader("Accept", "text/event-stream");
req.setTransferTimeout(60000);  // 60s 超时

// 流式请求关键：监听 readyRead 而不是 finished
QNetworkReply* reply = m_nam->post(req, payload);
connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
    QByteArray chunk = reply->readAll();
    parseSSEChunk(chunk);
});
```

### 7.2 取消正在进行的请求

```cpp
void AgentService::stopGeneration() {
    if (m_activeReply) {
        m_activeReply->abort();
        m_activeReply->deleteLater();
        m_activeReply = nullptr;
    }
    emit generationStopped();
}
```

### 7.3 图片编码

```cpp
QString encodeImageForLLM(const QImage& image) {
    // 缩放到 1024 以内
    QImage scaled = image.width() > 1024 || image.height() > 1024
        ? image.scaled(1024, 1024, Qt::KeepAspectRatio, Qt::SmoothTransformation)
        : image;
    
    // 转 JPEG，quality=80
    QByteArray jpegData;
    QBuffer buffer(&jpegData);
    buffer.open(QIODevice::WriteOnly);
    scaled.save(&buffer, "JPEG", 80);
    
    // base64 编码 + 添加 data URI 前缀
    return QString("data:image/jpeg;base64,%1").arg(jpegData.toBase64());
}
```

### 7.4 时间戳点击识别（Markdown 渲染后）

```cpp
// 在 ChatBubbleWidget 渲染 Markdown 后，用正则匹配时间戳
QRegularExpression re(R"(\[((\d{1,2}):)?(\d{1,2}):(\d{2})\])");
// 将匹配到的 [mm:ss] 替换为带 hover/click 样式的 span
// 点击时计算毫秒数，emit timestampClicked(ms);
```

---

## 八、API 配置项

### 8.1 非敏感配置（持久化到 SQLite `settings` 表）

```
key                          | example                           | description
-----------------------------|----------------------------------|----------------------
llm.endpoint                 | https://api.openai.com/v1        | API base URL
llm.model                    | gpt-4o                           | 默认模型
llm.temperature              | 0.7                              | 生成温度
llm.max_tokens               | 2048                             | 最大输出 token
llm.timeout_sec              | 60                               | 请求超时
llm.max_tool_rounds          | 5                                | 最大工具调用轮数

embed.endpoint               | https://api.openai.com/v1        | embedding API
embed.model                  | text-embedding-3-small           | embedding 模型
embed.use_local              | false                            | 是否用本地模型

image.max_size               | 1024                             | 单帧最大边长
image.jpeg_quality           | 80                               | JPEG 质量
image.max_per_request        | 10                               | 单次请求最多帧数
```

### 8.2 敏感凭证（走 OS 密钥管理服务，**不入** SQLite）

| 凭证 | 名称（kv key） | 存储位置 |
|------|--------------|---------|
| LLM API Key | `secret.llm.api_key` | Windows DPAPI / macOS Keychain / Linux libsecret |
| Embedding API Key | `secret.embed.api_key` | 同上 |

> **安全规则（参见项目 `security_rules`）**：禁止在数据库 / 配置文件 / 日志中以任何形式存储或打印 API Key。
> 所有密钥的读写通过 `SettingsService::secretGet(name) / secretSet(name, value)` 抽象，
> 内部按平台分发到对应密钥管理 API。导出/备份对话历史时也必须脱敏。

---

## 九、调试与日志

### 9.1 日志级别

| 级别 | 内容 |
|------|------|
| TRACE | 完整请求体、响应流每个 chunk |
| DEBUG | 请求 URL、参数摘要、工具调用 |
| INFO | 对话开始/结束、token 使用量 |
| WARN | 重试、降级、超时 |
| ERROR | 失败的请求、解析错误 |

### 9.2 调试模式

设置开关 `app.debug.log_full_requests = true` 时：
- 完整记录每次 LLM 请求与响应
- **必须**对 `Authorization` header、payload 中任何包含 `api_key` / `token` 的字段做脱敏（替换为 `***`）
- 图片 base64 仅记录前 64 字符 + 总长度，避免日志文件爆炸
- 保存到 `%APPDATA%/FrameMind/logs/llm-trace-{date}.jsonl`
- 单行 JSON 格式，便于事后分析

---

## 十、版本兼容性

```
协议版本: v1.0 (基于 OpenAI Chat Completions 2024-03 版本)

未来扩展点（v1.1+）:
- 视频原生输入（OpenAI 计划支持，待协议公开）
- 音频原生输入（whisper API 集成到 chat completions）
- Reasoning 模型的思考过程展示
- 并行工具调用（parallel_tool_calls）
```

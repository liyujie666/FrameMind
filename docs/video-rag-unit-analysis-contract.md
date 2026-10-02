# 单元理解请求与上下文接口合同

日期：2026-10-02  
实现范围：性能实施计划 P0～P5 的代码接口；P4～P5 本轮仅完成代码与静态审查，未运行构建或测试。

## 请求身份与终态

`src/model/unit_analysis_request.h` 定义 `UnitAnalysisRequest`：

| 字段 | 合同 |
|---|---|
| context | 现有 VideoBuildContext，包含 buildId、taskGeneration、取消标记及 cancellationKey |
| unitId / pageId | 本次构建中的单元、页身份 |
| pageOrdinal | 本单元内从 0 开始的页序号 |
| attempt | 0 为首次请求，1 为唯一重试 |
| requestId | 每次尝试新建 UUID，包括重试；不复用旧请求 ID |
| workerId | 本单元独占的工作通道，直到所有页面进入终态 |

协调器同时校验构建、generation、requestId、页序和 attempt，终态回复最多处理一次。图片准备回调也校验构建与请求 ID。迟到／重复回复不推进页序、不改变覆盖、不触发第二次总结。

`UnitPageAnalysisResult` 保存 pageId、原始 sourceIds、状态、标题、正文、辅助描述、有效事实和错误；属于构建内存态，不新增 SQL 表。`UnitAnalysisTask` 独立拥有页游标、attempt、重试原因、carry、结果和 coverage。

## 通道与生命周期

`OneShotVlmChannel::enqueueRequest(requestId, system, text, frames, priority, cancellationKey, done)` 是新增的显式请求接口。

- `cancelRequest(requestId)` 只取消匹配的排队／在途请求，幂等。
- 显式请求正常完成、失败、超时或取消均通过 `done(ModelReply)` 提供一次终态；取消错误以 `cancelled:` 开头。
- 旧 `enqueue/enqueueDetailed/cancelBackground` 保持兼容；旧构建级取消仍允许静默丢弃回复。
- 对象销毁停止请求和定时器；已销毁接收者不会被异步回调访问。销毁后不承诺向已消失的调用方发送回调。

`UnitAnalysisWorkerPool` 使用最多三套独立的 NetworkClient → AgentService → OneShotVlmChannel。对象与协调器位于同一线程，依靠异步网络重叠；不存在单个 AgentService 多流复用。

`submit()` 只能在对应槽位空闲且单元／构建绑定一致时接受请求；`releaseUnit()` 只释放没有在途请求的对应单元；`cancelBuild()` 清理该构建的全部绑定并终止在途请求。工作池析构时终止请求，内部成员按 channel → agent → network 顺序销毁。

运行配置 `video_rag.unit_concurrency` 默认 3，启动时读取，限于 1～3。构建开始理解时冻结额度，不改变当前原始证据或派生计划指纹。DI 保留原有聊天和后台／局部视觉通道；只有单元页请求进入新池。

仅传入旧单通道、未装配工作池的协调器构造入口按额度 1 运行，同样使用显式 requestId 与请求级取消；不能把提交多个排队请求误当成真并发。

## 单页输入与输出

保留现有 `page_id`、`unit_id`、`core_evidence` 字段，新增：

```json
{
  "unit": {"unit_id":"u1","kind":"topic","start_ms":0,"end_ms":60000},
  "page": {"page_id":"u1:page:0","ordinal":0,"total":3},
  "carry_context": {
    "topic":"",
    "current_state":{"text":"","fact_refs":[]},
    "key_facts":[],
    "pending_threads":[],
    "uncertainties":[],
    "last_successful_page":-1,
    "gap_count":0
  }
}
```

有图页通过图片参数传输一张网格 JPEG，并在文本对象内增加 `grid_cells`（label、source_id、pts_ms、row、column、cell_rect、image_rect）、`grid_empty_cells`、`grid_image_size` 和 `grid_version`；纯文本页图片数为零。文本证据的来源、时间和偏移不改变。

`UnitFrameEvidence` 显式绑定 sourceId/path/PTS/稳定原始顺序，不能将混合文字／帧的 sourceIds 列表当作逐帧映射。后台只读取图片头规划全部页；格内有效短边低于配置目标时，顺序拆为更小的连续子页。原始证据条目及文字偏移逐条分配一次，文字按原始时间段中心与子页帧时间中心的距离归属，不重写其实际时间。

网格为 1×1、2×1、2×2 或 2 列×3 行，保持图像比例，不裁剪；标签在独立顶部区域，空格以灰色交叉标记。标签采用程序绘制的固定字形，不依赖后台线程访问 GUI 字体数据库。默认最大边长 2048、JPEG 85、格内短边目标 480、标签高度 32。小于 480 的原始帧仅要求保留原有像素，不通过放大宣称提高可读性。

`ImageEncodingOptions` 从 UnitAnalysisRequest 贯穿工作池／单通道至 AgentService，携带后台生成的 JPEG 字节及最终尺寸；单元理解设置 requirePrepared，发送层验证数量和尺寸，不再套用旧 1024 缩放。其他调用方默认仍为 1024/JPEG80。

配置 `video_rag.grid_max_edge/grid_jpeg_quality/grid_min_cell_short_edge/grid_label_height/grid_max_encoded_bytes` 在启动时读取。字节限制 0 表示尚未配置具体提供商限制，不代表所有服务端无限制。明确字节上限会保守缩小画布并参与预拆页，最后仍验证实际 JPEG 字节；单帧仍不满足尺寸／清晰度或编码字节限制时按页失败保留原始来源，不能静默删帧或降低质量冒充成功。真实细节可读性需要另外验收。

主结果仍为 title、visual_description、audio_summary、非空 summary 和 facts 数组；facts 必须通过 `validatedFacts()` 的本页来源和类型校验。主结果无效最多重试一次。

同一次请求新增输出 `carry_context`：

```json
{
  "topic":"当前主题",
  "current_state":{"text":"已确认状态","fact_refs":["P0.F0"]},
  "key_fact_refs":["P0.F0"],
  "pending_threads":[],
  "uncertainties":[]
}
```

短编号统一使用零基 `P页序.F事实数组下标`。主 facts 全部验证通过后，程序建立原数组下标映射；这里不会过滤后重新编号。事实来源的真实 source_chunk_ids 完整保存。编号池只属于当前单元，禁止未来页及未知编号。

topic 最多 80 字符；current_state.text 最多 160 字符；current_state.fact_refs 和 key_fact_refs 各最多 4 项，合并去重后最多 4 个编号；pending_threads、uncertainties 各最多 2 项、每项最多 80 字符。非空已确认状态必须关联有效事实编号。

程序根据编号展开有效事实文本为下一页的 key_facts，补入 last_successful_page 和 gap_count。所有输入 carry 以 Compact JSON 序列化后 QString::size() 不超过 1000，包含展开文本、字段和进度；这是 UTF-16 字符计数口径，不是 token 或计费统计。

模型的 carry 是替换后的精简状态，不是历史正文追加。有效更新可以继续保留早期必要事实、改变当前状态和移除已解决事项。

## 降级与错误政策

| 情况 | 行为 |
|---|---|
| 主结果或本页事实无效 | 同页最多重试一次，使用相同输入 carry；失败响应不进入上下文 |
| carry 缺失、超长、类型／引用无效 | 主结果仍成功，不重发；沿用旧有效 carry，加少量当前已确认事实，按完整条目裁剪 |
| 新状态无法确认 | 不写入已确认状态；程序可增加上下文未通过校验的不确定提示 |
| 当前页最终失败 | 不产生事实，gap_count 增加；后页提示不推断缺失过程 |
| 下一页成功 | last_successful_page 更新为该页，gap_count 归零 |
| HTTP 429、服务端临时错误 | 非阻塞退避，消耗同一页唯一重试额度；默认至少等待 1000ms，尊重有效 Retry-After |
| Retry-After 大于 30000ms | 本次页请求进入最终失败，记录错误；不提前重试服务端要求的等待 |
| 其他 4xx（408 除外） | 直接最终失败，不重复提交永久错误 |
| 请求超时 | 只取消 requestId；不会调用构建级 key 误杀其他单元 |
| 用户取消／构建替换／模型签名变化 | 停止调度、取消全部相关通道及单元 watchdog；旧回调无效 |

纯 carry 降级计入成功页面和降级统计，不将单元或构建误标 Partial。原有上游能力缺口、分段降级、页失败及摘要失败仍沿用现有 Ready／Partial 政策。

## 完成屏障、进度与观测

全部页描述在理解开始时确定，图片只准备活跃页。每个单元最多一个在途页；重试和退避期间仍占用该单元的槽位。页面终态后才推进页序；单元终态后填充空闲槽位。

结果按 unitOrdinal 写入固定单元槽位；全部叶子任务完成后通过单次屏障，才追加父单元、准备摘要输入及执行原总结／发布分支。成功或最终失败页均增加完成计数，重试不重复计数；进度映射到现有理解阶段 45～85。

`ModelReply` 增加可选 httpStatus、retryAfterMs 和 diagnostics。NetworkClient 解析 Retry-After 秒数／HTTP 日期并传递 usage；AgentService 提供请求耗时、首字节、首内容和响应字节等元数据。usage 没有返回时标不可用。

manifest.artifacts.unit_analysis 保存合同版本、配置额度、最大实际在途数、阶段耗时、总页数／已处理页数、carry 降级数、重试数和限流数。发布时合并 artifacts，保留这些统计。逐页身份和紧凑请求诊断写日志，不含 API Key。

额外记录页面规划、读图、合成和编码耗时、拆页数量、实际 JPEG 尺寸／字节、通道排队及请求时间、首字节／首内容、主结果状态及 carry 降级。重试复用同一份 JPEG；准备统计只累计首次准备，重试记录标记 prepared_reused。

详细诊断写入 AppDataLocation 下 `diagnostics/video-rag/<build_id>/unit-analysis.jsonl`，manifest.artifacts 保存文件路径和可用／截断状态。单文件上限 16MiB；达到上限后停止文件写入并保留日志。写文件失败不改变理解／发布状态。未写入图片字节、请求 Prompt、carry 正文或 API Key；不会自动删除既有记录。

## 版本及后续接口

`VideoRAGBuildPlan.unitUnderstandingVersion` 默认 `unit_grid_carry_v2`，carryVersion=`carry_v1_1000`、gridVersion=`evidence_grid_v1`。合同、版本、布局关联配置、尺寸、质量和字节限制进入 JSON 和 fingerprint；并发额度只影响运行统计。旧 JSON 缺字段解码为 legacy 标识，避免旧派生结果命中新合同。

原始快照按文件、采样、ASR、文本／视觉向量能力及模型指纹复用；还检查全部原始帧 chunk 的路径存在，缺失时重新提取。网格参数变化重做派生理解，不因 C 变化重新提取帧。持久化仍使用现有 manifest JSON/artifacts，无新增 SQL 表或图片 BLOB。

本阶段提供独立页结果和完成屏障，后续内容质量方案可在单元完成入口使用这些结果进行综合。本阶段仍保留原有 fusedDescription 归并和总结行为，未实施章节、类型化笔记或 UI。carry 及页结果不支持跨进程断点恢复。

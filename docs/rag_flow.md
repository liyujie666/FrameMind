# FrameMind 视频 RAG 实际流程

更新：2026-10-01。本文描述默认类型驱动构建；设计与验收分别见
[类型策略设计](video-rag-type-strategy-design.md) 和 [实施记录](video-rag-implementation-status.md)。

## 打开与构建

PlayerViewModel 的 videoOpened 经 ChatViewModel 调用 VideoAnalysisService::onVideoOpened，
再调用 VideoRAGBuildCoordinator::start。同一个正在构建的视频重复打开不会增加流水线。
手动分析强制派生重建；指定类型通过 changeType；自动类型重建明确清除用户覆盖。

1. 校验文件身份，读取活动构建、原始快照、语义单元及用户类型覆盖。
2. 没有指定类型或兼容画像时，通过独立 FFmpeg 文件读取并 seek 到目标附近，探测五个分散位置画面及短音频，先分类。
3. 路由 Meeting / Interview / Lecture / Tutorial / Generic，并协商实际 ASR、文本向量、视觉向量能力。
4. 创建候选构建。文件及模型匹配、采样足够密集时复用原始证据；否则异步重新提取。
5. VideoIndexer 按计划提取。对话和课程先 ASR，其余先画面；完整转写以 60 秒区间解码。
   画面保存真实 PTS，SceneDetector 生成视觉镜头，不把镜头当语义分段。
6. SemanticUnitBuilder 根据文本上下文、停顿、主题或问答线索，以及课件/状态变化提出本地候选。
   模型只能在真实合法端点上校正，引用区间内真实原始段；非法结果修复一次，仍失败本地回退。
7. 课程/教程围绕单元开始、过程和结果补帧；建立 raw snapshot。
8. 分页分析全部核心证据，记录成功、失败、缺失能力。每个语义单元保存 visual/audio/fused 及专用事实。
9. 生成父单元、章节和全局摘要；短事实与摘要按 BGE 实际 token 限制拆分并索引。
10. 校验候选并事务提交；CAS 检查预期活动 build，成功后发布规范表示、更新 UI 并使旧 QA/断点失效。

取消立即产生 Cancelled 终态。失败、取消或发布冲突不会提前删除旧活动索引。
所有后台回调验证 generation、build、文件与取消上下文，旧模型响应不能提交。

## 查询与展开

QueryPlan 固定当前视频的 build、revision 和 raw snapshot；显式时间条件优先。
文本 dense、词面、CLIP、实体路径继续共用 RRF。对话和课程优先文本；模型指纹不匹配时跳过对应向量路径。
chunk 身份去重保留同时间不同事实；派生摘要/事实不增加独立互证计数。

命中单元后展开原始转写和帧；EvidenceComposer 将引文、实际时间、图片、版本及不足标记送入 Agent。
get_semantic_unit 按单元或时间读取原始证据分页，并可沿 previous/next 获取相邻步骤。
操作过程证据不够、单元 Partial 或精确视觉问题会要求 analyze_time_range 重新读取局部帧，
局部解码独立于播放位置，并验证视频及构建没有变化。

普通 Agent 与 workflow 共用检索、证据组合和工具；QA 引用 chunk/unit/build/revision，
checkpoint 恢复必须与活动版本一致。播放器时间线可切换镜头和语义单元。

## 数据与兼容

四个检索集合继续保留。raw snapshots 管理原始证据；builds 管理派生分析、计划、状态和指纹；
semantic units 与 unit links 保存时间及来源关系。活动版本通过 video_metadata.active_build_id 解析。
已发布构建与快照不可修改，重建创建新 build ID；首版 revision 为 1。
没有新活动构建时提供 legacy 读取，旧表示明确标为 Partial。

P3 叙事专用策略、专用 OCR、说话人分离和非语音音频分析未实现。
离线测试不代表真实模型效果达标，完整验证范围及限制见实施记录。

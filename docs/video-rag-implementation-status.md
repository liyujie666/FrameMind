# 类型驱动视频 RAG 实施记录

构建：`cmake -S . -B build -DFRAMEMIND_THIRD_PARTY_ROOT=<原工程 third_party>`。
离线验证：Qt bin 加入 PATH 后，`cmake --build build --config Debug`、`ctest --test-dir build -C Debug --output-on-failure`。

## 公共底座与版本化构建

- 第三方二进制路径通过 CMake cache 配置，默认路径不变。
- 共享画像、计划、构建上下文、状态和语义单元支持 JSON 与 Qt metatype。
- 原始快照、构建、单元及关系存储独立于活动版本。发布使用 SQLite 事务和预期活动版本检查；取消、失败保留活动版本。
- 独立文件探测、计划驱动提取与分块 ASR 在线程池执行；规范表示和数据库提交在主线程。
- `VideoAnalysisService` 的自动入口统一转发 `VideoRAGBuildCoordinator`；旧 L1 不再自动触发场景分析。
- 本地分段保留镜头；语义单元采用本地候选与受约束模型校正。证据分页逐页记录完成、失败和缺失能力。
- BGE 暴露未截断 token 数，按 tokenizer 的实际限制拆分入库文本。

## 当前验证

主程序 Debug 构建通过。QtTest 使用临时 SQLite 验证重复迁移、候选发布冲突、活动版本恢复、真实帧 PTS；验证五种策略映射、静态镜头多主题、分页覆盖、虚构来源拒绝与步骤关系。

真实模型质量、固定视频素材对照及全部构建开关矩阵须在最终验收记录中单独标明，不能用离线夹具通过代替。

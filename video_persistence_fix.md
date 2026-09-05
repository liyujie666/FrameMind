# 视频内容持久化修复补丁

## 问题
时间线、总结、字幕三个Tab在重新打开视频时没有加载已索引的内容。

## 根本原因
`VideoRepresentation`（包含场景、语音段、摘要、场景描述）只存在内存中，没有持久化。

## 已完成的修改
1. 数据库schema已更新（video_metadata和scene_descriptions表）
2. DatabaseManager已添加保存/加载方法
3. VideoIndexer::representation()已修改为从数据库加载

## 还需要手动完成的步骤

### 1. 在 VideoAnalysisService 中保存场景描述

找到 `commitSceneFusion` 方法中的 `emit sceneDescribed(...)` 语句，在它**之前**添加：

```cpp
// 保存场景描述到数据库
if (m_db) {
    m_db->saveSceneDescription(
        repr->videoId,
        fusion.sceneId,
        fusion.fusedDescription,
        fusion.visualDescription
    );
}
```

### 2. 在 VideoAnalysisService 中保存视频摘要

找到 `summarizeVideo` 方法中的 `emit summaryReady(summary)` 语句，在它**之前**添加：

```cpp
// 保存摘要到数据库
if (m_db) {
    m_db->saveVideoMetadata(
        repr->videoId,
        repr->metadata.filePath,
        summary,
        static_cast<int>(VideoRepresentation::Level2)
    );
}
```

### 3. 在 DIContainer 中更新依赖注入

找到 `DIContainer::initialize()` 或创建 `VideoIndexer` 和 `VideoAnalysisService` 的地方，
将 `DatabaseManager::instance()` 传递给它们的构造函数。

搜索：
```cpp
new VideoIndexer(
```

修改为：
```cpp
new VideoIndexer(
    playerService,
    sceneDetector,
    ragStore,
    DatabaseManager::instance(),  // 添加这行
    parent
)
```

类似地修改 `VideoAnalysisService` 的创建：
```cpp
new VideoAnalysisService(
    vlmChannel,
    indexer,
    ragStore,
    playerService,
    DatabaseManager::instance(),  // 添加这行
    parent
)
```

## 测试步骤

1. 编译并运行程序
2. 打开一个视频，等待索引完成（Level 1 + Level 2）
3. 查看时间线、总结、字幕是否显示
4. 关闭视频或退出程序
5. 重新打开同一个视频
6. **预期结果**：时间线、总结、字幕应该立即显示已索引的内容

## 如果还是不显示

检查日志中是否有：
```
[VideoIndexer] 从数据库加载视频表示 videoId=xxx 场景数=xxx 摘要长度=xxx
```

如果没有这条日志，说明数据库加载失败。检查：
1. 数据库中是否有数据：打开 `AppData/Local/FrameMind/framemind.db`
2. SQL是否执行成功

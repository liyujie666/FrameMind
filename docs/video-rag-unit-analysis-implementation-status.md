# 单元并发与滚动上下文实现及验收记录

日期：2026-10-02  
范围：[性能实施计划](video-rag-unit-understanding-performance-plan.md) P0～P3。P4 网格、P5 完整集成矩阵和 P6 真实模型对照尚未执行。

后续更新：P4～P5 代码和静态审查已完成，见[网格代码审查记录](video-rag-grid-code-review.md)。本文件的测试通过记录属于网格接入之前的 P0～P3；本轮未构建或测试，不能用这些旧记录证明当前代码通过运行验收。

## 基线回归

以当前工作区已有修改为基线，没有重置或覆盖用户的网络、超时、Agent、工具编排及 CLIP 修复。基线时已有 12 个已跟踪文件被修改，另有设计／计划文档和 `src/model/build_request_policy.h` 未跟踪。

环境：Windows、MSVC 2022 14.42、Qt 6.9.1、Ninja、Debug，ONNX=ON、Whisper=ON。实际构建目录为 `build/Desktop_Qt_6_9_1_MSVC2022_64bit-Debug`。

- 主程序及现有测试 target 构建通过。
- 基线 CTest 4/4 通过，耗时 5.75 秒，见 `build/unit-analysis-baseline-ctest.log`。
- 首次运行因 DLL 搜索路径不足而无法启动部分测试；补齐 Qt、MSVC Debug CRT 及 UCRT 路径后恢复。
- 模型通道测试调用 Windows DPAPI 存取虚构 fixture-secret；沙箱内设置测试密钥失败，沙箱外执行通过。测试仍只连接本地 HTTP 服务，使用临时 SQLite，不访问在线模型或用户业务数据库。

## 已实现

| 模块 | 结果 |
|---|---|
| 请求合同 | 新增 UnitAnalysisRequest 和 UnitPageAnalysisResult；完整身份与零基事实编号定义见[接口合同](video-rag-unit-analysis-contract.md) |
| 请求级取消 | 显式 requestId 支持排队／在途取消；幂等；显式请求提供终态；旧通道入口保持兼容 |
| 独立工作池 | 最多 3 个独立 NetworkClient／AgentService／通道；单元独占 worker；用户聊天与局部分析仍走现有链路 |
| 生产装配 | DIContainer 接入工作池，启动时读取 video_rag.unit_concurrency，默认 3，范围 1～3 |
| 单元调度 | 独立页序、attempt、重试原因、carry、结果和 coverage；异步网络重叠，页内严格串行 |
| 顺序及屏障 | 固定 unitOrdinal 写入结果；所有叶子任务终态后一次归并父单元并启动总结 |
| 图片准备 | 有界后台线程池只读取活跃页；返回主线程后校验构建及请求身份 |
| 上下文 | 程序解析事实短编号并展开原事实文本；1000 字符预算，字段数量／长度／引用校验及状态字段白名单 |
| 上下文降级 | carry 单独失败不重发、不误标 Partial；保留旧有效上下文并加入少量已确认事实；最终失败页记录缺口 |
| 重试及限流 | 主结果最多重试一次；429／临时服务错误使用非阻塞退避；读取 Retry-After，超过 30 秒预算直接最终失败 |
| 隔离及停止 | 单页 watchdog 只取消 requestId；整构建取消停止工作池与单元 watchdog；旧构建、重复及迟到回调不生效 |
| 版本及统计 | unit_carry_v1 进入计划指纹，旧 JSON 解码为 legacy_pages_v1；阶段统计合并到 artifacts，发布时保留 |

类型关注点、原始事实引用校验、分段校正 Prompt、提取／向量编码及事务发布路径继续沿用现有逻辑。本阶段未合成网格，没有增加记忆压缩模型调用，也未增加章节整理模型调用。

## 自动化验收

修改后主程序构建通过，CTest 4/4 通过；业务用例计入数据行共 34 个，不计 init／cleanup：

| 测试 target | 业务通过数 | 新增覆盖 |
|---|---:|---|
| video_rag_tests | 22 | carry 校验／预算／早期事实／事项退出／字段白名单；额度 1、2、3；页内串行；乱序和重复回复；重试 carry 不变；429 退避；超时隔离；失败缺口；旧构建页面回复丢弃 |
| video_model_channel_tests | 9 | 两个独立客户端真实网络重叠；指定请求取消；槽位复用；整构建取消及新构建复用；排队取消；429 与 Retry-After 元数据 |
| video_tokenizer_tests | 1 | 原 tokenizer 回归 |
| video_media_tests | 2 | 原取帧／音频回归 |

模型通道测试通过本地 QTcpServer 控制 SSE 延迟和停滞，实际观察第二个请求在第一个尚未结束时到达。协调器测试分别设置 1／2／3 额度，捕获每次提交的 carry、页序、attempt 和 requestId，验证实际在途数、唯一请求身份、覆盖及最终屏障。

修复并增加了对两个集成问题的检查：发布覆盖 artifacts 导致统计丢失；carry 单独降级通过普通 diagnostics 影响构建 Ready／Partial。现在 artifacts 使用合并写入，纯 carry 降级仅写日志和计数。

重建替换测试中的超长文字夹具会触发现有分段输入预算回退，因此可能保留 Partial；验收检查新构建的页覆盖完整且不存在旧 generation 的分析失败，不将其误判为取消隔离失败。

## 日志与复现

构建日志：`build/unit-analysis-build.log`。最终 CTest：`build/unit-analysis-ctest.log`。逐例 XML 和 LastTest.log 位于上述构建目录。build 目录中的日志属于本地验证产物。

PowerShell 运行测试前需要有效的运行时路径，例如当前环境：

```powershell
$env:PATH='D:\Qt\6.9.1\msvc2022_64\bin;D:\Visual Studio\Visual Studio2022\VC\Redist\MSVC\14.42.34433\debug_nonredist\x64\Microsoft.VC143.DebugCRT;D:\Windows Kits\10\bin\10.0.20348.0\x64\ucrt;'+$env:PATH
ctest --test-dir build/Desktop_Qt_6_9_1_MSVC2022_64bit-Debug --output-on-failure
```

编译需在 MSVC 开发环境中运行 `cmake --build build/Desktop_Qt_6_9_1_MSVC2022_64bit-Debug --parallel 3`。受限沙箱无法使用 DPAPI 时，模型通道测试需要允许其临时测试密钥的系统访问。

## 后续验收边界

尚未执行网格及专用图片编码、四种 ONNX／Whisper 组合的完整新矩阵、真实模型 A/B/C/D 对照、真实视频语义质量评估和 UI 人工验收。现有图片仍使用原始多图、1024 内 JPEG 编码。

本次结果确认 P0～P3 的代码和离线行为；不宣称实测提速比例、在线限流表现或重复描述减少程度。下一阶段按原计划进入 P4，并在最终对照中区分并发、carry 和网格的各自贡献。

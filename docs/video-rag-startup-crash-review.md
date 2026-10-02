# 启动加载 Manifest 崩溃排查

日期：2026-10-02。仅检查源码、已存在的构建元数据及用户启动的进程模块，未执行配置、构建、应用或测试。

## 发现

当前进程为 `build/Desktop_Qt_6_9_1_MSVC2022_64bit-Debug/FrameMind.exe`，Qt Core/Gui/Sql 使用配置中的 Qt 6.9.1 Debug DLL。

同一构建目录出现了结构定义变更后的混合目标文件：

- `src/model/video_rag_types.h`：16:05:44 修改，包含新增网格及 carry 配置成员。
- `video_rag_types.cpp.obj`：16:43:40，使用新定义。
- `video_rag_store.cpp.obj`：15:39:09，早于结构定义变更。
- `FrameMind.exe`：16:44:52。

只读 `ninja -t deps` 显示 store 目标文件为 `#deps 0`，types 目标文件为 `#deps 315`；多个其他应用目标文件也为零依赖。旧调用方按旧尺寸为 `VideoBuildManifest` 的返回对象分配空间，新解码函数使用新尺寸写入，是当前启动崩溃的强证据。异常代码和实际越界地址尚未取得，修复后运行结果未确认。

构建规则通过 MSVC `/showIncludes` 追踪头文件，当前 Ninja 前缀为中文。不同编译环境的输出语言与此前配置的前缀不一致，可导致 Ninja 丢失所有头文件依赖；历史构建输出未保留，无法进一步确认是哪一次环境切换引起。

## 修改

在 CMake 的 MSVC + Ninja 分支中，通过编译 launcher 固定 `VSLANG=1033`，同时固定 `/showIncludes` 解析前缀为 `Note: including file: `。保留已有 compiler launcher，其他生成器保持原配置。修改编译命令后，重新运行 CMake 并构建将使 Ninja 重新编译目标并收集头文件依赖。

必须重新生成构建规则并重新编译；修改 JSON 解码或删除历史数据无法修复混合对象文件。建议停止当前调试进程后，在 Qt Creator 重新运行 CMake，并清理、重新构建应用，以消除所有已有零依赖目标文件。无需删除数据库或原始证据。

只完成静态审查与 `git diff --check`，未执行上述运行验证。

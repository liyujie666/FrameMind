# FrameMind

基于 **Qt 6.9 + SmartPlayer SDK** 的视频分析 AI Agent 客户端。

> 设计文档见 [`docs/`](./docs)；开发任务卡见 [`docs/dev-tasks.md`](./docs/dev-tasks.md)。

## 当前进度

- **M1 — 骨架**：能打开本地 mp4，流畅播放、可拖动 seek、可调音量/倍速；三栏布局 + 空对话占位。
- **M2 — 单帧 AI 问答**：SSE 流式对话、Markdown 渲染、对话历史持久化（SQLite）、
  「📷 当前帧」截帧提问、多会话切换。首次使用需在「文件 → AI 设置」中填写
  Endpoint / 模型 / API Key（Key 经 Windows DPAPI 加密存于系统密钥库，**不入数据库/日志**）。

## 环境要求

| 依赖 | 版本 |
|------|------|
| Qt | 6.9（Core / Gui / Widgets / Network / Sql / Concurrent / Svg）|
| CMake | ≥ 3.20 |
| 编译器 | MSVC 2019 / 2022 (x64) |
| SmartPlayer SDK | 预编译产物，置于 `third_party/smartplayer_sdk/` |

`third_party/smartplayer_sdk/` 结构：

```
third_party/smartplayer_sdk/
├── include/   # smartplayer.h / smartplayercallback.h / smartplayerdefs.h
├── lib/       # SmartPlayerSDK.lib
└── bin/       # SmartPlayerSDK.dll + FFmpeg/SDL2 运行时 dll
```

## 编译

```powershell
# 在仓库根目录
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_PREFIX_PATH="C:/Qt/6.9.0/msvc2019_64"

cmake --build build --config Debug
```

> 把 `CMAKE_PREFIX_PATH` 改成你本机的 Qt 安装路径。

构建完成后，可执行文件位于 `build/Debug/FrameMind.exe`，运行所需的 SDK / FFmpeg / SDL2 dll 会在编译后自动拷贝到同目录。

## 运行

```powershell
./build/Debug/FrameMind.exe
```

菜单「文件 → 打开视频...」选择本地 mp4 即可播放。

## 目录结构

```
src/
├── main.cpp              # 入口
├── app/                  # Application + DIContainer（依赖注入）
├── view/                 # View 层（MainWindow / Sidebar / Player ...）
├── viewmodel/            # ViewModel 层
├── model/                # Domain Models + 公共类型
├── service/              # Service 层（PlayerService ...）
├── infrastructure/       # 基础设施（ImageProcessor / EventBus ...）
└── utils/
```

下面是完整的本地配置指南，分为 **库安装**（编译期）和 **模型下载**（运行期）两部分。按顺序操作即可。

---

## 一、安装 ONNX Runtime（编译期库）

这是 CLIP / BGE / TransNetV2 的共同依赖。

### 步骤

```powershell
# 1. 进入 third_party 目录
cd d:\lyj\projects\Frame_Mind\third_party

# 2. 下载 onnxruntime-win-x64-1.18.1.zip
#    浏览器打开：https://github.com/microsoft/onnxruntime/releases/tag/v1.18.1
#    下载 onnxruntime-win-x64-1.18.1.zip
#    或用 PowerShell：
Invoke-WebRequest -Uri "https://github.com/microsoft/onnxruntime/releases/download/v1.18.1/onnxruntime-win-x64-1.18.1.zip" -OutFile "onnxruntime.zip"

# 3. 解压并重命名
Expand-Archive -Path onnxruntime.zip -DestinationPath .
Rename-Item onnxruntime-win-x64-1.18.1 onnxruntime

# 4. 清理
Remove-Item onnxruntime.zip
```

### 验证目录结构

解压后 `third_party/onnxruntime/` 应该长这样：

```
third_party/onnxruntime/
├── include/              ← 头文件
│   ├── onnxruntime_cxx_api.h
│   ├── onnxruntime_c_api.h
│   └── ...
├── lib/
│   └── onnxruntime.lib   ← 链接库
└── bin/
    └── onnxruntime.dll   ← 运行时 DLL
```

---

## 二、安装 whisper.cpp（编译期库）

```powershell
# 1. 进入 third_party 目录
cd d:\lyj\projects\Frame_Mind\third_party

# 2. 克隆 whisper.cpp
git clone https://github.com/ggerganov/whisper.cpp.git

# 3. 不需要手动编译，CMake 的 add_subdirectory 会自动构建
```

验证：`third_party/whisper.cpp/` 下应有 `CMakeLists.txt` 和 `whisper.h` 等文件。

---

## 三、下载模型文件（运行期）

模型文件不放在 `third_party`（那是编译期库的目录），而是放在应用的 AppData 目录。代码会自动从 `%APPDATA%/FrameMind/models/` 查找。

### 创建模型目录

```powershell
# 创建模型存放目录
mkdir "$env:APPDATA\FrameMind\models" -Force
```

### 3.1 下载 CLIP ONNX 模型

CLIP 需要两个文件：视觉编码器 + 文本编码器。

```powershell
# 方法 A：用 Python 导出（推荐，可控）
# 需要安装 Python + pip install transformers onnxruntime torch

pip install transformers onnxruntime torch optimum

# 导出脚本
python -c @"
from transformers import CLIPVisionModelWithProjection, CLIPTextModelWithProjection, CLIPTokenizer
import torch

model_name = 'openai/clip-vit-base-patch32'

# 导出视觉编码器
vision_model = CLIPVisionModelWithProjection.from_pretrained(model_name)
vision_model.eval()
dummy = torch.randn(1, 3, 224, 224)
torch.onnx.export(
    vision_model, dummy,
    r'$env:APPDATA\FrameMind\models\clip_visual.onnx',
    input_names=['pixel_values'],
    output_names=['image_embeds'],
    dynamic_axes={'pixel_values': {0: 'batch'}},
    opset_version=14
)

# 导出文本编码器
text_model = CLIPTextModelWithProjection.from_pretrained(model_name)
text_model.eval()
dummy_ids = torch.zeros(1, 77, dtype=torch.long)
torch.onnx.export(
    text_model, dummy_ids,
    r'$env:APPDATA\FrameMind\models\clip_text.onnx',
    input_names=['input_ids'],
    output_names=['text_embeds'],
    dynamic_axes={'input_ids': {0: 'batch'}},
    opset_version=14
)
print('CLIP ONNX 导出完成')
"@

# 方法 B：直接下载社区导出的 ONNX（更快但不可控）
# https://huggingface.co/onnx-community/clip-vit-base-patch32-ONNX
# 下载 model.onnx → 重命名为 clip_visual.onnx
```

验证：
```powershell
dir "$env:APPDATA\FrameMind\models\clip_visual.onnx"   # ~350MB
dir "$env:APPDATA\FrameMind\models\clip_text.onnx"      # ~250MB
```

### 3.2 下载 BGE-small-zh ONNX 模型

```powershell
# 用 Python 导出
pip install optimum[onnxruntime]

python -c @"
from optimum.onnxruntime import ORTModel
from transformers import AutoTokenizer

model_id = 'BAAI/bge-small-zh-v1.5'

# 导出为 ONNX
model = ORTModel.from_pretrained(model_id, export=True)
model.save_pretrained(r'$env:APPDATA\FrameMind\models\bge-small-zh')

# 重命名为代码中期望的文件名
Move-Item "$env:APPDATA\FrameMind\models\bge-small-zh\model.onnx" "$env:APPDATA\FrameMind\models\bge-small-zh.onnx" -Force
Remove-Item "$env:APPDATA\FrameMind\models\bge-small-zh" -Recurse -Force
print('BGE ONNX 导出完成')
"@
```

或者直接从 HuggingFace 下载预导出的：
```powershell
# 直接下载
Invoke-WebRequest -Uri "https://huggingface.co/BAAI/bge-small-zh-v1.5/resolve/main/onnx/model.onnx" -OutFile "$env:APPDATA\FrameMind\models\bge-small-zh.onnx"
```

验证：
```powershell
dir "$env:APPDATA\FrameMind\models\bge-small-zh.onnx"   # ~100MB
```

### 3.3 下载 Whisper ggml 模型

```powershell
# 下载 ggml-small.bin (推荐，中文效果好，466MB)
Invoke-WebRequest -Uri "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin" -OutFile "$env:APPDATA\FrameMind\models\ggml-small.bin"

# 或者下载更小的 ggml-base.bin (142MB，精度略低但更快)
# Invoke-WebRequest -Uri "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin" -OutFile "$env:APPDATA\FrameMind\models\ggml-small.bin"
```

验证：
```powershell
dir "$env:APPDATA\FrameMind\models\ggml-small.bin"   # 466MB 或 142MB
```

---

## 四、CMake 配置

库和模型都准备好后，重新配置 CMake：

```powershell
cd d:\lyj\projects\Frame_Mind

# 清理旧的 build 缓存
Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue
mkdir build
cd build

# 配置：开启 ONNX + Whisper
cmake .. -G "Visual Studio 17 2022" -A x64 `
    -DFRAMEMIND_ENABLE_ONNX=ON `
    -DFRAMEMIND_ENABLE_WHISPER=ON

# 编译
cmake --build . --config Release
```

如果你用 IDE（如 CLion / VSCode），在 CMake Settings 里添加：
```
FRAMEMIND_ENABLE_ONNX=ON
FRAMEMIND_ENABLE_WHISPER=ON
```

---

## 五、验证安装

### 5.1 编译期验证

CMake 配置成功后，输出里应能看到：
```
-- FRAMEMIND_ENABLE_ONNX=ON
-- FRAMEMIND_ENABLE_WHISPER=ON
-- Found onnxruntime
-- Adding whisper subdirectory
```

### 5.2 运行期验证

程序启动后，在输出日志中应能看到：
```
[OnnxRuntime] 模型加载成功: .../models/clip_visual.onnx | inputs: 1 | outputs: 1
[OnnxRuntime] 模型加载成功: .../models/clip_text.onnx | inputs: 1 | outputs: 1
[ClipService] 初始化成功 | visual: ... | text: ...
[OnnxRuntime] 模型加载成功: .../models/bge-small-zh.onnx | inputs: 3 | outputs: 1
[EmbeddingService] 初始化成功: .../models/bge-small-zh.onnx
[WhisperService] 模型加载成功: .../models/ggml-small.bin
```

如果看到 `模型文件不存在` 或 `初始化失败`，检查模型路径是否正确。

### 5.3 DLL 验证

编译后可执行目录下应有 `onnxruntime.dll`：
```powershell
dir d:\lyj\projects\Frame_Mind\build\Release\onnxruntime.dll
```

---

## 六、最终目录结构总览

```
d:\lyj\projects\Frame_Mind\
├── third_party/
│   ├── smartplayer_sdk/          # 已有
│   ├── onnxruntime/               # ← 新增
│   │   ├── include/
│   │   │   └── onnxruntime_cxx_api.h
│   │   ├── lib/
│   │   │   └── onnxruntime.lib
│   │   └── bin/
│   │       └── onnxruntime.dll
│   └── whisper.cpp/              # ← 新增 (git clone)
│       ├── CMakeLists.txt
│       ├── whisper.h
│       └── ...

%APPDATA%/FrameMind/models/        # ← 运行期模型目录
├── clip_visual.onnx               # ~350MB
├── clip_text.onnx                 # ~250MB
├── bge-small-zh.onnx              # ~100MB
└── ggml-small.bin                 # ~466MB
```

---

## 七、分阶段实施建议

不要一次性全装，按开发节奏逐步来：

| 阶段 | 需要安装 | CMake 选项 | 需要下载的模型 |
|------|---------|-----------|--------------|
| **M3 场景分割** | 无 | 都不开 | 无（直方图差异纯算法） |
| **M4-1 CLIP+BGE** | ONNX Runtime | `FRAMEMIND_ENABLE_ONNX=ON` | `clip_visual.onnx` + `clip_text.onnx` + `bge-small-zh.onnx` |
| **M4-2 Whisper** | whisper.cpp | `FRAMEMIND_ENABLE_WHISPER=ON` | `ggml-small.bin` |
| **可选 TransNetV2** | ONNX Runtime | 已有 | `transnetv2.onnx`（自行导出） |

**建议先做 M3**（不需要任何模型库），验证 `SceneDetector` 的直方图差异能正常工作后，再逐步安装 ONNX Runtime 和 whisper.cpp。
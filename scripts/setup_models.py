#!/usr/bin/env python3
"""
FrameMind 模型权重一键配置脚本
================================

把下载好的 / 导出的模型权重放到 FrameMind 期望的位置，
并按 dicontainer.cpp 硬编码的文件名重命名。

FrameMind 期望的模型目录:
  %LOCALAPPDATA%\\FrameMind\\FrameMind\\models\\
    ├── clip_visual.onnx     (CLIP 视觉编码器, ~350MB)
    ├── clip_text.onnx       (CLIP 文本编码器, ~255MB)
    ├── clip_vocab.json      (CLIP BPE 词表, ~860KB)
    ├── clip_merges.txt      (CLIP BPE merge 规则, ~525KB)
    ├── bge-small-zh.onnx    (BGE-small-zh-v1.5 ONNX, ~100MB)
    ├── bge_tokenizer.json   (BGE WordPiece tokenizer)
    └── ggml-medium.bin      (Whisper medium, ~1.5GB)

用法:
  # 0. 【首次】在项目根创建 .venv，所有 Python 依赖都装在里面（不入 git）
  D:\\MiniConda\\python.exe -m venv .venv
  .venv\\Scripts\\activate
  python -m pip install --upgrade pip

  # 1. 安装 CLIP（假设你已用 hf download 下到 D:\\Downloads\\clip-onnx）
  python scripts\\setup_models.py --clip --clip-src D:\\Downloads\\clip-onnx

  # 2. 导出 BGE-small-zh ONNX（脚本会自动 pip install optimum + torch 到 .venv）
  python scripts\\setup_models.py --bge

  # 3. 下载 Whisper ggml-medium.bin
  python scripts\\setup_models.py --whisper

  # 4. 一键全套
  python scripts\\setup_models.py --all --clip-src D:\\Downloads\\clip-onnx

  # 5. 只验证已就位的文件
  python scripts\\setup_models.py --verify

  # 6. 自定义目标目录（默认 %LOCALAPPDATA%\\FrameMind\\FrameMind\\models）
  python scripts\\setup_models.py --all --clip-src D:\\Downloads\\clip-onnx --output E:\\my_models

说明:
  - 如果检测到项目根有 .venv\\Scripts\\python.exe，pip 与 optimum-cli 都会
    用 .venv 的 python，依赖全部装到项目内，不污染系统/MiniConda。
  - 没建 .venv 时，会装到当前跑脚本的 python 解释器对应的 site-packages。
  - 导出 BGE ONNX 是一次性操作，导出完之后 .venv 可整个删除（FrameMind
    运行时不需要 Python，只需要 .onnx 文件）。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import urllib.request
import urllib.error
from pathlib import Path


# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------

# FrameMind 期望的最终文件名（来自 dicontainer.cpp:97-109）
EXPECTED_FILES = {
    "clip_visual.onnx":  ("CLIP 视觉编码器",  300 * 1024 * 1024),  # >= 300MB
    "clip_text.onnx":    ("CLIP 文本编码器",   200 * 1024 * 1024),  # >= 200MB
    "clip_vocab.json":   ("CLIP BPE 词表",     500 * 1024),          # >= 500KB
    "clip_merges.txt":   ("CLIP BPE merge",    400 * 1024),          # >= 400KB
    "bge-small-zh.onnx": ("BGE-small-zh ONNX", 80 * 1024 * 1024),   # >= 80MB
    "bge_tokenizer.json":("BGE tokenizer",     100 * 1024),          # >= 100KB
    "ggml-medium.bin":   ("Whisper medium",   1400 * 1024 * 1024),  # >= 1.4GB
}

# CLIP 文件名映射：源文件名 -> 目标文件名
CLIP_FILE_MAP = {
    "vision_model.onnx": "clip_visual.onnx",
    "text_model.onnx":   "clip_text.onnx",
    "vocab.json":        "clip_vocab.json",
    "merges.txt":        "clip_merges.txt",
}

# BGE 源模型（HuggingFace）
BGE_SOURCE_MODEL = "BAAI/bge-small-zh-v1.5"

# Whisper ggml 下载
WHISPER_REPO = "ggerganov/whisper.cpp"
WHISPER_FILE = "ggml-medium.bin"
WHISPER_URL  = f"https://huggingface.co/{WHISPER_REPO}/resolve/main/{WHISPER_FILE}"


# ---------------------------------------------------------------------------
# 工具函数
# ---------------------------------------------------------------------------

def get_default_models_dir() -> Path:
    """返回 FrameMind 默认模型目录。

    优先级：
      1. 环境变量 FRAMEMIND_MODELS_DIR
      2. 项目根 ./models/                  ← 与 dicontainer.cpp 的 resolveModelsDir() 对齐
      3. <AppData>/FrameMind/FrameMind/models/  ← 兜底
    """
    # 1. 环境变量
    env = os.environ.get("FRAMEMIND_MODELS_DIR")
    if env:
        return Path(env)

    # 2. 项目根 ./models/（脚本在 scripts/ 下，项目根是上一级）
    project_root = Path(__file__).resolve().parent.parent
    project_models = project_root / "models"
    if project_models.exists() or project_root.exists():
        return project_models

    # 3. 兜底：<AppData>/models/
    if sys.platform == "win32":
        base = Path(os.environ.get("LOCALAPPDATA",
                                   Path.home() / "AppData" / "Local"))
        return base / "FrameMind" / "FrameMind" / "models"
    elif sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "FrameMind" / "models"
    else:
        return Path.home() / ".local" / "share" / "FrameMind" / "models"


def human_size(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} TB"


def download_with_progress(url: str, dest: Path) -> bool:
    """带进度条的下载"""
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "FrameMind/1.0"})
        with urllib.request.urlopen(req, timeout=60) as resp:
            total = int(resp.headers.get("Content-Length", 0))
            print(f"  URL:   {url}")
            print(f"  Size:  {human_size(total) if total else 'unknown'}")
            print(f"  Dest:  {dest}")

            downloaded = 0
            chunk = 1024 * 64
            last_pct = -1
            with open(dest, "wb") as f:
                while True:
                    data = resp.read(chunk)
                    if not data:
                        break
                    f.write(data)
                    downloaded += len(data)
                    if total:
                        pct = downloaded * 100 // total
                        if pct != last_pct:
                            bar = "#" * (pct // 2) + "-" * (50 - pct // 2)
                            sys.stdout.write(
                                f"\r  [{bar}] {pct:3d}% "
                                f"({human_size(downloaded)}/{human_size(total)})"
                            )
                            sys.stdout.flush()
                            last_pct = pct
            print()
        return True
    except (urllib.error.URLError, OSError) as e:
        print(f"\n  ERROR: {e}")
        if dest.exists():
            dest.unlink()
        return False


def get_project_venv_python() -> Path | None:
    """检测项目根目录下是否有 .venv，返回其 python.exe 路径（不存在返回 None）"""
    project_root = Path(__file__).resolve().parent.parent
    candidates = [
        project_root / ".venv" / "Scripts" / "python.exe",   # Windows
        project_root / ".venv" / "bin" / "python",           # Linux/macOS
    ]
    for p in candidates:
        if p.exists():
            return p
    return None


def is_package_installed(name: str) -> bool:
    """检测包是否已安装"""
    try:
        cmd = [sys.executable, "-c", f"import {name}; print({name}.__version__)"]
        result = subprocess.run(cmd, capture_output=True, text=True)
        return result.returncode == 0
    except Exception:
        return False


def pip_install(packages: list[str]) -> bool:
    """安装依赖：优先装到项目内 .venv，否则装到当前 python。
    已安装的包不会主动升级（避免把用户手动降级的版本升回去）。
    """
    venv_python = get_project_venv_python()
    if venv_python and str(venv_python).lower() != Path(sys.executable).resolve().as_posix().lower():
        # 项目内有 .venv，且当前不是它的 python → 用 .venv 的 pip 装
        cmd = [str(venv_python), "-m", "pip", "install"] + packages
        print(f"  Install to project venv: {venv_python.parent.parent}")
    else:
        # 没有项目 .venv，或已经在 .venv 里 → 装到当前 python
        cmd = [sys.executable, "-m", "pip", "install"] + packages
        print(f"  Install to current python: {sys.executable}")
    # 注意：不用 --upgrade，避免把用户手动降级的版本升回去
    print(f"  Run: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=False)
    return result.returncode == 0


# ---------------------------------------------------------------------------
# 安装任务
# ---------------------------------------------------------------------------

def install_clip(models_dir: Path, clip_src: Path) -> bool:
    """从已下载目录拷贝并重命名 CLIP 文件"""
    print("\n" + "=" * 60)
    print("[1/3] CLIP ViT-B/32 ONNX")
    print("=" * 60)
    print(f"  Source: {clip_src}")
    print(f"  Target: {models_dir}")

    if not clip_src.exists():
        print(f"  ERROR: 源目录不存在: {clip_src}")
        print(f"  请先用以下命令下载:")
        print(f"    hf download inference4j/clip-vit-base-patch32 "
              f"--local-dir {clip_src}")
        return False

    ok = True
    for src_name, dst_name in CLIP_FILE_MAP.items():
        src = clip_src / src_name
        dst = models_dir / dst_name
        if not src.exists():
            print(f"  MISSING: {src}")
            ok = False
            continue
        if dst.exists() and dst.stat().st_size == src.stat().st_size:
            print(f"  SKIP (已存在): {dst_name}")
            continue
        print(f"  Copy: {src_name} -> {dst_name} ({human_size(src.stat().st_size)})")
        shutil.copy2(src, dst)
    return ok


def install_bge(models_dir: Path) -> bool:
    """用 optimum-cli 导出 BGE-small-zh-v1.5 ONNX"""
    print("\n" + "=" * 60)
    print("[2/3] BGE-small-zh-v1.5 ONNX (自动导出)")
    print("=" * 60)

    dst_onnx = models_dir / "bge-small-zh.onnx"
    dst_tok  = models_dir / "bge_tokenizer.json"

    if dst_onnx.exists() and dst_tok.exists():
        print(f"  SKIP (已存在):")
        print(f"    {dst_onnx}")
        print(f"    {dst_tok}")
        return True

    # 1. 安装依赖（optimum + onnx + transformers + torch）
    #    已安装的包不会被升级（避免把用户手动降级的版本升回去）
    print("  Step 1/3: 检查 Python 依赖")
    deps = ["optimum", "onnx", "onnxruntime", "transformers", "torch"]
    missing = [d for d in deps if not is_package_installed(d)]
    if missing:
        print(f"  缺失依赖: {missing}")
        print(f"  正在安装（不会升级已安装的包）...")
        if not pip_install(missing):
            print("  ERROR: pip install 失败")
            return False
    else:
        print(f"  所有依赖已安装: {deps}")
        print(f"  当前版本:")
        for d in deps:
            try:
                r = subprocess.run([sys.executable, "-c", f"import {d}; print({d}.__version__)"],
                                   capture_output=True, text=True)
                print(f"    {d}: {r.stdout.strip()}")
            except Exception:
                pass

    # 2. 导出 ONNX
    #    优先尝试 torch.onnx.export（不经过 ORT 后端，规避 torch.int4 兼容问题）；
    #    若失败则回退到 optimum ORTModelForFeatureExtraction。
    import tempfile
    venv_python = get_project_venv_python()
    use_python = str(venv_python) if venv_python else sys.executable

    with tempfile.TemporaryDirectory(prefix="bge_export_") as tmp:
        tmp_path = Path(tmp)

        # --- 主路径：pure torch.onnx.export，不触碰 onnxruntime io_binding ---
        export_script = tmp_path / "_export_bge.py"
        export_script.write_text(
            'import sys, os, torch\n'
            'from pathlib import Path\n'
            'from transformers import AutoTokenizer, AutoModel\n'
            '\n'
            f'output_dir = Path(r"{tmp_path}")\n'
            f'model_id   = "{BGE_SOURCE_MODEL}"\n'
            '\n'
            'print(f"Loading {model_id} ...")\n'
            'tokenizer = AutoTokenizer.from_pretrained(model_id)\n'
            'model     = AutoModel.from_pretrained(model_id)\n'
            'model.eval()\n'
            '\n'
            'dummy = tokenizer("hello world", return_tensors="pt")\n'
            'input_ids      = dummy["input_ids"]\n'
            'attention_mask = dummy["attention_mask"]\n'
            'token_type_ids = dummy.get("token_type_ids")\n'
            '\n'
            'onnx_path = output_dir / "model.onnx"\n'
            'print(f"Exporting to {onnx_path} ...")\n'
            '\n'
            'if token_type_ids is not None:\n'
            '    args   = (input_ids, attention_mask, token_type_ids)\n'
            '    names  = ["input_ids", "attention_mask", "token_type_ids"]\n'
            'else:\n'
            '    args   = (input_ids, attention_mask)\n'
            '    names  = ["input_ids", "attention_mask"]\n'
            '\n'
            'with torch.no_grad():\n'
            '    torch.onnx.export(\n'
            '        model, args, str(onnx_path),\n'
            '        input_names  = names,\n'
            '        output_names = ["last_hidden_state", "pooler_output"],\n'
            '        dynamic_axes = {n: {0: "batch", 1: "seq"} for n in names},\n'
            '        opset_version = 14,\n'
            '    )\n'
            '\n'
            'tokenizer.save_pretrained(str(output_dir))\n'
            'print(f"Done. Files: {list(output_dir.iterdir())}")\n'
        )

        cmd = [use_python, str(export_script)]
        print(f"\n  Step 2/3: 导出 ONNX (torch.onnx.export, 兼容 torch 2.5)")
        print(f"  Run: {' '.join(cmd)}")
        result = subprocess.run(cmd, capture_output=False)

        if result.returncode != 0:
            # --- 回退路径：optimum ORTModelForFeatureExtraction ---
            print("  WARN: torch.onnx.export 失败，尝试 optimum 回退路径 ...")
            fallback_script = tmp_path / "_export_bge_optimum.py"
            fallback_script.write_text(
                'from optimum.onnxruntime import ORTModelForFeatureExtraction\n'
                'from transformers import AutoTokenizer\n'
                f'output_dir = r"{tmp_path}"\n'
                f'model_id   = "{BGE_SOURCE_MODEL}"\n'
                'print(f"Loading and exporting {model_id} via optimum ...")\n'
                'model = ORTModelForFeatureExtraction.from_pretrained(\n'
                '    model_id, export=True, provider="CPUExecutionProvider"\n'
                ')\n'
                'model.save_pretrained(output_dir)\n'
                'AutoTokenizer.from_pretrained(model_id).save_pretrained(output_dir)\n'
                'print(f"Saved to {output_dir}")\n'
            )
            cmd2 = [use_python, str(fallback_script)]
            print(f"  Run: {' '.join(cmd2)}")
            result2 = subprocess.run(cmd2, capture_output=False)
            if result2.returncode != 0:
                print("  ERROR: optimum 回退路径也失败")
                print(f"\n  手动导出命令（任选其一）：")
                print(f"  [torch] {use_python} -c \""
                      f"import torch; from transformers import AutoTokenizer, AutoModel; "
                      f"m=AutoModel.from_pretrained('{BGE_SOURCE_MODEL}').eval(); "
                      f"tok=AutoTokenizer.from_pretrained('{BGE_SOURCE_MODEL}'); "
                      f"d=tok('test',return_tensors='pt'); "
                      f"torch.onnx.export(m,(d['input_ids'],d['attention_mask']),'bge.onnx',"
                      f"opset_version=14)\"")
                return False

        # 找到导出的 .onnx（取最大的，排除可能的 external data 分片）
        onnx_candidates = sorted(tmp_path.glob("*.onnx"),
                                 key=lambda p: p.stat().st_size, reverse=True)
        if not onnx_candidates:
            print(f"  ERROR: 导出目录没有 .onnx 文件")
            print(f"  Files: {list(tmp_path.iterdir())}")
            return False

        src_onnx = onnx_candidates[0]
        print(f"\n  Step 3/3: 拷贝到目标位置")
        print(f"  Copy: {src_onnx.name} -> bge-small-zh.onnx "
              f"({human_size(src_onnx.stat().st_size)})")
        shutil.copy2(src_onnx, dst_onnx)

        # tokenizer.json
        src_tok = tmp_path / "tokenizer.json"
        if src_tok.exists():
            shutil.copy2(src_tok, dst_tok)
            print(f"  Copy: tokenizer.json -> bge_tokenizer.json "
                  f"({human_size(src_tok.stat().st_size)})")
        else:
            print(f"  WARN: 未找到 tokenizer.json（BGE WordPiece tokenizer 需要）")

    return True


def install_whisper(models_dir: Path) -> bool:
    """下载 Whisper small ggml"""
    print("\n" + "=" * 60)
    print("[3/3] Whisper small ggml")
    print("=" * 60)

    dst = models_dir / WHISPER_FILE
    if dst.exists():
        print(f"  SKIP (已存在): {dst} ({human_size(dst.stat().st_size)})")
        return True

    print(f"  Downloading {WHISPER_FILE} ...")
    return download_with_progress(WHISPER_URL, dst)


# ---------------------------------------------------------------------------
# 验证
# ---------------------------------------------------------------------------

def verify(models_dir: Path) -> bool:
    print("\n" + "=" * 60)
    print("验证模型文件")
    print("=" * 60)
    print(f"  目录: {models_dir}")
    print()

    if not models_dir.exists():
        print(f"  ERROR: 目录不存在")
        return False

    all_ok = True
    for fname, (desc, min_size) in EXPECTED_FILES.items():
        path = models_dir / fname
        if not path.exists():
            print(f"  [缺失] {fname:25s}  {desc}")
            all_ok = False
        elif path.stat().st_size < min_size:
            print(f"  [过小] {fname:25s}  "
                  f"{human_size(path.stat().st_size)} < {human_size(min_size)}")
            all_ok = False
        else:
            print(f"  [OK]   {fname:25s}  "
                  f"{human_size(path.stat().st_size):>10s}  {desc}")

    print()
    if all_ok:
        print("  [OK] 全部就位，可以编译运行 FrameMind 了")
        print()
        print("  下一步:")
        print("    1. 确认 third_party/onnxruntime/ 已就位")
        print("    2. 确认 third_party/whisper.cpp/ 已 git clone")
        print("    3. cmake -S . -B build -G \"Visual Studio 17 2022\" -A x64 ^")
        print("             -DCMAKE_PREFIX_PATH=\"D:/Qt/6.9.1/msvc2022_64\" ^")
        print("             -DFRAMEMIND_ENABLE_ONNX=ON ^")
        print("             -DFRAMEMIND_ENABLE_WHISPER=ON")
        print("    4. cmake --build build --config Debug")
    else:
        print("  [FAIL] 有缺失文件，请按上面的提示补齐")
    return all_ok


# ---------------------------------------------------------------------------
# 主入口
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="FrameMind 模型权重一键配置脚本",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--clip", action="store_true",
                        help="安装 CLIP ONNX（从 --clip-src 拷贝并重命名）")
    parser.add_argument("--clip-src", type=str, default=r"D:\Downloads\clip-onnx",
                        help="CLIP 已下载目录 (默认 D:\\Downloads\\clip-onnx)")
    parser.add_argument("--bge", action="store_true",
                        help="导出 BGE-small-zh-v1.5 ONNX (需联网 + PyTorch)")
    parser.add_argument("--whisper", action="store_true",
                        help="下载 Whisper ggml-small.bin")
    parser.add_argument("--all", action="store_true",
                        help="安装全部 (CLIP + BGE + Whisper)")
    parser.add_argument("--verify", action="store_true",
                        help="只验证已就位的文件")
    parser.add_argument("--output", type=str,
                        help="目标目录 (默认 %%LOCALAPPDATA%%\\FrameMind\\FrameMind\\models)")
    args = parser.parse_args()

    # 默认行为：无参数时只 verify
    if not any([args.clip, args.bge, args.whisper, args.all, args.verify]):
        args.verify = True

    if args.all:
        args.clip = args.bge = args.whisper = True

    models_dir = Path(args.output) if args.output else get_default_models_dir()

    print("FrameMind 模型权重配置")
    print(f"  Python:  {sys.executable}")
    print(f"  目标目录: {models_dir}")
    print(f"  计划:")
    if args.clip:    print(f"    - 安装 CLIP ONNX (from {args.clip_src})")
    if args.bge:     print(f"    - 导出 BGE-small-zh-v1.5 ONNX")
    if args.whisper: print(f"    - 下载 Whisper ggml-small.bin")
    if args.verify and not args.all:
        print(f"    - 仅验证")

    models_dir.mkdir(parents=True, exist_ok=True)

    results = []
    if args.clip:
        results.append(("CLIP", install_clip(models_dir, Path(args.clip_src))))
    if args.bge:
        results.append(("BGE",  install_bge(models_dir)))
    if args.whisper:
        results.append(("Whisper", install_whisper(models_dir)))

    if results:
        print("\n" + "=" * 60)
        print("安装结果")
        print("=" * 60)
        for name, ok in results:
            print(f"  {name:8s}: {'OK' if ok else 'FAILED'}")

    # 最后总验证
    verify(models_dir)

    return 0 if all(ok for _, ok in results) else 1


if __name__ == "__main__":
    sys.exit(main())

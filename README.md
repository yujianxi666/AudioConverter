# ♫ AudioConverter

批量音频转换工具 —— 将 MP3、FLAC、M4A 以及网易云音乐 NCM 加密文件转换为 FLAC 或 WAV。

支持中英文界面切换、深色/浅色主题，内置多线程并行处理。

## 功能特性

- **多格式输入** — 支持 MP3、FLAC、M4A、NCM（网易云加密格式）
- **NCM 解密** — 内置网易云音乐 `.ncm` 文件解密，无需额外工具
- **输出格式** — 可选择 FLAC（无损）或 WAV（PCM 16-bit）
- **批量转换** — 递归扫描源文件夹，多线程并行处理
- **中英双语** — 一键切换中文/英文界面
- **深色模式** — 支持深色/浅色主题切换
- **独立打包** — 可编译为单个 EXE，无需安装 Python 环境

注意
本工具解密的歌曲文件可能会吞掉音乐的封面等元数据，可以使用工具musictag来补全音乐元数据。

musictag作者博客：https://www.cnblogs.com/vinlxc/p/11347744.html

开源docker网页版：https://github.com/xhongc/music-tag-web

## 依赖

- Python 3.10+
- [customtkinter](https://github.com/TomSchimansky/CustomTkinter) ≥ 5.2.0 — 现代化 GUI 框架
- [miniaudio](https://github.com/mackron/miniaudio) ≥ 1.60 — 音频解码
- [soundfile](https://github.com/bastibe/python-soundfile) ≥ 0.12.0 — 音频写入
- [numpy](https://numpy.org/) ≥ 1.24.0 — 音频数据处理
- [pycryptodome](https://github.com/Legrandin/pycryptodome) ≥ 3.19.0 — NCM 解密（AES）
- [mutagen](https://github.com/quodlibet/mutagen) ≥ 1.46 — 音频元数据读写
- [PyInstaller](https://github.com/pyinstaller/pyinstaller) ≥ 6.0.0 — 打包为 EXE（仅构建时需要）

## 快速开始

### 从源码运行

```bash
# 1. 进入项目目录
cd AudioConverter_py

# 2. 安装依赖
pip install -r requirements.txt

# 3. 运行
python audio_converter.py
```

### 打包为 EXE

```bash
# 直接运行构建脚本
build_exe.bat

# 或手动执行
pyinstaller --onefile --windowed --name "AudioConverter" \
    --add-data "ncm_decryptor.py;." \
    --hidden-import miniaudio \
    --hidden-import soundfile \
    --hidden-import numpy \
    --hidden-import Crypto.Cipher.AES \
    --hidden-import mutagen \
    audio_converter.py
```

构建成功后，可执行文件位于 `dist/AudioConverter.exe`。

## 使用说明

1. 点击 **源文件夹** → **浏览…**，选择包含音频文件的目录（支持递归扫描子目录）
2. 点击 **输出文件夹** → **浏览…**，选择转换后的文件存放位置
3. 在 **输出格式** 下拉框中选择 FLAC 或 WAV
4. 点击 **开始转换**，等待进度条完成
5. 转换完成后弹出汇总对话框，显示成功/失败数量

右上角按钮可切换深色/浅色主题和中英文界面。

## 项目结构

```
AudioConverter_py/
├── audio_converter.py    # 主程序（GUI + 转换逻辑）
├── ncm_decryptor.py      # NCM 文件解密模块
├── requirements.txt      # Python 依赖
├── build_exe.bat         # PyInstaller 打包脚本
├── README.md
└── AudioConverter.exe    # 预编译的可执行文件
```

## 支持的格式

| 输入格式 | 说明 |
|-----------|------|
| MP3 | MPEG Audio Layer III |
| FLAC | Free Lossless Audio Codec |
| M4A | MPEG-4 Audio（AAC/ALAC） |
| NCM | 网易云音乐加密格式（自动解密） |

| 输出格式 | 说明 |
|-----------|------|
| FLAC | 无损压缩，保留最高音质 |
| WAV | PCM 16-bit，无压缩 |

## 技术栈

- **GUI**: customtkinter（基于 Tkinter 的现代化封装）
- **音频解码**: miniaudio（C 库的 Python 绑定）
- **音频写入**: soundfile（libsndfile 封装）
- **加密**: PyCryptodome（AES-128-ECB）
- **元数据**: mutagen
- **打包**: PyInstaller（onefile 模式）

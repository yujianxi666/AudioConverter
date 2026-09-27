# ♫ AudioConverter

批量音频转换工具 —— 将 MP3、FLAC、M4A 以及网易云音乐 NCM 加密文件转换为 FLAC 或 WAV。

**已从 Python 完全重构为原生 C++ / Win32 程序**，无 Python 运行时、无第三方 DLL，编译后为单个独立的 EXE。

支持中英文界面切换、深色/浅色主题，内置多线程并行处理。

---

## 相比原 Python 版本的改进

| 项目 | 说明 |
|------|------|
| **M4A 真正可用** | 原版虽然把 `.m4a` 列为支持格式，但底层 miniaudio 没有 AAC 解码器，实际转换必然失败。现在改用 Windows 自带的 Media Foundation 解码，M4A/AAC 可正常转换。 |
| **不产生临时文件** | NCM 直接在内存中解密，不再往输出目录写临时文件。 |
| **命令行模式** | 新增 `--cli` 无界面批量转换，便于脚本调用。 |
| **内置自检** | `--selftest` 会校验 AES/MD5 标准测试向量，并对 FLAC/WAV 编码做解码回环比对。 |
| **流式处理** | 解码与编码全程分块流式进行，内存占用与文件大小无关。 |

## 功能特性

- **多格式输入** — MP3、FLAC、M4A、NCM（wyy加密格式）
- **NCM 解密** — 内置解密实现，纯 C++（自实现 AES-128-ECB + RC4 密钥流），无需 OpenSSL
- **输出格式** — FLAC（无损，自实现编码器）或 WAV（PCM 16-bit）
- **批量转换** — 递归扫描源文件夹并按原目录结构输出，多线程并行（最多 8 线程）
- **中英双语** — 一键切换，带逐字显示动画
- **深色模式** — 深色/浅色主题切换，带淡入淡出动画，标题栏同步变色
- **高 DPI** — Per-Monitor V2，125%/150% 缩放下界面依然清晰
- **独立打包** — 静态链接，单个 EXE，无需任何运行时

## 环境要求

- Windows 10 或更高版本（需要 Media Foundation，系统自带）
- 编译器（任选其一）：
  - **MinGW-w64 GCC 11+**（推荐，使用 WinLibs 或 MSYS2 的 mingw-w64 工具链）
  - MSVC 2019+ 配合 CMake

本项目的验证环境为 **MinGW-w64 GCC 16.1.0 (UCRT, POSIX threads)**。

## 编译

### 方式一：一键脚本（推荐）

```bat
build.bat
```

`build.bat` 会调用 `build.ps1`，自动查找 `g++` 与 `windres`，增量编译并输出
`build\AudioConverter.exe`。

```powershell
# 也可以直接使用 PowerShell 脚本
powershell -ExecutionPolicy Bypass -File build.ps1          # 增量编译
powershell -ExecutionPolicy Bypass -File build.ps1 -Clean   # 完全重新编译
powershell -ExecutionPolicy Bypass -File build.ps1 -DebugBuild
```

若 `g++` 不在 `PATH` 中，脚本还会尝试以下位置，也可以把它们加入 `PATH`：

```
%LOCALAPPDATA%\AudioConverterTools\mingw64\bin
C:\mingw64\bin
C:\msys64\mingw64\bin
```

### 方式二：CMake

```bash
cmake -B out -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build out -j
```

## 使用说明

### 图形界面

直接双击 `build\AudioConverter.exe`，或带参数启动以预填目录：

```bat
AudioConverter.exe --source "D:\Music" --output "D:\Converted" -f flac
```

1. 点击 **源文件夹** → **浏览…**，选择包含音频文件的目录（支持递归扫描子目录）
2. 点击 **输出文件夹** → **浏览…**，选择转换后的文件存放位置
3. 在 **输出格式** 下拉框中选择 FLAC 或 WAV
4. 点击 **开始转换**，等待进度条完成
5. 转换完成后弹出汇总对话框，显示成功/失败数量

右上角按钮可切换深色/浅色主题和中英文界面。

### 命令行

```bat
AudioConverter.exe --cli -i <输入目录或文件> -o <输出目录> [-f flac|wav] [-j 线程数] [--lang zh|en] [-q]
```

| 参数 | 说明 |
|------|------|
| `-i, --input <路径>` | 输入文件夹（递归扫描）或单个音频文件 |
| `-o, --output <目录>` | 输出目录，自动重建子目录结构 |
| `-f, --format <格式>` | 输出格式：`flac`（默认）或 `wav` |
| `-j, --jobs <n>` | 工作线程数，默认 `min(CPU 核心数, 8)` |
| `--lang <zh\|en>` | 提示信息语言 |
| `-q, --quiet` | 只输出最终汇总 |

```bat
:: 自检：AES / MD5 标准向量 + FLAC/WAV 编解码回环
AudioConverter.exe --selftest

:: 查看版本与帮助
AudioConverter.exe --version
AudioConverter.exe --help
```

退出码：`0` 全部成功，`1` 存在失败文件，`2` 参数错误。

## 项目结构

```
AudioConverter/
├── src/
│   ├── main.cpp                       # 入口：分发 GUI / 命令行
│   ├── cli.cpp / cli.h                # 命令行模式与自检
│   ├── core/
│   │   ├── util.h/.cpp                # 字符串、路径、文件、错误分类
│   │   ├── crypto.h/.cpp              # 自实现 AES、MD5、base64
│   │   ├── ncm.h/.cpp                 # NCM 容器解密 + 格式嗅探
│   │   ├── audio.h/.cpp               # PCM 源/写入器接口、WAV 写入、编解码流程
│   │   ├── decoders.h                 # 解码后端声明
│   │   ├── decode_miniaudio.cpp       # WAV/FLAC/MP3 解码（miniaudio）
│   │   ├── decode_mediafoundation.cpp # M4A/AAC 解码（Media Foundation）
│   │   ├── flac_encoder.h/.cpp        # 自实现 FLAC 编码器
│   │   ├── metadata.cpp               # FLAC/ID3v2/MP4 标签与封面读写
│   │   ├── i18n.h/.cpp                # 中英文文案
│   │   └── converter.h/.cpp           # 批量转换引擎（线程池、进度、取消）
│   ├── gui/
│   │   ├── theme.h/.cpp               # 配色、字体、GDI+ 绘制原语
│   │   └── main_window.h/.cpp         # 主窗口（自绘控件、动画、事件）
│   └── third_party/
│       └── miniaudio.h                # v0.11.25（单头文件，公共领域）
├── res/
│   ├── app.rc                         # 图标与版本信息
│   └── app.ico
├── build.ps1 / build.bat              # 构建脚本
├── CMakeLists.txt
└── README.md
```

## 技术栈

| 环节 | 实现 |
|------|------|
| GUI | Win32 + GDI+ 全自绘（圆角控件、自绘列表/下拉框/滚动条、悬停与动画） |
| 音频解码 | miniaudio（WAV/FLAC/MP3）+ Windows Media Foundation（M4A/AAC） |
| FLAC 编码 | 自实现：固定预测器 0–4 阶、分区 Rice 编码、四种立体声去相关模式、真实 MD5 签名 |
| WAV 写出 | 自实现规范 44 字节 PCM 头，收尾回填长度 |
| NCM 解密 | 自实现 AES-128-ECB、RC4 密钥盒、base64、JSON 元数据解析 |
| 元数据 | 自实现 FLAC Vorbis Comment / ID3v2.2-2.4 / MP4 ilst 解析 |
| 并发 | `std::thread` 工作线程池，进度经 `PostMessage` 回传 UI 线程 |

## 支持的格式

| 输入格式 | 说明 |
|-----------|------|
| MP3 | MPEG Audio Layer III |
| FLAC | Free Lossless Audio Codec（同格式输出时直接字节复制，不做重编码） |
| M4A | MPEG-4 Audio（AAC），经 Media Foundation 解码 |
| NCM | 网易云音乐加密格式（自动解密；载荷与目标格式一致时直接输出原始字节） |

| 输出格式 | 说明 |
|-----------|------|
| FLAC | 无损压缩，保留最高音质，写入标签与封面 |
| WAV | PCM 16-bit，无压缩 |

## 验证情况

重构后已完成的验证（全部通过）：

- **AES / MD5** — FIPS-197 与 RFC 1321 标准测试向量
- **FLAC 编码器** — 3 秒合成信号（正弦 + 噪声 + 静音，覆盖常数/固定预测/逐样本三种子帧）编码后经 dr_flac 解码，**264,600 个采样点完全一致**
- **WAV 写出** — 同样回环比对，264,600 个采样点完全一致
- **NCM 解密** — 用 Python + PyCryptodome 独立构造 NCM 容器，本程序解密结果与原载荷**逐字节一致**；容器内元数据（`musicName`/`artist`/`album`）正确解析
- **三种独立解码器交叉验证** — 本程序的 FLAC/WAV 输出经 libsndfile（soundfile）读取，与源 MP3 的 PCM **逐采样一致**
- **M4A** — 用 Media Foundation 编码的 AAC 测试文件转换后，采样率/声道正确，主频与源信号一致（440 Hz），波形相关性 0.993
- **目录树** — 递归扫描、子目录结构保持、Unicode 文件名、隐藏目录跳过、FLAC→FLAC 字节级复制、并行转换
- **GUI** — 列表渲染、中英切换、深浅主题（含标题栏）、进度条、结果对话框，GUI 与命令行输出**逐字节一致**

## 说明

本工具解密的歌曲文件可能会吞掉音乐的封面等元数据，可以使用工具 musictag 来补全音乐元数据。

- musictag 作者博客：https://www.cnblogs.com/vinlxc/p/11347744.html

## 许可证

MIT，详见 [LICENSE](LICENSE)。

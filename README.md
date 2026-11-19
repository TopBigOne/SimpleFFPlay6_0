# SimpleFFPlay6.0

基于 FFmpeg 6.0 的独立 ffplay 播放器，使用 CMake 管理，支持 CLion 断点调试。

## 目标

将 FFmpeg 6.0 源码中的 `fftools/ffplay.c` 移植为独立可编译、可调试的项目。

## 项目来源

| 文件 | 来源路径 |
|------|---------|
| `ffplay.c` | `/Users/dev/Documents/AV/FFmpeg-6.0/fftools/ffplay.c` |
| `cmdutils.c/h` | `/Users/dev/Documents/AV/FFmpeg-6.0/fftools/cmdutils.c` |
| `opt_common.c/h` | `/Users/dev/Documents/AV/FFmpeg-6.0/fftools/opt_common.c` |
| `config.h` | 需根据本机编译环境手动生成/适配 |

参考老项目：`/Users/dev/Documents/AV/W_3/Study-FFmpeg-Principle/start_read_ffplay`（基于 FFmpeg 4.4）

## 环境依赖

| 依赖 | 版本 | 本机路径 |
|------|------|---------|
| FFmpeg | 6.0（需本机编译 macOS 版本） | 源码：`/Users/dev/Documents/AV/FFmpeg-6.0/` |
| SDL2 | 2.32.8 | `/opt/homebrew/Cellar/sdl2/2.32.8` |
| CMake | >= 4.3 | - |
| CLion | - | 用于调试 |

> **注意**：当前本机 FFmpeg 6.0 仅编译了 Android arm64-v8a 静态库，尚未编译 macOS 动态库。
> 已安装的 `/usr/local/ffmpeg/4.4` 是 FFmpeg 4.4 版本，不适用于本项目。

## 编译 FFmpeg 6.0（macOS）

```bash
cd /Users/dev/Documents/AV/FFmpeg-6.0

# 清理之前的 Android 交叉编译配置
make clean

# 配置 macOS 本机编译，安装到独立目录避免与 4.4 冲突
./configure \
  --prefix=/usr/local/ffmpeg/6.0 \
  --enable-shared \
  --disable-static \
  --enable-gpl \
  --enable-debug=3 \
  --disable-optimizations \
  --disable-stripping

make -j$(sysctl -n hw.ncpu)
make install
```

> `--enable-debug=3 --disable-optimizations --disable-stripping` 是调试关键，确保符号表完整、代码不被优化。

## 项目结构

原来 3766 行的 `ffplay.c` 已按职责拆成多个模块（目录分层参考 `rk_work/1_base_work/day8_qt_player/src`）。
**函数体与原版逐行一致**，只做了三件事：去掉跨文件调用函数的 `static`、补头文件声明、把全局变量挪到对应模块。
原版文件保留在 `origin/ffplay.c`（不参与编译），方便对照。

```
SimpleFFPlay6.0/
├── CMakeLists.txt
├── origin/ffplay.c                  # 原版 ffplay.c，仅供对照
└── src/
    ├── fftools/                     # FFmpeg 自带：cmdutils、opt_common、config.h（原样）
    ├── common/
    │   ├── ffplay_common.h          # FFmpeg/SDL 头文件、调优常量、同步/显示模式枚举
    │   └── player_options.h/.c      # 命令行选项对应的全局变量
    ├── pipeline/                    # 数据管线
    │   ├── packet_queue.h/.c        # 压缩包队列（读线程 → 解码线程）
    │   ├── frame_queue.h/.c         # 解码帧环形队列（解码线程 → 显示/声卡）
    │   └── decoder.h/.c             # 解码器封装 + 启停解码线程
    ├── sync/
    │   ├── clock.h/.c               # 播放时钟
    │   └── av_sync.h/.c             # 主时钟选择、视频等待时间、音频采样补偿
    ├── player/
    │   ├── video_state.h            # VideoState / AudioParams 总状态
    │   └── player_control.h/.c      # seek、暂停、静音、音量、逐帧、全屏、章节
    ├── demux/
    │   ├── read_thread.h/.c         # 读线程：av_read_frame 分发包、处理 seek/循环
    │   └── stream.h/.c              # stream_open/close、打开/关闭单个流、切换流
    ├── audio/
    │   ├── audio_decoder_thread.h/.c # 音频解码线程
    │   └── audio_output.h/.c        # SDL 音频设备、回调、重采样、更新音频时钟
    ├── video/
    │   ├── video_decoder_thread.h/.c # 视频解码线程
    │   ├── video_refresh.h/.c       # 显示调度：何时显示/丢弃哪一帧
    │   ├── video_render.h/.c        # 纹理上传、视频/字幕绘制、窗口
    │   ├── audio_visualizer.h/.c    # 波形 / 频谱显示
    │   └── sdl_texture_format.h     # AVPixelFormat ↔ SDL 纹理格式对照表
    ├── subtitle/subtitle_thread.h/.c # 字幕解码线程
    ├── filter/av_filters.h/.c       # 音视频滤镜图（CONFIG_AVFILTER）
    ├── ui/
    │   ├── sdl_context.h/.c         # window / renderer / audio_dev 句柄
    │   └── event_loop.h/.c          # 主事件循环、键鼠处理、退出清理
    └── app/
        ├── ffplay_options.h/.c      # 命令行选项表、帮助信息
        └── main.c                   # 入口
```

线程关系：

```
main 线程:   main → stream_open → event_loop ⟲ (video_refresh → video_display)
read_thread:  av_read_frame → audioq / videoq / subtitleq
audio_thread / video_thread / subtitle_thread:  PacketQueue → Decoder → sampq / pictq / subpq
SDL 音频线程: sdl_audio_callback → audio_decode_frame(sampq) → 声卡，更新 audclk
```

## 换电脑怎么配置

项目**不依赖 FFmpeg 源码**，只需要：

1. **FFmpeg 6.0 的安装目录**（`make install` 的 `--prefix`，里面有 `include/` 和 `lib/`）
2. **SDL2**：`brew install sdl2`（CMake 用 `find_package(SDL2)` 自动找到，不写死版本号路径）

FFmpeg 安装目录通过 `FFMPEG_PREFIX` 指定，三选一：

- CLion → Settings → Build, Execution, Deployment → CMake → **CMake options** 填 `-DFFMPEG_PREFIX=/your/ffmpeg6`
- 设置环境变量 `FFMPEG_PREFIX`
- 直接改 [CMakeLists.txt](CMakeLists.txt) 里的默认值

路径不对时，CMake 会直接报错并提示。

> FFmpeg 库用 `find_library(... NO_DEFAULT_PATH)` 只在 `FFMPEG_PREFIX/lib` 里找，得到完整路径再链接，避免误链到系统里的 FFmpeg 4.4。
>
> `cmdutils.c` 用到两个 FFmpeg 不对外安装的内部头文件 `libavutil/getenv_utf8.h`、`libavutil/libm.h`，
> 已拷贝到 `src/fftools/libavutil/`（仅把其中 `#include "mem.h"` 这类相对路径改成了 `"libavutil/mem.h"`）。

## 移植步骤

1. **编译 FFmpeg 6.0 macOS 动态库**（见上方）
2. **复制源文件**：从 `FFmpeg-6.0/fftools/` 复制 `ffplay.c`、`cmdutils.c/h`、`opt_common.c/h`
3. **生成 config.h**：从 FFmpeg 编译目录提取，或参考老项目手动适配
4. **修改 CMakeLists.txt**：配置 FFmpeg 6.0 和 SDL2 的头文件、库路径
5. **处理编译错误**：FFmpeg 6.0 相比 4.4 有 API 变化，需逐个修复
6. **CLion 调试**：打开项目，设置运行参数（如 `-i input.mp4`），断点调试

## FFmpeg 6.0 vs 4.4 注意事项

- FFmpeg 6.0 的 `fftools/` 新增了 `opt_common.c/h`，将通用选项从 `cmdutils` 中拆分
- 部分 API 有 deprecation 或签名变化，移植时需注意编译警告
- `config.h` 内容与编译选项强相关，不能直接复用 4.4 版本

## 调试参数示例

```
-x 800 -i "/path/to/video.mp4"
-an -x 400 -i "/path/to/video.mp4"    # 不解码音频
```

## 参考资料

- [ffplay.c 源码分析](https://www.cnblogs.com/juju-go/p/16489044.html)
- [AVFifoBuffer](https://ffmpeg.xianwaizhiyin.net/api-ffmpeg/avfifobuffer.html)
- [ffplay Mac CLion Debug 调试](https://cfm880.github.io/2019/04/27/audio_video/001_ffplay_clion/)
- [./configure && make && make install 详解](https://www.cnblogs.com/dsc65749924/p/5852177.html)

cmake_minimum_required(VERSION 4.0)
project(SimpleFFPlay6_0 C)

set(CMAKE_C_STANDARD 99)

# ---------------- FFmpeg 6.0 ----------------
# 只需要 FFmpeg 的安装目录（make install 的 --prefix，里面有 include/ 和 lib/），不需要 FFmpeg 源码。
# 换电脑时三选一：
#   1. CLion → Settings → Build, Execution, Deployment → CMake → CMake options 填 -DFFMPEG_PREFIX=/your/ffmpeg6
#   2. 设置环境变量 FFMPEG_PREFIX
#   3. 直接改下面的默认值
if(DEFINED ENV{FFMPEG_PREFIX})
    set(_ffmpeg_prefix_default "$ENV{FFMPEG_PREFIX}")
else()
    set(_ffmpeg_prefix_default "/Users/dev/Documents/AV/FFmpeg-6.0/install_macos")
endif()
set(FFMPEG_PREFIX "${_ffmpeg_prefix_default}" CACHE PATH "FFmpeg 6.0 安装目录（含 include/ 和 lib/）")

if(NOT EXISTS "${FFMPEG_PREFIX}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "FFMPEG_PREFIX=${FFMPEG_PREFIX} 下没找到 FFmpeg 头文件，请用 -DFFMPEG_PREFIX=... 指向 FFmpeg 6.0 的安装目录")
endif()

# 逐个在 FFMPEG_PREFIX/lib 里找库，得到完整路径；NO_DEFAULT_PATH 避免误链到系统里别的 FFmpeg 版本（如 4.4）
set(FFMPEG_LIBRARIES)
foreach(_lib swscale swresample avcodec avutil avdevice avfilter avformat postproc)
    find_library(FFMPEG_${_lib}_LIBRARY NAMES ${_lib} PATHS "${FFMPEG_PREFIX}/lib" NO_DEFAULT_PATH)
    if(NOT FFMPEG_${_lib}_LIBRARY)
        message(FATAL_ERROR "在 ${FFMPEG_PREFIX}/lib 里没找到 lib${_lib}")
    endif()
    list(APPEND FFMPEG_LIBRARIES ${FFMPEG_${_lib}_LIBRARY})
endforeach()

# ---------------- SDL2 ----------------
# 用 SDL2 自带的 CMake 配置查找（brew install sdl2 会装好），不写死 Cellar 里的版本号路径
find_package(SDL2 REQUIRED CONFIG HINTS /opt/homebrew /usr/local)

# src：模块之间统一写 #include "pipeline/packet_queue.h" 这种带目录的路径
# src/fftools：FFmpeg fftools 自带的 cmdutils / opt_common / config.h，
#              以及 FFmpeg 不对外安装的两个内部头文件 libavutil/getenv_utf8.h、libavutil/libm.h
include_directories(
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/fftools
        ${FFMPEG_PREFIX}/include
)

set(SOURCE_FILES
        # FFmpeg fftools 公共代码（命令行解析、banner、-version 等）
        src/fftools/config.h
        src/fftools/config_components.h
        src/fftools/cmdutils.h
        src/fftools/cmdutils.c
        src/fftools/opt_common.h
        src/fftools/opt_common.c
        src/fftools/libavutil/getenv_utf8.h
        src/fftools/libavutil/libm.h

        # 公共：头文件、常量、全局选项
        src/common/ffplay_common.h
        src/common/player_options.h
        src/common/player_options.c

        # 管线：包队列 → 解码器 → 帧队列
        src/pipeline/packet_queue.h
        src/pipeline/packet_queue.c
        src/pipeline/frame_queue.h
        src/pipeline/frame_queue.c
        src/pipeline/decoder.h
        src/pipeline/decoder.c

        # 时钟与音画同步
        src/sync/clock.h
        src/sync/clock.c
        src/sync/av_sync.h
        src/sync/av_sync.c

        # 播放器总状态与控制
        src/player/video_state.h
        src/player/player_control.h
        src/player/player_control.c

        # 解复用：读线程、流的打开/关闭
        src/demux/read_thread.h
        src/demux/read_thread.c
        src/demux/stream.h
        src/demux/stream.c

        # 音频：解码线程、SDL 输出
        src/audio/audio_decoder_thread.h
        src/audio/audio_decoder_thread.c
        src/audio/audio_output.h
        src/audio/audio_output.c

        # 视频：解码线程、刷新调度、渲染、音频可视化
        src/video/sdl_texture_format.h
        src/video/video_decoder_thread.h
        src/video/video_decoder_thread.c
        src/video/video_refresh.h
        src/video/video_refresh.c
        src/video/video_render.h
        src/video/video_render.c
        src/video/audio_visualizer.h
        src/video/audio_visualizer.c

        # 字幕
        src/subtitle/subtitle_thread.h
        src/subtitle/subtitle_thread.c

        # 滤镜
        src/filter/av_filters.h
        src/filter/av_filters.c

        # 界面：SDL 句柄、事件循环
        src/ui/sdl_context.h
        src/ui/sdl_context.c
        src/ui/event_loop.h
        src/ui/event_loop.c

        # 入口：命令行选项、main
        src/app/ffplay_options.h
        src/app/ffplay_options.c
        src/app/main.c
)

add_executable(SimpleFFPlay6_0 ${SOURCE_FILES})

# FFmpeg 用完整路径链接，避免与系统 FFmpeg 4.4 冲突；SDL2::SDL2 自带 include/SDL2 头文件目录
target_link_libraries(SimpleFFPlay6_0
        ${FFMPEG_LIBRARIES}
        SDL2::SDL2
)

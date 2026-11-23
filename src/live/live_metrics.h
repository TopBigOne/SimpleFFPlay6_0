#pragma once

/**
 * @file
 * @brief 运行指标（-metrics_file）：定期把播放健康度写成 JSON 行，对应 WebRTC 的 getStats
 *
 * 每 -metrics_interval 秒写一行 {"type":"interval",...}，退出时写一行 {"type":"summary",...}。
 * 计数器由不同线程累加（读线程：字节数、重连；视频解码线程：冻结时长；主线程：显示帧、延迟），
 * 只用于统计，不加锁（对齐的 64 位读写在 arm64 / x86-64 上不会撕裂，最多差一两次计数）。
 * 字段说明见 docs/live_reliability.md。
 */

#include "common/ffplay_common.h"

/** 延迟直方图：5ms 一格，0~2000ms，最后一格放 >= 2000ms */
#define LIVE_METRICS_LAG_BIN_MS 5
#define LIVE_METRICS_LAG_BINS   401

typedef struct LiveMetrics {
    FILE   *f;                  ///< NULL = 没开 -metrics_file
    int64_t interval_us;
    int64_t start_us;
    int64_t last_emit_us;

    int64_t frames_shown;       ///< 主线程：显示的帧数
    int64_t bytes_read;         ///< 读线程：读到的包字节数
    int64_t frozen_us;          ///< 视频解码线程：损坏冻结累计时长
    int     reconnects;         ///< 读线程：断线后重连成功次数
    int64_t disconnected_us;    ///< 读线程：断线到重连成功的累计时长

    int64_t last_frames;        ///< 上次输出时的 frames_shown，用于算区间帧率
    int64_t last_bytes;         ///< 上次输出时的 bytes_read，用于算区间码率
    unsigned lag_interval[LIVE_METRICS_LAG_BINS];  ///< 本区间的延迟直方图
    unsigned lag_total[LIVE_METRICS_LAG_BINS];     ///< 全程的延迟直方图
} LiveMetrics;

struct VideoState;

/**
 * @brief 打开指标文件（path 为 NULL 时什么都不做，之后的调用都是空操作）
 * @return 0 = 成功或未启用；<0 = 打不开文件
 */
int live_metrics_open(LiveMetrics *m, const char *path, int interval_sec);

/** @brief 主线程：显示了一帧，lag 为它比"最顺利情况"晚的秒数（NAN = 不统计延迟） */
void live_metrics_on_frame_shown(LiveMetrics *m, double lag);

/** @brief 主线程：到时间就写一行区间指标（每次 video_refresh 调用，自己判断间隔） */
void live_metrics_tick(struct VideoState *is);

/** @brief 主线程：写汇总并关闭文件 */
void live_metrics_close(struct VideoState *is);

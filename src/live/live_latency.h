#pragma once

/**
 * @file
 * @brief 低延迟直播（-lowlatency）的延迟估计：借鉴 WebRTC 的到达时间模型
 *
 * 思路（对应 WebRTC NetEq packet_arrival_history + 视频 VCMTiming）：
 *  - 读线程每收到一个参考流的包，记录 o = 到达时刻 − pts。
 *    取最近约 2 秒内 o 的最小值作为基准 base：它代表"最顺利的包"从采集到到达的固定延迟，
 *    不需要知道摄像头的墙钟，摄像头时钟和本机时钟的慢速漂移也会被窗口跟上。
 *  - 解码线程每解出一帧，记录 e = 解码完成时刻 − pts − base，即这帧比"最顺利情况"晚了多少
 *    （网络抖动 + 排队 + 解码耗时）。对 e 做指数平均得到均值和方差，
 *    目标延迟 target = 均值 + 2.33σ + 5ms（2.33σ ≈ 99% 分位，同 WebRTC 视频 jitter 估计），
 *    变大立刻生效，变小每秒最多降 100ms（同 WebRTC kDelayMaxChangeMsPerS），夹在 [min, max]。
 *  - 每帧的显示时刻 render_time = pts + base + target，由 video_refresh 按它显示或丢帧。
 *
 * 线程：base 由读线程写，jitter/target 由视频解码线程写，render_time 由主线程读，全部加锁。
 */

#include "common/ffplay_common.h"

typedef struct LiveLatency {
    SDL_mutex *mutex;
    int    has_base;        ///< 是否已收到第一个包
    double base;            ///< 2 秒窗口内 (到达时刻 − pts) 的最小值，秒
    double win_min_cur;     ///< 当前 1 秒窗口的最小值
    double win_min_prev;    ///< 上一个 1 秒窗口的最小值
    double win_start;       ///< 当前窗口开始时刻

    int    nb_frames;       ///< 已统计的帧数
    double jitter_mean;     ///< e 的指数平均，秒
    double jitter_var;      ///< e 的指数方差
    double target;          ///< 当前目标延迟（相对 base），秒
    double last_frame_time; ///< 上一帧统计的时刻，用于限制 target 下降速度

    double min_target;      ///< 目标延迟下限，秒
    double max_target;      ///< 目标延迟上限，秒
    int    flushes;         ///< 积压过大触发"跳到关键帧"的次数
} LiveLatency;

/**
 * @brief 初始化
 * @param[in] min_ms  目标延迟下限（毫秒）
 * @param[in] max_ms  目标延迟上限（毫秒）
 * @return 0 = 成功；AVERROR(ENOMEM) = 创建锁失败
 */
int live_latency_init(LiveLatency *l, int min_ms, int max_ms);

/** @brief 释放锁 */
void live_latency_uninit(LiveLatency *l);

/** @brief 清空估计（断线重连后时间戳可能不连续，必须重新建立基准） */
void live_latency_reset(LiveLatency *l);

/**
 * @brief 读线程：参考流（有视频用视频，否则用音频）每收到一个包调用一次，更新 base
 * @param[in] pts  包的 pts（秒）；NAN 时忽略
 * @param[in] now  到达时刻（秒，av_gettime_relative 基准）
 * @note 发现 o 与 base 相差超过 AV_NOSYNC_THRESHOLD（时间戳跳变、换源）时整体重置
 */
void live_latency_on_packet(LiveLatency *l, double pts, double now);

/**
 * @brief 视频解码线程：每解出一帧调用一次，更新抖动估计和目标延迟
 * @param[in] pts  帧的 pts（秒）；NAN 时忽略
 * @param[in] now  解码完成时刻（秒）
 */
void live_latency_on_frame(LiveLatency *l, double pts, double now);

/**
 * @brief 主线程：这一帧应该在什么时刻显示
 * @return render_time = pts + base + target（秒）；还没有 base 或 pts 为 NAN 时返回 NAN
 */
double live_latency_render_time(LiveLatency *l, double pts);

/**
 * @brief 当前显示的帧比"最顺利情况"晚了多少（秒），用于 -stats 打印和判断积压
 * @param[in] pts  正在显示 / 将要入队的帧的 pts（秒）
 * @param[in] now  当前时刻（秒）
 * @return now − pts − base；没有 base 时返回 NAN
 */
double live_latency_lag(LiveLatency *l, double pts, double now);

/** @brief 读取当前目标延迟（秒） */
double live_latency_target(LiveLatency *l);

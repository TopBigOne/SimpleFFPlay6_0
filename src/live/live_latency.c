#include "live/live_latency.h"

/** base 的滑动窗口分两段，每段 1 秒，base = 两段最小值中的较小者（约 2 秒窗口，同 WebRTC kPacketHistorySizeMs） */
#define LIVE_BASE_HALF_WINDOW     1.0
/** 抖动均值 / 方差的指数平均系数：每帧 1/16，25fps 下时间常数约 0.6 秒 */
#define LIVE_JITTER_ALPHA         (1.0 / 16)
/** 目标延迟 = 均值 + 2.33σ（约 99% 分位） */
#define LIVE_JITTER_SIGMA         2.33
/** 渲染余量，同 WebRTC 视频 render_delay 的量级 */
#define LIVE_RENDER_MARGIN        0.005
/** 目标延迟每秒最多下降 100ms，同 WebRTC kDelayMaxChangeMsPerS */
#define LIVE_TARGET_DECAY_PER_SEC 0.1

static void reset_locked(LiveLatency *l)
{
    l->has_base        = 0;
    l->nb_frames       = 0;
    l->jitter_mean     = 0;
    l->jitter_var      = 0;
    l->target          = l->min_target;
    l->last_frame_time = NAN;
}

int live_latency_init(LiveLatency *l, int min_ms, int max_ms)
{
    memset(l, 0, sizeof(*l));
    l->mutex = SDL_CreateMutex();
    if (!l->mutex)
        return AVERROR(ENOMEM);
    l->min_target = FFMAX(min_ms, 0) / 1000.0;
    l->max_target = FFMAX(max_ms, min_ms) / 1000.0;
    reset_locked(l);
    return 0;
}

void live_latency_uninit(LiveLatency *l)
{
    if (l->mutex)
        SDL_DestroyMutex(l->mutex);
    l->mutex = NULL;
}

void live_latency_reset(LiveLatency *l)
{
    SDL_LockMutex(l->mutex);
    reset_locked(l);
    SDL_UnlockMutex(l->mutex);
}

void live_latency_on_packet(LiveLatency *l, double pts, double now)
{
    double o;

    if (isnan(pts))
        return;
    o = now - pts;

    SDL_LockMutex(l->mutex);
    if (l->has_base && fabs(o - l->base) > AV_NOSYNC_THRESHOLD) {
        av_log(NULL, AV_LOG_INFO, "live: timestamp jump of %.3fs, resetting latency estimate\n", o - l->base);
        reset_locked(l);
    }
    if (!l->has_base) {
        l->has_base     = 1;
        l->base         = o;
        l->win_min_cur  = o;
        l->win_min_prev = o;
        l->win_start    = now;
    } else {
        if (now - l->win_start >= LIVE_BASE_HALF_WINDOW) {
            // 窗口滚动：旧窗口的最小值过期，base 才能随时钟漂移向上调整
            l->win_min_prev = l->win_min_cur;
            l->win_min_cur  = o;
            l->win_start    = now;
        } else if (o < l->win_min_cur) {
            l->win_min_cur = o;
        }
        l->base = FFMIN(l->win_min_cur, l->win_min_prev);
    }
    SDL_UnlockMutex(l->mutex);
}

void live_latency_on_frame(LiveLatency *l, double pts, double now)
{
    double e, diff, wanted;

    if (isnan(pts))
        return;

    SDL_LockMutex(l->mutex);
    if (!l->has_base) {
        SDL_UnlockMutex(l->mutex);
        return;
    }
    e = FFMAX(now - pts - l->base, 0);

    if (l->nb_frames++ == 0) {
        l->jitter_mean = e;
        l->jitter_var  = 0;
    } else {
        diff = e - l->jitter_mean;
        l->jitter_mean += LIVE_JITTER_ALPHA * diff;
        l->jitter_var   = (1 - LIVE_JITTER_ALPHA) * (l->jitter_var + LIVE_JITTER_ALPHA * diff * diff);
    }

    wanted = l->jitter_mean + LIVE_JITTER_SIGMA * sqrt(l->jitter_var) + LIVE_RENDER_MARGIN;
    if (wanted >= l->target) {
        l->target = wanted;                                   // 变大：立刻生效，避免继续丢帧
    } else if (!isnan(l->last_frame_time)) {
        double max_drop = LIVE_TARGET_DECAY_PER_SEC * FFMAX(now - l->last_frame_time, 0);
        l->target = FFMAX(wanted, l->target - max_drop);      // 变小：限速下降，避免画面突然快进
    }
    l->target = av_clipd(l->target, l->min_target, l->max_target);
    l->last_frame_time = now;
    SDL_UnlockMutex(l->mutex);
}

double live_latency_render_time(LiveLatency *l, double pts)
{
    double t = NAN;

    if (isnan(pts))
        return NAN;
    SDL_LockMutex(l->mutex);
    if (l->has_base)
        t = pts + l->base + l->target;
    SDL_UnlockMutex(l->mutex);
    return t;
}

double live_latency_lag(LiveLatency *l, double pts, double now)
{
    double lag = NAN;

    if (isnan(pts))
        return NAN;
    SDL_LockMutex(l->mutex);
    if (l->has_base)
        lag = now - pts - l->base;
    SDL_UnlockMutex(l->mutex);
    return lag;
}

double live_latency_target(LiveLatency *l)
{
    double t;

    SDL_LockMutex(l->mutex);
    t = l->target;
    SDL_UnlockMutex(l->mutex);
    return t;
}

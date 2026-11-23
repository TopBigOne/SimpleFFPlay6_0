#include "live/live_metrics.h"
#include "player/video_state.h"

int live_metrics_open(LiveMetrics *m, const char *path, int interval_sec)
{
    memset(m, 0, sizeof(*m));
    if (!path)
        return 0;
    m->f = fopen(path, "w");
    if (!m->f) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open metrics file '%s'\n", path);
        return AVERROR(errno);
    }
    m->interval_us  = (int64_t)FFMAX(interval_sec, 1) * 1000000;
    m->start_us     = av_gettime_relative();
    m->last_emit_us = m->start_us;
    return 0;
}

void live_metrics_on_frame_shown(LiveMetrics *m, double lag)
{
    int bin;

    if (!m->f)
        return;
    m->frames_shown++;
    if (isnan(lag))
        return;
    bin = av_clip((int)(lag * 1000 / LIVE_METRICS_LAG_BIN_MS), 0, LIVE_METRICS_LAG_BINS - 1);
    m->lag_interval[bin]++;
    m->lag_total[bin]++;
}

/** 直方图分位数（毫秒，取格子上沿）；没有样本时返回 -1 */
static int hist_percentile(const unsigned *h, double q)
{
    uint64_t n = 0, acc = 0, want;
    int i;

    for (i = 0; i < LIVE_METRICS_LAG_BINS; i++)
        n += h[i];
    if (!n)
        return -1;
    want = (uint64_t)ceil(q * n);
    for (i = 0; i < LIVE_METRICS_LAG_BINS; i++) {
        acc += h[i];
        if (acc >= want)
            return (i + 1) * LIVE_METRICS_LAG_BIN_MS;
    }
    return LIVE_METRICS_LAG_BINS * LIVE_METRICS_LAG_BIN_MS;
}

/** 两种记录共用的计数器部分 */
static void write_counters(FILE *f, VideoState *is)
{
    LiveMetrics *m = &is->metrics;

    fprintf(f, "\"frames_shown\":%"PRId64",\"frames_dropped\":%d,"
               "\"freezes\":%d,\"frozen_ms\":%"PRId64",\"keyframe_jumps\":%d,"
               "\"reconnects\":%d,\"disconnected_ms\":%"PRId64",\"watchdog\":%d,"
               "\"target_ms\":%.0f,"
               "\"audio\":{\"underruns\":%d,\"waits\":%d,\"drops\":%d,\"lag_accept_ms\":%.0f}",
            m->frames_shown, is->frame_drops_early + is->frame_drops_late,
            is->vid_freezes, m->frozen_us / 1000, is->live.flushes,
            m->reconnects, m->disconnected_us / 1000, is->wd_triggers,
            live_latency_target(&is->live) * 1000,
            is->audio_underruns, is->audio_waits, is->audio_drops, is->audio_lag_accept * 1000);
}

void live_metrics_tick(VideoState *is)
{
    LiveMetrics *m = &is->metrics;
    int64_t now = av_gettime_relative();
    double secs;

    if (!m->f || now - m->last_emit_us < m->interval_us)
        return;
    secs = (now - m->last_emit_us) / 1000000.0;
    fprintf(m->f, "{\"type\":\"interval\",\"t\":%.1f,\"fps\":%.1f,\"kbps\":%.0f,"
                  "\"lag_ms\":{\"p50\":%d,\"p95\":%d,\"p99\":%d},",
            (now - m->start_us) / 1000000.0,
            (m->frames_shown - m->last_frames) / secs,
            (m->bytes_read - m->last_bytes) * 8 / 1000.0 / secs,
            hist_percentile(m->lag_interval, 0.50), hist_percentile(m->lag_interval, 0.95),
            hist_percentile(m->lag_interval, 0.99));
    write_counters(m->f, is);
    fputs("}\n", m->f);
    fflush(m->f);
    memset(m->lag_interval, 0, sizeof(m->lag_interval));
    m->last_frames  = m->frames_shown;
    m->last_bytes   = m->bytes_read;
    m->last_emit_us = now;
}

void live_metrics_close(VideoState *is)
{
    LiveMetrics *m = &is->metrics;
    double secs;

    if (!m->f)
        return;
    secs = FFMAX((av_gettime_relative() - m->start_us) / 1000000.0, 0.001);
    fprintf(m->f, "{\"type\":\"summary\",\"t\":%.1f,\"fps\":%.1f,\"kbps\":%.0f,"
                  "\"lag_ms\":{\"p50\":%d,\"p95\":%d,\"p99\":%d},",
            secs, m->frames_shown / secs, m->bytes_read * 8 / 1000.0 / secs,
            hist_percentile(m->lag_total, 0.50), hist_percentile(m->lag_total, 0.95),
            hist_percentile(m->lag_total, 0.99));
    write_counters(m->f, is);
    fputs("}\n", m->f);
    fclose(m->f);
    m->f = NULL;
}

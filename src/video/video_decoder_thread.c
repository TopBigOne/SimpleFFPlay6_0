/*
 * 本文件拆自 FFmpeg 6.0 fftools/ffplay.c（Copyright (c) 2003 Fabrice Bellard），
 * 遵循 GNU LGPL 2.1 或更高版本。函数体保持原样，仅调整 static 和 #include。
 */
#include "video/video_decoder_thread.h"
#include "common/player_options.h"
#include "filter/av_filters.h"
#include "sync/av_sync.h"
#include "video/video_render.h"

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    Frame *vp;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    // 低延迟模式：在可能阻塞的 peek_writable 之前记录"解码完成时刻"，更新抖动估计和目标延迟
    if (low_latency)
        live_latency_on_frame(&is->live, pts, av_gettime_relative() / 1000000.0);

    if (!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    set_default_window_size(vp->width, vp->height, vp->sar);

    av_frame_move_ref(vp->frame, src_frame);
    frame_queue_push(&is->pictq);
    return 0;
}

/** 冻结等关键帧最多等这么久（微秒）：有的摄像头用逐行刷新（intra refresh）不发关键帧，不能永远冻住 */
#define CORRUPT_FREEZE_MAX_US 10000000

static int freeze_enabled(void)
{
    return freeze_on_corrupt > 0 || (freeze_on_corrupt < 0 && low_latency);
}

/**
 * @brief 丢包不花屏（借鉴 WebRTC：参考链不完整的帧永远不解码 / 不显示）
 *
 * 一帧被标记损坏（传输层丢包、解码出错、解码器做了错误掩盖）之后，后面的 P 帧都参考了坏数据，
 * 即使它们自己解码"成功"也可能花屏。所以从这一帧起全部丢弃，画面停在最后一个好帧，
 * 直到一个干净的关键帧（IDR 或带恢复点 SEI）到来。包 serial 变化（seek、跳关键帧、重连）后同样先等关键帧。
 *
 * @return 1 = 这帧可以显示；0 = 丢弃
 */
static int frame_integrity_ok(VideoState *is, AVFrame *frame)
{
    int64_t now = av_gettime_relative();
    int bad = frame->decode_error_flags || (frame->flags & AV_FRAME_FLAG_CORRUPT);

    // serial 变化（seek / 跳关键帧 / 重连）：解码器已 flush，正常情况，静默等关键帧；正在进行的损坏冻结继续算同一次
    if (is->viddec.pkt_serial != is->vid_wait_serial) {
        is->vid_wait_serial = is->viddec.pkt_serial;
        if (!is->vid_wait_key) {
            is->vid_wait_key   = 1;
            is->vid_wait_since = now;
        }
    }

    if (bad) {
        // 只有正常出画面时出现损坏才算一次"冻结"；起播 / 跳转后本来就在等首个关键帧，那时的坏帧静默丢弃
        if (!is->vid_wait_key && !is->vid_wait_bad) {
            is->vid_wait_bad   = 1;
            is->vid_wait_since = now;
            is->vid_freezes++;
            av_log(NULL, AV_LOG_WARNING, "live: corrupted video frame (pts %"PRId64", errors 0x%x), holding last good frame until next keyframe\n",
                   frame->pts, frame->decode_error_flags);
        }
        is->vid_wait_key = 1;
        is->vid_wait_dropped++;
        return 0;
    }

    if (!is->vid_wait_key)
        return 1;

    if (frame->key_frame || now - is->vid_wait_since > CORRUPT_FREEZE_MAX_US) {
        if (!frame->key_frame)
            av_log(NULL, AV_LOG_WARNING, "live: no keyframe within %ds, resuming display anyway\n",
                   CORRUPT_FREEZE_MAX_US / 1000000);
        if (is->vid_wait_bad)
            av_log(NULL, AV_LOG_WARNING, "live: recovered after %.0fms frozen, dropped %d frames\n",
                   (now - is->vid_wait_since) / 1000.0, is->vid_wait_dropped);
        is->vid_wait_key     = 0;
        is->vid_wait_bad     = 0;
        is->vid_wait_dropped = 0;
        return 1;
    }
    is->vid_wait_dropped++;
    return 0;
}

static int get_video_frame(VideoState *is, AVFrame *frame)
{
    int got_picture;

    if ((got_picture = decoder_decode_frame(&is->viddec, frame, NULL)) < 0)
        return -1;

    if (got_picture && freeze_enabled() && !frame_integrity_ok(is, frame)) {
        av_frame_unref(frame);
        return 0;
    }

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        if (framedrop>0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - get_master_clock(is);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 &&
                    is->viddec.pkt_serial == is->vidclk.serial &&
                    is->videoq.nb_packets) {
                    is->frame_drops_early++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

/**
 * @brief 仅供测试：模拟解码器卡死，用来验证看门狗
 *
 * 设置环境变量 SIMPLEFFPLAY_TEST_STALL_DECODER_AT=<秒> 后，进程启动这么多秒时视频解码线程停住，
 * 直到它的包队列被 abort（即解码器被关闭重建）才退出——和真实的解码器卡死一样，清空队列救不回来。
 * 只触发一次，重建后的解码线程正常工作。
 */
static void test_maybe_stall_decoder(VideoState *is)
{
    static int64_t stall_at = -1;
    static int stalled;

    if (stall_at < 0) {
        const char *env = getenv("SIMPLEFFPLAY_TEST_STALL_DECODER_AT");
        stall_at = env ? av_gettime_relative() + (int64_t)(atof(env) * 1000000) : 0;
    }
    if (!stall_at || stalled || av_gettime_relative() < stall_at)
        return;
    stalled = 1;
    av_log(NULL, AV_LOG_WARNING, "test: simulating a stuck video decoder\n");
    while (!is->videoq.abort_request)
        av_usleep(10000);
}

int video_thread(void *arg)
{
    VideoState *is = arg;
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

#if CONFIG_AVFILTER
    AVFilterGraph *graph = NULL;
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = -2;
    int last_serial = -1;
    int last_vfilter_idx = 0;
#endif

    if (!frame)
        return AVERROR(ENOMEM);

    for (;;) {
        ret = get_video_frame(is, frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;
        test_maybe_stall_decoder(is);

#if CONFIG_AVFILTER
        if (   last_w != frame->width
            || last_h != frame->height
            || last_format != frame->format
            || last_serial != is->viddec.pkt_serial
            || last_vfilter_idx != is->vfilter_idx) {
            av_log(NULL, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(frame->format), "none"), is->viddec.pkt_serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if (!graph) {
                ret = AVERROR(ENOMEM);
                goto the_end;
            }
            graph->nb_threads = filter_nbthreads;
            if ((ret = configure_video_filters(graph, is, vfilters_list ? vfilters_list[is->vfilter_idx] : NULL, frame)) < 0) {
                SDL_Event event;
                event.type = FF_QUIT_EVENT;
                event.user.data1 = is;
                SDL_PushEvent(&event);
                goto the_end;
            }
            filt_in  = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = frame->format;
            last_serial = is->viddec.pkt_serial;
            last_vfilter_idx = is->vfilter_idx;
            frame_rate = av_buffersink_get_frame_rate(filt_out);
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {
            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    is->viddec.finished = is->viddec.pkt_serial;
                ret = 0;
                break;
            }

            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;
            tb = av_buffersink_get_time_base(filt_out);
#endif
            duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = queue_picture(is, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);
            av_frame_unref(frame);
#if CONFIG_AVFILTER
            if (is->videoq.serial != is->viddec.pkt_serial)
                break;
        }
#endif

        if (ret < 0)
            goto the_end;
    }
 the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&graph);
#endif
    av_frame_free(&frame);
    return 0;
}

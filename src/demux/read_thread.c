/*
 * 本文件拆自 FFmpeg 6.0 fftools/ffplay.c（Copyright (c) 2003 Fabrice Bellard），
 * 遵循 GNU LGPL 2.1 或更高版本。函数体保持原样，仅调整 static 和 #include。
 */
#include "demux/read_thread.h"
#include "common/player_options.h"
#include "demux/stream.h"
#include "player/player_control.h"
#include "sync/av_sync.h"
#include "video/video_render.h"

/** 直播重连：读包时这么久没有任何数据就当作断线（同 ExoPlayer 的卡死检测 4 秒） */
#define LIVE_READ_TIMEOUT_US  4000000
/** 直播重连：打开连接 + 探测流信息的超时 */
#define LIVE_OPEN_TIMEOUT_US  10000000
/** 直播重连：第 n 次失败后等待 min((n−1) × 1s, 5s) 再重试（同 ExoPlayer DefaultLoadErrorHandlingPolicy） */
#define LIVE_RETRY_STEP_MS    1000
#define LIVE_RETRY_MAX_MS     5000

static int decode_interrupt_cb(void *ctx)
{
    VideoState *is = ctx;
    // 除了退出请求，直播重连模式下超过 io_deadline 也中断阻塞中的网络读写，避免死连接永远卡住
    return is->abort_request ||
           (is->io_deadline && av_gettime_relative() > is->io_deadline);
}

static int reconnect_enabled(void)
{
    return live_reconnect > 0 || (live_reconnect < 0 && low_latency);
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    return stream_id < 0 ||
           queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

/**
 * @brief 低延迟模式：每个包入队前调用
 *  1. 参考流（有视频用视频，否则用音频）的包更新延迟基准 base（见 live/live_latency.h）
 *  2. 视频关键帧到达时，如果它比当前显示的画面超前了 2 × latency_max 以上，说明积压过多
 *     （解码跟不上或卡顿后数据涌入）：清空三个包队列，从这个关键帧重新开始。
 *     packet_queue_flush 会让 serial + 1，解码器、帧队列里的旧数据自动作废（ffplay 原有机制）。
 */
static void live_on_packet(VideoState *is, AVPacket *pkt)
{
    int ref_stream = is->video_stream >= 0 ? is->video_stream : is->audio_stream;
    AVStream *st;
    int64_t ts;
    double pts, now, shown_pts;

    if (pkt->stream_index != ref_stream)
        return;
    st  = is->ic->streams[pkt->stream_index];
    ts  = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
    pts = ts == AV_NOPTS_VALUE ? NAN : ts * av_q2d(st->time_base);
    now = av_gettime_relative() / 1000000.0;

    live_latency_on_packet(&is->live, pts, now);

    if (pkt->stream_index != is->video_stream || !(pkt->flags & AV_PKT_FLAG_KEY) || isnan(pts))
        return;
    // 画面因损坏冻结、正在等关键帧时，"落后"是冻结造成的而不是解码积压，不触发跳转
    // （否则会把音频队列也一起清掉；关键帧解出来后按截止时间显示，自然回到当前）
    if (is->vid_wait_bad)
        return;
    // 当前显示画面的 pts；serial 不一致说明刚 flush 过，还没有新画面显示，不判断
    if (is->vidclk.serial != is->videoq.serial)
        return;
    shown_pts = is->vidclk.pts;
    if (isnan(shown_pts) || pts - shown_pts <= 2.0 * latency_max_ms / 1000.0)
        return;

    is->live.flushes++;
    av_log(NULL, AV_LOG_WARNING, "live: backlog %.0fms > %dms, jumping to keyframe at %.3f\n",
           (pts - shown_pts) * 1000, 2 * latency_max_ms, pts);
    if (is->audio_stream >= 0)
        packet_queue_flush(&is->audioq);
    if (is->subtitle_stream >= 0)
        packet_queue_flush(&is->subtitleq);
    packet_queue_flush(&is->videoq);
}

static int is_realtime(AVFormatContext *s)
{
    if(   !strcmp(s->iformat->name, "rtp")
       || !strcmp(s->iformat->name, "rtsp")
       || !strcmp(s->iformat->name, "sdp")
    )
        return 1;

    if(s->pb && (   !strncmp(s->url, "rtp:", 4)
                 || !strncmp(s->url, "udp:", 4)
                )
    )
        return 1;
    return 0;
}

/**
 * @brief 重连打开时遇到的错误是否重试也没用（同 ExoPlayer：解析错误、文件不存在、配置错误直接放弃）
 * @note 超时、连接被拒、HTTP 4xx/5xx 等网络错误都重试：摄像头重启期间常见
 */
static int is_fatal_open_error(int err)
{
    return err == AVERROR_INVALIDDATA ||
           err == AVERROR(ENOENT) ||
           err == AVERROR_PROTOCOL_NOT_FOUND ||
           err == AVERROR_DEMUXER_NOT_FOUND ||
           err == AVERROR_OPTION_NOT_FOUND;
}

/**
 * @brief 等待 ms 毫秒，期间响应退出请求
 * @return 1 = 收到退出请求；0 = 正常等完
 */
static int wait_or_abort(VideoState *is, SDL_mutex *wait_mutex, int ms)
{
    int64_t end = av_gettime_relative() + (int64_t)ms * 1000;

    while (!is->abort_request && av_gettime_relative() < end) {
        SDL_LockMutex(wait_mutex);
        SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
        SDL_UnlockMutex(wait_mutex);
    }
    return is->abort_request;
}

/**
 * @brief 建立一个新连接：打开输入 + 探测流信息（首次连接重试和断线重连共用）
 *
 * 用 opts_template 的副本打开（avformat_open_input 会改写传入的字典），整个过程受 LIVE_OPEN_TIMEOUT_US 限制。
 *
 * @param[out] out            成功时得到新的 AVFormatContext
 * @param[in]  opts_template  第一次打开前保存的格式选项（如 rtsp_transport）
 * @return 0 = 成功；<0 = 错误码（AVERROR_OPTION_NOT_FOUND 表示有不认识的选项）
 */
static int open_input(VideoState *is, AVFormatContext **out, AVDictionary *opts_template)
{
    AVFormatContext *nic;
    AVDictionary *opts = NULL;
    const AVDictionaryEntry *t;
    int i, err;

    nic = avformat_alloc_context();
    if (!nic)
        return AVERROR(ENOMEM);
    nic->interrupt_callback.callback = decode_interrupt_cb;
    nic->interrupt_callback.opaque = is;

    av_dict_copy(&opts, opts_template, 0);
    is->io_deadline = av_gettime_relative() + LIVE_OPEN_TIMEOUT_US;
    err = avformat_open_input(&nic, is->filename, is->iformat, &opts);  // 失败时会释放 nic 并置 NULL
    if (err >= 0) {
        // 没被消费的选项：scan_all_pmts 是 read_thread 自动加的，非 mpegts 不认识，忽略；其他就是写错了
        av_dict_set(&opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
        if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
            av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
            err = AVERROR_OPTION_NOT_FOUND;
        }
    }
    av_dict_free(&opts);
    if (err >= 0) {
        if (genpts)
            nic->flags |= AVFMT_FLAG_GENPTS;
        av_format_inject_global_side_data(nic);
    }
    if (err >= 0 && find_stream_info) {
        AVDictionary **sopts = setup_find_stream_info_opts(nic, codec_opts);
        int orig_nb_streams = nic->nb_streams;

        err = avformat_find_stream_info(nic, sopts);
        for (i = 0; i < orig_nb_streams; i++)
            av_dict_free(&sopts[i]);
        av_freep(&sopts);
    }
    is->io_deadline = 0;
    if (err < 0) {
        avformat_close_input(&nic);
        return err;
    }
    *out = nic;
    return 0;
}

/**
 * @brief 重新建立连接并替换掉旧的
 *
 * 顺序：先打开新连接、探测流信息；成功后才关闭旧的解码线程（它们引用旧 ic 的 AVStream），
 * 换上新 ic，再按原来打开了哪几种流重新打开。失败时旧的一切保持不变，画面停在最后一帧。
 * 重新打开的解码器、包队列 serial 都是新的，旧数据由 ffplay 的 serial 机制自动作废。
 *
 * @param[in,out] pic          read_thread 的局部 ic，成功后指向新连接
 * @param[in]     reopen_opts  第一次打开前保存的格式选项（如 rtsp_transport）
 * @return 0 = 成功；<0 = 打开失败的错误码
 */
static int reopen_input(VideoState *is, AVFormatContext **pic, AVDictionary *reopen_opts)
{
    AVFormatContext *nic = NULL;
    int had_audio = is->audio_stream >= 0;
    int had_video = is->video_stream >= 0;
    int had_sub   = is->subtitle_stream >= 0;
    int vi = -1, ai = -1, si = -1;
    int i, err;

    if ((err = open_input(is, &nic, reopen_opts)) < 0)
        return err;

    // 新连接就绪：关掉旧的解码线程，再换 ic
    if (had_audio)
        stream_component_close(is, is->audio_stream);
    if (had_video)
        stream_component_close(is, is->video_stream);
    if (had_sub)
        stream_component_close(is, is->subtitle_stream);

    is->ic = nic;
    avformat_close_input(pic);
    *pic = nic;
    is->realtime = is_realtime(nic);
    is->max_frame_duration = (nic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
    is->eof = 0;
    live_latency_reset(&is->live);  // 新连接的 pts 可能从头开始，延迟基准必须重建

    // 按原来打开了哪几种流，在新连接里选同类型的最佳流（不再套用 -ast/-vst/-sst）
    for (i = 0; i < nic->nb_streams; i++)
        nic->streams[i]->discard = AVDISCARD_ALL;
    if (had_video)
        vi = av_find_best_stream(nic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (had_audio)
        ai = av_find_best_stream(nic, AVMEDIA_TYPE_AUDIO, -1, vi, NULL, 0);
    if (had_sub)
        si = av_find_best_stream(nic, AVMEDIA_TYPE_SUBTITLE, -1, ai >= 0 ? ai : vi, NULL, 0);
    if (ai >= 0)
        stream_component_open(is, ai);
    if (vi >= 0)
        stream_component_open(is, vi);
    if (si >= 0)
        stream_component_open(is, si);

    if (is->video_stream < 0 && is->audio_stream < 0)
        return AVERROR_STREAM_NOT_FOUND;
    return 0;
}

/**
 * @brief 按 ExoPlayer 规则重试连接，直到成功、放弃或退出（首次连接失败和播放中断线共用）
 *
 * 第 n 次连续失败后等 min((n−1) × 1s, 5s)：0、1、2、3、4、5、5… 秒。
 * retry_count 只在读到包（有进展）时由调用方清零，所以"连上了但一个包都没读到又断"也会继续累加。
 *
 * @param[in,out] pic          首次连接时传入 NULL、成功后得到新连接；断线重连时是旧连接、成功后替换为新连接
 * @param[in]     initial      1 = 首次连接（还没有解码线程，只需打开）；0 = 播放中断线（要替换旧连接）
 * @param[in]     reason       导致失败的错误码（用于日志）
 * @param[in,out] retry_count  连续无进展的失败次数
 * @return 0 = 已连上；AVERROR_EXIT = 收到退出请求；其他 <0 = 放弃
 */
static int handle_disconnect(VideoState *is, AVFormatContext **pic, AVDictionary *reopen_opts,
                             SDL_mutex *wait_mutex, int initial, int reason, int *retry_count)
{
    int err = reason;
    AVFormatContext *old = *pic;
    const char *first_msg = initial ? "cannot connect" : "connection lost";
    int64_t lost_at = av_gettime_relative();
    int first = 1;

    /*
     * 旧连接已经断了、不会再读：先释放它占用的网络资源（avio），结构体留到新连接就绪时再整体关闭，
     * 主线程在这期间访问 is->ic 仍然安全。udp:// 这类要在本地绑定端口的输入，
     * 不先释放旧 socket 的话新连接会报 "Address already in use"。
     * AVFMT_NOFILE 的格式（如 rtsp）socket 归 demuxer 自己管，客户端端口是临时的，不会冲突。
     */
    if (old && old->pb && !(old->iformat->flags & AVFMT_NOFILE) && !(old->flags & AVFMT_FLAG_CUSTOM_IO))
        avio_closep(&old->pb);

    for (;;) {
        int delay_ms;
        char max_str[16] = "unlimited";

        (*retry_count)++;
        if (reconnect_max > 0 && *retry_count > reconnect_max) {
            av_log(NULL, AV_LOG_ERROR, "live: giving up after %d consecutive failures without progress (last error: %s)\n",
                   reconnect_max, av_err2str(err));
            return err;
        }
        delay_ms = FFMIN((*retry_count - 1) * LIVE_RETRY_STEP_MS, LIVE_RETRY_MAX_MS);
        if (reconnect_max > 0)
            snprintf(max_str, sizeof(max_str), "%d", reconnect_max);
        av_log(NULL, AV_LOG_WARNING, "live: %s (%s), reconnecting in %.0fs (attempt %d/%s)\n",
               first ? first_msg : "reconnect failed",
               av_err2str(err), delay_ms / 1000.0, *retry_count, max_str);
        first = 0;
        if (wait_or_abort(is, wait_mutex, delay_ms))
            return AVERROR_EXIT;

        err = initial ? open_input(is, pic, reopen_opts) : reopen_input(is, pic, reopen_opts);
        if (is->abort_request)
            return AVERROR_EXIT;
        if (err == AVERROR_EXIT)
            err = AVERROR(ETIMEDOUT);   // 被 io_deadline 中断 = 打开超时
        if (err >= 0) {
            av_log(NULL, AV_LOG_WARNING, "live: %s\n", initial ? "connected" : "reconnected");
            // 断线期间本来就没画面：重置看门狗计时，给新连接出画面的时间，否则数据一到就会被误判成卡死
            if (is->wd_last_shown)
                is->wd_last_shown = av_gettime_relative();
            is->wd_pkts_since_shown = 0;
            is->wd_level = 0;
            if (!initial) {
                is->metrics.reconnects++;
                is->metrics.disconnected_us += av_gettime_relative() - lost_at;
            }
            return 0;
        }
        if (is_fatal_open_error(err)) {
            av_log(NULL, AV_LOG_ERROR, "live: reconnect failed with non-retryable error: %s\n", av_err2str(err));
            return err;
        }
    }
}

/** 看门狗：数据在到、画面这么久没更新就判定卡死（同 WebRTC kMaxWaitForFrame = 3s） */
#define WATCHDOG_STALL_US    3000000
/** 看门狗：最近这么久没收到视频包，说明是网络问题（交给 4 秒读超时 + 重连处理），看门狗不管 */
#define WATCHDOG_INPUT_US    1000000
/** 看门狗：上次出画面以来至少收到这么多视频包（约 1.6 秒的 25fps 视频），才算"数据一直在到"；
 *  网络卡住后数据刚恢复的那一刻"有包 + 很久没画面"同时成立，只看最近 1 秒有没有包会误判 */
#define WATCHDOG_MIN_PKTS    40
/** 看门狗：上次恢复后画面持续正常这么久，升级计数清零 */
#define WATCHDOG_HEALTHY_US  10000000

/**
 * @brief 看门狗：网络正常但画面不动（解码器卡死、硬解异常、线程死锁）时逐级恢复
 *
 * 判定：低延迟模式、有视频、没暂停、已经出过画面、不在"损坏冻结等关键帧"中（那个有自己的 10 秒上限），
 * 最近 1 秒内还有视频包进来，但已经 3 秒没有显示新画面。
 * 恢复动作逐级升级，每级之间至少再等 3 秒看效果：
 *   1. 清空三个包队列（serial + 1，解码器 flush，从下一个关键帧重来）
 *   2. 关闭并重建视频解码器（解码线程卡住时只有这一级能救回来）
 *   3. 断开整条连接重连（复用 handle_disconnect）
 * 画面恢复正常 10 秒后，级别清零。
 *
 * @return 0 = 继续；AVERROR_EXIT = 退出；其他 <0 = 重连放弃
 */
static int watchdog_check(VideoState *is, AVFormatContext **pic, AVDictionary *reopen_opts,
                          SDL_mutex *wait_mutex, int *retry_count)
{
    int64_t now = av_gettime_relative();
    int vidx;

    if (!low_latency || is->video_stream < 0 || is->paused || is->step ||
        !is->wd_last_shown || is->vid_wait_bad)
        return 0;
    if (now - is->wd_last_vpkt > WATCHDOG_INPUT_US || is->wd_pkts_since_shown < WATCHDOG_MIN_PKTS)
        return 0;
    if (now - is->wd_last_shown < WATCHDOG_STALL_US) {
        if (is->wd_level && now - is->wd_level_time > WATCHDOG_HEALTHY_US)
            is->wd_level = 0;
        return 0;
    }
    if (is->wd_level && now - is->wd_level_time < WATCHDOG_STALL_US)
        return 0;     // 上一级动作的效果还没到时间看
    // 帧队列是满的：解码器在正常产出，是显示端（主线程）没在取帧——例如 macOS 窗口被遮挡 / 最小化时
    // 画面提交可能阻塞。这不是解码器的问题，清队列、重建解码器都没用，不动作
    if (frame_queue_nb_remaining(&is->pictq) >= is->pictq.max_size)
        return 0;

    is->wd_level = FFMIN(is->wd_level + 1, 3);
    is->wd_level_time = now;
    is->wd_triggers++;
    av_log(NULL, AV_LOG_WARNING, "live: watchdog: no new picture for %.1fs while data keeps arriving, recovery level %d (%s)\n",
           (now - is->wd_last_shown) / 1000000.0, is->wd_level,
           is->wd_level == 1 ? "flush queues" : is->wd_level == 2 ? "recreate video decoder" : "reconnect");

    switch (is->wd_level) {
    case 1:
        if (is->audio_stream >= 0)
            packet_queue_flush(&is->audioq);
        if (is->subtitle_stream >= 0)
            packet_queue_flush(&is->subtitleq);
        packet_queue_flush(&is->videoq);
        return 0;
    case 2:
        vidx = is->video_stream;
        stream_component_close(is, vidx);   // abort 包队列并等解码线程退出
        if (stream_component_open(is, vidx) < 0)
            av_log(NULL, AV_LOG_ERROR, "live: watchdog: failed to reopen the video decoder\n");
        return 0;
    default:
        return handle_disconnect(is, pic, reopen_opts, wait_mutex, 0, AVERROR(ETIMEDOUT), retry_count);
    }
}

/* this thread gets the stream from the disk or the network */
int read_thread(void *arg)
{
    VideoState *is = arg;
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket *pkt = NULL;
    int64_t stream_start_time;
    int pkt_in_play_range = 0;
    const AVDictionaryEntry *t;
    SDL_mutex *wait_mutex = SDL_CreateMutex();
    int scan_all_pmts_set = 0;
    int64_t pkt_ts;
    AVDictionary *reopen_opts = NULL;  // 直播重连用：第一次打开前的格式选项副本（avformat_open_input 会改写 format_opts）
    int retry_count = 0;               // 直播重连：连续没有读到包的失败次数，读到包就清零

    if (!wait_mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(st_index, -1, sizeof(st_index));
    is->eof = 0;

    pkt = av_packet_alloc();
    if (!pkt) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    // 低延迟模式：探测阶段少缓存包、最多分析 1 秒（默认 5 秒），加快起播和重连（用户自己给了就不覆盖）
    if (low_latency) {
        av_dict_set(&format_opts, "fflags", "nobuffer", AV_DICT_DONT_OVERWRITE);
        av_dict_set(&format_opts, "analyzeduration", "1000000", AV_DICT_DONT_OVERWRITE);
    }
    if (reconnect_enabled()) {
        av_dict_copy(&reopen_opts, format_opts, 0);
        is->io_deadline = av_gettime_relative() + LIVE_OPEN_TIMEOUT_US;  // 首次连接也加超时，防止卡死
    }
    err = avformat_open_input(&ic, is->filename, is->iformat, &format_opts);  // 失败时会释放 ic 并置 NULL
    if (err >= 0) {
        if (scan_all_pmts_set)
            av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

        if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
            av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
            ret = AVERROR_OPTION_NOT_FOUND;
            goto fail;
        }

        if (genpts)
            ic->flags |= AVFMT_FLAG_GENPTS;

        av_format_inject_global_side_data(ic);

        if (find_stream_info) {
            AVDictionary **opts = setup_find_stream_info_opts(ic, codec_opts);
            int orig_nb_streams = ic->nb_streams;

            err = avformat_find_stream_info(ic, opts);

            for (i = 0; i < orig_nb_streams; i++)
                av_dict_free(&opts[i]);
            av_freep(&opts);

            if (err < 0) {
                av_log(NULL, AV_LOG_WARNING,
                       "%s: could not find codec parameters\n", is->filename);
                avformat_close_input(&ic);   // 当作连接失败：直播时进入下面的重试
            }
        }
    }
    is->io_deadline = 0;

    // 首次连接失败（摄像头还没上线、网络暂时不通）：直播重连模式下按同样的规则重试
    if (err < 0 && reconnect_enabled() && !is->abort_request && !is_fatal_open_error(err)) {
        if (err == AVERROR_EXIT)
            err = AVERROR(ETIMEDOUT);   // 被 io_deadline 中断 = 连接超时
        err = handle_disconnect(is, &ic, reopen_opts, wait_mutex, 1, err, &retry_count);
        if (err == AVERROR_EXIT) {      // 等待期间用户关闭了窗口
            ret = 0;
            goto fail;
        }
    }
    if (err < 0) {
        print_error(is->filename, err);
        ret = -1;
        goto fail;
    }
    is->ic = ic;

    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    if (seek_by_bytes < 0)
        seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                        !!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
                        strcmp("ogg", ic->iformat->name);

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
        window_title = av_asprintf("%s - %s", t->value, input_filename);

    /* if seeking requested, we execute it */
    if (start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                    is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    is->realtime = is_realtime(ic);

    if (show_status)
        av_dump_format(ic, 0, is->filename, 0);

    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1)
            if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0)
                st_index[type] = i;
    }
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (wanted_stream_spec[i] && st_index[i] == -1) {
            av_log(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n", wanted_stream_spec[i], av_get_media_type_string(i));
            st_index[i] = INT_MAX;
        }
    }

    if (!video_disable)
        st_index[AVMEDIA_TYPE_VIDEO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    if (!audio_disable)
        st_index[AVMEDIA_TYPE_AUDIO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                st_index[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                NULL, 0);
    if (!video_disable && !subtitle_disable)
        st_index[AVMEDIA_TYPE_SUBTITLE] =
            av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                                st_index[AVMEDIA_TYPE_SUBTITLE],
                                (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                                 st_index[AVMEDIA_TYPE_AUDIO] :
                                 st_index[AVMEDIA_TYPE_VIDEO]),
                                NULL, 0);

    is->show_mode = show_mode;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
        if (codecpar->width)
            set_default_window_size(codecpar->width, codecpar->height, sar);
    }

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (is->show_mode == SHOW_MODE_NONE)
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               is->filename);
        ret = -1;
        goto fail;
    }

    if (infinite_buffer < 0 && is->realtime)
        infinite_buffer = 1;
    // 低延迟模式：读线程有多少读多少，不让数据堆在 socket 里；延迟由显示端按截止时间丢帧来控制
    if (low_latency) {
        infinite_buffer = 1;
        // 延迟估计依赖"包按实时节奏到达"，本地文件 / 点播会被一口气读完，估计会失真
        if (ic->pb && (ic->pb->seekable & AVIO_SEEKABLE_NORMAL))
            av_log(NULL, AV_LOG_WARNING, "-lowlatency is meant for live streams; '%s' looks seekable (file/VOD), playback timing may be wrong\n",
                   is->filename);
    }

    for (;;) {
        if (is->abort_request)
            break;
        ret = watchdog_check(is, &ic, reopen_opts, wait_mutex, &retry_count);
        if (ret == AVERROR_EXIT)
            break;
        if (ret < 0)
            goto fail;
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
                (!strcmp(ic->iformat->name, "rtsp") ||
                 (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            SDL_Delay(10);
            continue;
        }
#endif
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;
// FIXME the +-2 is due to rounding being not done in the correct direction in generation
//      of the seek_pos/seek_rel variables

            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "%s: error while seeking\n", is->ic->url);
            } else {
                if (is->audio_stream >= 0)
                    packet_queue_flush(&is->audioq);
                if (is->subtitle_stream >= 0)
                    packet_queue_flush(&is->subtitleq);
                if (is->video_stream >= 0)
                    packet_queue_flush(&is->videoq);
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                   set_clock(&is->extclk, NAN, 0);
                } else {
                   set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
            if (is->paused)
                step_to_next_frame(is);
        }
        if (is->queue_attachments_req) {
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                if ((ret = av_packet_ref(pkt, &is->video_st->attached_pic)) < 0)
                    goto fail;
                packet_queue_put(&is->videoq, pkt);
                packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        /* if the queue are full, no need to read more */
        if (infinite_buffer<1 &&
              (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE
            || (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
                stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
                stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)))) {
            /* wait 10 ms */
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }
        if (!is->paused &&
            (!is->audio_st || (is->auddec.finished == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0)) &&
            (!is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0))) {
            if (loop != 1 && (!loop || --loop)) {
                stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
            } else if (autoexit) {
                ret = AVERROR_EOF;
                goto fail;
            }
        }
        // 直播重连模式：给这次读包设截止时间，4 秒没数据 decode_interrupt_cb 会让它返回错误
        if (reconnect_enabled())
            is->io_deadline = av_gettime_relative() + LIVE_READ_TIMEOUT_US;
        ret = av_read_frame(ic, pkt);
        is->io_deadline = 0;
        if (ret < 0 && reconnect_enabled() && !is->abort_request && ret != AVERROR(EAGAIN)) {
            // 直播没有"播完"：EOF（服务器断开）、IO 错误、读超时都当断线处理
            if (ret == AVERROR_EXIT)
                ret = AVERROR(ETIMEDOUT);   // 被 io_deadline 中断 = 4 秒没收到数据
            ret = handle_disconnect(is, &ic, reopen_opts, wait_mutex, 0, ret, &retry_count);
            if (ret == AVERROR_EXIT)
                break;
            if (ret < 0)
                goto fail;      // 放弃：投递 FF_QUIT_EVENT 退出
            continue;
        }
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, pkt, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    packet_queue_put_nullpacket(&is->subtitleq, pkt, is->subtitle_stream);
                is->eof = 1;
            }
            if (ic->pb && ic->pb->error) {
                if (autoexit)
                    goto fail;
                else
                    break;
            }
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        } else {
            is->eof = 0;
            retry_count = 0;    // 读到包 = 有进展，重连计数清零（同 ExoPlayer）
            is->metrics.bytes_read += pkt->size;
        }
        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range = duration == AV_NOPTS_VALUE ||
                (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                av_q2d(ic->streams[pkt->stream_index]->time_base) -
                (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000
                <= ((double)duration / 1000000);
        if (low_latency && pkt_in_play_range)
            live_on_packet(is, pkt);
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range
                   && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            is->wd_last_vpkt = av_gettime_relative();
            is->wd_pkts_since_shown++;
            packet_queue_put(&is->videoq, pkt);
        } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }

    ret = 0;
 fail:
    if (ic && !is->ic)
        avformat_close_input(&ic);

    av_packet_free(&pkt);
    av_dict_free(&reopen_opts);
    if (ret != 0) {
        SDL_Event event;

        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    SDL_DestroyMutex(wait_mutex);
    return 0;
}

/*
 * 本文件拆自 FFmpeg 6.0 fftools/ffplay.c（Copyright (c) 2003 Fabrice Bellard），
 * 遵循 GNU LGPL 2.1 或更高版本。函数体保持原样，仅调整 static 和 #include。
 */
#include "audio/audio_output.h"
#include "common/player_options.h"
#include "sync/av_sync.h"
#include "ui/sdl_context.h"
#include "video/audio_visualizer.h"

int64_t audio_callback_time;

/** 低延迟模式下，音频帧结束时刻比主时钟（视频）早这么多秒就丢弃 */
#define LIVE_AUDIO_DROP_THRESHOLD 0.08

/**
 * @brief 低延迟模式：音频帧已经落后于视频主时钟，直接丢掉，让声音追上画面
 * @note 原版用 synchronize_audio 最多 ±10% 增减采样慢慢追，卡顿后积压的几百毫秒要追很久
 */
static int audio_frame_too_late(VideoState *is, Frame *af)
{
    double master, end;

    if (!low_latency || isnan(af->pts) || get_master_sync_type(is) == AV_SYNC_AUDIO_MASTER)
        return 0;
    master = get_master_clock(is);
    if (isnan(master))
        return 0;
    end = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
    return end < master - LIVE_AUDIO_DROP_THRESHOLD;
}

/**
 * @brief 低延迟模式：音频帧还没到显示时刻（pts + base + target，与视频同一个截止时间），先不播
 * @note 视频要等到截止时间才显示，音频如果解出就播会比画面超前约 target。
 *       借鉴 WebRTC"谁快就给谁加延迟"：音频也按同一截止时间播放，没到就输出一段静音、帧留在队列里。
 *       这次写进 SDL 缓冲的数据要过约 2 个硬件缓冲才真正响，所以截止时间减去这段延迟再比较。
 */
static int audio_frame_too_early(VideoState *is, Frame *af)
{
    double rt, now, hw_delay;

    // 只在"开始出声"时对齐截止时间（起播、从断音恢复、跳转后）；正在连续播放时绝不插静音，
    // 否则目标延迟 / 基准的小幅波动会让音频被反复判成"早了"，变成一段一段的（实测每秒约 10 次断音）。
    // 连续播放中的音画偏差交给 synchronize_audio 的 ±10% 采样补偿慢慢修正（同 NetEq：靠变速而不是插静音）。
    if (!low_latency || !is->video_st || isnan(af->pts) || is->audio_running)
        return 0;
    rt = live_latency_render_time(&is->live, af->pts);
    if (isnan(rt))
        return 0;
    now      = audio_callback_time / 1000000.0;
    hw_delay = 2.0 * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec;
    return rt > now + hw_delay;
}

/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
int audio_decode_frame(VideoState *is)
{
    int data_size, resampled_data_size;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;

    if (is->paused)
        return -1;

    for (;;) {
#if defined(_WIN32)
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            if ((av_gettime_relative() - audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
                return -1;
            av_usleep (1000);
        }
#endif
        if (!(af = frame_queue_peek_readable(&is->sampq)))
            return -1;
        if (af->serial == is->audioq.serial) {
            // 和上一段接不上（跳关键帧 / 重连后 serial 变了，或时间戳跳了 20ms 以上）：当作重新开始出声
            if (low_latency && is->audio_running &&
                (af->serial != is->audio_clock_serial ||
                 (!isnan(af->pts) && !isnan(is->audio_clock) && fabs(af->pts - is->audio_clock) > 0.02)))
                is->audio_running = 0;
            // 还没到播放时刻：不取走这帧，返回 -1 让回调输出一小段静音
            if (audio_frame_too_early(is, af)) {
                is->audio_waits++;
                return -1;
            }
        }
        frame_queue_next(&is->sampq);
        if (af->serial == is->audioq.serial && !audio_frame_too_late(is, af))
            break;
        // 跳过了帧（旧 serial 或落后太多）：下一段和上一段接不上
        if (af->serial == is->audioq.serial)
            is->audio_drops++;
        audio_smooth_mark_discont(&is->asmooth);
    }
    // 时间戳和上一帧的结束时刻差 20ms 以上（丢包、重连、跳关键帧）：同样接不上
    if (!isnan(af->pts) && !isnan(is->audio_clock) && fabs(af->pts - is->audio_clock) > 0.02)
        audio_smooth_mark_discont(&is->asmooth);

    data_size = av_samples_get_buffer_size(NULL, af->frame->ch_layout.nb_channels,
                                           af->frame->nb_samples,
                                           af->frame->format, 1);

    wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

    if (af->frame->format        != is->audio_src.fmt            ||
        av_channel_layout_compare(&af->frame->ch_layout, &is->audio_src.ch_layout) ||
        af->frame->sample_rate   != is->audio_src.freq           ||
        (wanted_nb_samples       != af->frame->nb_samples && !is->swr_ctx)) {
        swr_free(&is->swr_ctx);
        swr_alloc_set_opts2(&is->swr_ctx,
                            &is->audio_tgt.ch_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                            &af->frame->ch_layout, af->frame->format, af->frame->sample_rate,
                            0, NULL);
        if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), af->frame->ch_layout.nb_channels,
                    is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.ch_layout.nb_channels);
            swr_free(&is->swr_ctx);
            return -1;
        }
        if (av_channel_layout_copy(&is->audio_src.ch_layout, &af->frame->ch_layout) < 0)
            return -1;
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = af->frame->format;
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
        int out_size  = av_samples_get_buffer_size(NULL, is->audio_tgt.ch_layout.nb_channels, out_count, is->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
                                        wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1)
            return AVERROR(ENOMEM);
        len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }
        is->audio_buf = is->audio_buf1;
        resampled_data_size = len2 * is->audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    } else {
        is->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    audio_clock0 = is->audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(af->pts))
        is->audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
    else
        is->audio_clock = NAN;
    is->audio_clock_serial = af->serial;
#ifdef DEBUG
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
               is->audio_clock - last_clock,
               is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif
    return resampled_data_size;
}

static int smooth_enabled(void)
{
    return audio_smooth > 0 || (audio_smooth < 0 && low_latency);
}

/**
 * @brief 仅供测试：环境变量 SIMPLEFFPLAY_TEST_AUDIO_DUMP=<文件> 时，把实际交给声卡的 PCM（S16 交错）原样追加写入，
 *        测试脚本用它检测"咔哒"跳变。文件头 12 字节："SFPA" + int32 采样率 + int32 声道数（本机字节序）
 */
static void test_dump_audio(const Uint8 *data, int len, int freq, int channels)
{
    static FILE *f;
    static int inited;

    if (!inited) {
        const char *path = getenv("SIMPLEFFPLAY_TEST_AUDIO_DUMP");
        inited = 1;
        f = path ? fopen(path, "wb") : NULL;
        if (f) {
            int32_t hdr[2] = { freq, channels };
            fwrite("SFPA", 1, 4, f);
            fwrite(hdr, sizeof(hdr), 1, f);
        }
    }
    if (f) {
        fwrite(data, 1, len, f);
        fflush(f);
    }
}

/* prepare a new audio buffer */
void sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
    VideoState *is = opaque;
    int audio_size, len1;
    Uint8 *stream_start = stream;
    int len_total = len;
    int channels = is->audio_tgt.ch_layout.nb_channels;

    audio_callback_time = av_gettime_relative();

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
           int waits = is->audio_waits;
           audio_size = audio_decode_frame(is);
           if (audio_size < 0) {
               const uint8_t *fade = NULL;
               int fade_size = 0;

               if (is->audio_waits == waits && is->audio_running)
                   is->audio_underruns++;
               is->audio_running = 0;
               // 有声 → 没数据：先播 5ms 渐弱，避免波形突然掉到 0 产生咔哒声
               if (smooth_enabled())
                   fade_size = audio_smooth_on_silence(&is->asmooth, &fade, channels, is->audio_tgt.freq);
               if (fade_size > 0) {
                   is->audio_buf = (uint8_t *)fade;
                   is->audio_buf_size = fade_size;
               } else {
                   /* if error, just output silence */
                   is->audio_buf = NULL;
                   is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
               }
           } else {
               is->audio_running = 1;
               // 从静音恢复 / 丢过帧 / 时间戳跳变：开头 5ms 和上一段做交叉淡化
               if (smooth_enabled())
                   is->audio_buf = (uint8_t *)audio_smooth_on_audio(&is->asmooth, is->audio_buf, audio_size,
                                                                     channels, is->audio_tgt.freq);
               if (is->show_mode != SHOW_MODE_VIDEO)
                   update_sample_display(is, (int16_t *)is->audio_buf, audio_size);
               is->audio_buf_size = audio_size;
           }
           is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        else {
            memset(stream, 0, len1);
            if (!is->muted && is->audio_buf)
                SDL_MixAudioFormat(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, AUDIO_S16SYS, len1, is->audio_volume);
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    test_dump_audio(stream_start, len_total, is->audio_tgt.freq, channels);
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        set_clock_at(&is->audclk, is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec, is->audio_clock_serial, audio_callback_time / 1000000.0);
        sync_clock_to_slave(&is->extclk, &is->audclk);
    }
}

int audio_open(void *opaque, AVChannelLayout *wanted_channel_layout, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
    int wanted_nb_channels = wanted_channel_layout->nb_channels;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    wanted_nb_channels = wanted_channel_layout->nb_channels;
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    // 低延迟模式：每秒回调 120 次，44.1k/48k 下缓冲 512 采样 ≈ 11ms（原版约 2048 采样 ≈ 46ms）
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / (low_latency ? 120 : SDL_AUDIO_MAX_CALLBACKS_PER_SEC)));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;
    while (!(audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(NULL, AV_LOG_ERROR,
                       "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        av_channel_layout_default(wanted_channel_layout, wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, spec.channels);
        if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
            av_log(NULL, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    if (av_channel_layout_copy(&audio_hw_params->ch_layout, wanted_channel_layout) < 0)
        return -1;
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }
    return spec.size;
}

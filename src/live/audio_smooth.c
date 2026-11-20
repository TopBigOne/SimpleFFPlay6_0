#include "live/audio_smooth.h"

/** 衔接窗口：5ms */
#define AUDIO_SMOOTH_WINDOW_SEC 0.005

static int window_frames(int freq)
{
    return av_clip((int)(freq * AUDIO_SMOOTH_WINDOW_SEC), 1, AUDIO_SMOOTH_MAX_FRAMES);
}

/** 尾巴的镜像延续：第 i 帧取倒数第 i+1 帧，和上一个输出样本连续；超出尾巴长度后来回折返 */
static int16_t mirror(const AudioSmooth *s, int i, int c, int channels)
{
    int n = s->tail_frames, k;

    if (n <= 0)
        return 0;
    k = i % (2 * n);
    k = k < n ? n - 1 - k : k - n;
    return s->tail[k * channels + c];
}

static void save_tail(AudioSmooth *s, const int16_t *pcm, int frames, int channels, int freq)
{
    int w = window_frames(freq);

    if (frames >= w) {
        memcpy(s->tail, pcm + (frames - w) * channels, w * channels * sizeof(int16_t));
        s->tail_frames = w;
    } else {
        // 新数据不够一个窗口：旧尾巴往前挪，拼上新数据
        int keep = FFMIN(s->tail_frames, w - frames);
        memmove(s->tail, s->tail + (s->tail_frames - keep) * channels, keep * channels * sizeof(int16_t));
        memcpy(s->tail + keep * channels, pcm, frames * channels * sizeof(int16_t));
        s->tail_frames = keep + frames;
    }
}

void audio_smooth_reset(AudioSmooth *s)
{
    s->tail_frames = 0;
    s->playing     = 0;
    s->discont     = 0;
}

void audio_smooth_free(AudioSmooth *s)
{
    av_freep(&s->buf);
    s->buf_size = 0;
    audio_smooth_reset(s);
}

void audio_smooth_mark_discont(AudioSmooth *s)
{
    s->discont = 1;
}

const uint8_t *audio_smooth_on_audio(AudioSmooth *s, const uint8_t *data, int bytes, int channels, int freq)
{
    int frames = bytes / (int)(channels * sizeof(int16_t));
    const int16_t *pcm = (const int16_t *)data;

    if (channels <= 0 || channels > AUDIO_SMOOTH_MAX_CH || frames <= 0)
        return data;

    if (!s->playing || s->discont) {
        int w = FFMIN(window_frames(freq), frames), i, c;
        int16_t *out;

        av_fast_malloc(&s->buf, &s->buf_size, bytes);
        if (!s->buf)
            return data;
        memcpy(s->buf, data, bytes);
        out = (int16_t *)s->buf;
        // 上一段是静音时尾巴视为 0：交叉淡化退化为淡入
        if (!s->playing)
            s->tail_frames = 0;
        for (i = 0; i < w; i++) {
            double g = (i + 0.5) / w;   // 新数据 0 → 1，尾巴 1 → 0
            for (c = 0; c < channels; c++) {
                int idx = i * channels + c;
                out[idx] = av_clip_int16(lrint(out[idx] * g + mirror(s, i, c, channels) * (1.0 - g)));
            }
        }
        s->fades++;
        pcm = out;
        data = s->buf;
    }
    save_tail(s, pcm, frames, channels, freq);
    s->playing = 1;
    s->discont = 0;
    return data;
}

int audio_smooth_on_silence(AudioSmooth *s, const uint8_t **out, int channels, int freq)
{
    int w = window_frames(freq), bytes, i, c;
    int16_t *pcm;

    if (!s->playing || s->tail_frames <= 0 || channels <= 0 || channels > AUDIO_SMOOTH_MAX_CH) {
        s->playing = 0;
        return 0;
    }
    bytes = w * channels * sizeof(int16_t);
    av_fast_malloc(&s->buf, &s->buf_size, bytes);
    if (!s->buf) {
        s->playing = 0;
        return 0;
    }
    pcm = (int16_t *)s->buf;
    for (i = 0; i < w; i++) {
        double g = 1.0 - (i + 0.5) / w;   // 1 → 0
        for (c = 0; c < channels; c++)
            pcm[i * channels + c] = av_clip_int16(lrint(mirror(s, i, c, channels) * g));
    }
    s->fades++;
    s->playing = 0;
    s->tail_frames = 0;
    *out = s->buf;
    return bytes;
}

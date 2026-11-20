#pragma once

/**
 * @file
 * @brief 音频衔接（-audio_smooth）：消除断音、跳变时的"咔哒"爆音，借鉴 WebRTC NetEq 的 Expand / Merge
 *
 * 爆音的来源：输出的波形突然跳变（有声 → 静音、丢帧后新旧波形接不上、静音 → 突然有声）。
 * 做法（只处理衔接处的 5ms，不额外增加延迟）：
 *  - 一直记住刚输出的最后 5ms（尾巴）。
 *  - 有声 → 没数据：先输出 5ms 由尾巴"镜像延续"并渐弱到 0 的声音，再输出静音（对应 NetEq Expand 的渐弱）。
 *    镜像（倒放尾巴）保证和上一个样本连续，不会在接缝处再跳一次。
 *  - 新数据和上一段不连续（从静音恢复、丢过帧、时间戳跳变）：新数据开头 5ms 与尾巴镜像做交叉淡化
 *    （对应 NetEq Merge）；从静音恢复时尾巴为 0，交叉淡化就是淡入。
 *
 * 只在 SDL 音频回调线程里使用，不需要加锁。输入固定是 audio_open 选定的 S16 交错格式。
 */

#include "common/ffplay_common.h"

/** 衔接窗口最长采样帧数（48kHz 下 5ms = 240 帧，留余量） */
#define AUDIO_SMOOTH_MAX_FRAMES 1024
/** 最多支持的声道数 */
#define AUDIO_SMOOTH_MAX_CH     8

typedef struct AudioSmooth {
    int16_t  tail[AUDIO_SMOOTH_MAX_FRAMES * AUDIO_SMOOTH_MAX_CH]; ///< 最近输出的最后 tail_frames 帧（交错）
    int      tail_frames;   ///< 尾巴里有效的采样帧数
    int      playing;       ///< 1 = 上一段输出的是真实音频（不是静音）
    int      discont;       ///< 1 = 下一段真实音频和上一段不连续，需要交叉淡化
    uint8_t *buf;           ///< 处理后数据的暂存区
    unsigned buf_size;
    int      fades;         ///< 累计做了多少次衔接处理（淡出 + 交叉淡化）
} AudioSmooth;

/** @brief 清空状态（设备重开、关闭流时调用），保留暂存区 */
void audio_smooth_reset(AudioSmooth *s);

/** @brief 释放暂存区 */
void audio_smooth_free(AudioSmooth *s);

/** @brief 标记"下一段音频和上一段不连续"（丢过帧、时间戳跳变时调用） */
void audio_smooth_mark_discont(AudioSmooth *s);

/**
 * @brief 一段真实音频要输出了：必要时对开头做交叉淡化，并更新尾巴
 * @param[in] data      S16 交错 PCM
 * @param[in] bytes     字节数
 * @param[in] channels  声道数
 * @param[in] freq      采样率
 * @return 要输出的数据（不需要处理时就是 data 本身，否则指向内部暂存区）；分配失败时返回 data
 */
const uint8_t *audio_smooth_on_audio(AudioSmooth *s, const uint8_t *data, int bytes, int channels, int freq);

/**
 * @brief 没有数据、要输出静音了
 * @param[out] out  需要先播一段渐弱声音时指向它
 * @return 渐弱声音的字节数；0 = 直接输出静音即可（本来就是静音，或没有尾巴）
 */
int audio_smooth_on_silence(AudioSmooth *s, const uint8_t **out, int channels, int freq);

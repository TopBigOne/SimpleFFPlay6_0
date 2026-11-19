/*
 * 本文件拆自 FFmpeg 6.0 fftools/ffplay.c（Copyright (c) 2003 Fabrice Bellard），
 * 遵循 GNU LGPL 2.1 或更高版本。
 */
#include "common/player_options.h"                       // 这些全局变量的 extern 声明

unsigned sws_flags = SWS_BICUBIC;                        // sws_scale 缩放算法：双三次插值（不支持的像素格式转 BGRA 时用）

/* options specified by the user */
const AVInputFormat *file_iformat;                       // -f 强制指定的输入格式；NULL = 让 FFmpeg 自动探测
// 定义（不带 extern）
const char *input_filename;                              // 要播放的文件路径或 URL（命令行里不带 - 的参数）
const char *window_title;                                // -window_title 窗口标题；NULL = 用文件名
int default_width = 640;                                 // 窗口默认宽度；没有视频或还没拿到视频尺寸时使用
int default_height = 480;                                // 窗口默认高度；同上
int screen_width = 0;                                    // -x 强制窗口宽度；0 = 按视频尺寸
int screen_height = 0;                                   // -y 强制窗口高度；0 = 按视频尺寸
int screen_left = SDL_WINDOWPOS_CENTERED;                // -left 窗口左上角 x 坐标；默认屏幕居中
int screen_top = SDL_WINDOWPOS_CENTERED;                 // -top 窗口左上角 y 坐标；默认屏幕居中
int audio_disable;                                       // -an 禁用音频（全局变量默认 0 = 不禁用）
int video_disable;                                       // -vn 禁用视频
int subtitle_disable;                                    // -sn 禁用字幕
const char *wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};   // -ast/-vst/-sst 按媒体类型指定要播放的流；NULL = 自动选最佳流
int seek_by_bytes = -1;                                  // -bytes 按字节偏移 seek：1 = 是，0 = 否，-1 = 按容器格式自动决定
float seek_interval = 10;                                // -seek_interval 左右方向键每次 seek 的秒数
int display_disable;                                     // -nodisp 不创建窗口、不显示画面（只播声音）
int borderless;                                          // -noborder 无边框窗口
int alwaysontop;                                         // -alwaysontop 窗口置顶
int startup_volume = 100;                                // -volume 启动音量，范围 0~100
int show_status = -1;                                    // -stats 控制台打印播放状态（A-V 差值、队列大小等）；-1 = 自动
int av_sync_type = AV_SYNC_AUDIO_MASTER;                 // -sync 音画同步的主时钟：默认以音频为准
int64_t start_time = AV_NOPTS_VALUE;                     // -ss 起播位置（微秒）；AV_NOPTS_VALUE = 从头播
int64_t duration = AV_NOPTS_VALUE;                       // -t 只播放这么长（微秒）；AV_NOPTS_VALUE = 播到结尾
int fast = 0;                                            // -fast 开启不严格符合标准的解码加速（AV_CODEC_FLAG2_FAST）
int genpts = 0;                                          // -genpts 让 demuxer 为缺失 pts 的包生成 pts（AVFMT_FLAG_GENPTS）
int lowres = 0;                                          // -lowres 低分辨率解码等级（缩小 2^n 倍）；0 = 原尺寸
int hwaccel = 1;                                         // -hwaccel 视频优先硬解（macOS 用 VideoToolbox）；默认开，-nohwaccel 关闭
int low_latency = 0;                                     // -lowlatency 安防低延迟直播模式：不停读、按截止时间显示 / 丢帧、视频做主时钟
int latency_min_ms = 30;                                 // -latency_min 目标延迟下限（毫秒）
int latency_max_ms = 300;                                // -latency_max 目标延迟上限（毫秒）；积压超过它的 2 倍就跳到下一个关键帧
int live_reconnect = -1;                                 // -live_reconnect 直播断线自动重连：1 = 开，0 = 关，-1 = 开了 -lowlatency 就开
int reconnect_max = 6;                                   // -reconnect_max 连续多少次没有读到任何包才放弃（同 ExoPlayer）；0 = 无限重试
int decoder_reorder_pts = -1;                            // -drp 帧时间戳来源：1 = 用 pts，0 = 用 dts，-1 = 用 best_effort_timestamp
int autoexit;                                            // -autoexit 播放结束后自动退出
int exit_on_keydown;                                     // -exitonkeydown 按任意键退出
int exit_on_mousedown;                                   // -exitonmousedown 点击鼠标退出
int loop = 1;                                            // -loop 播放次数：1 = 播一遍，0 = 无限循环
int framedrop = -1;                                      // -framedrop 视频落后主时钟时丢帧：1 = 丢，0 = 不丢，-1 = 主时钟不是视频时才丢
int infinite_buffer = -1;                                // -infbuf 不限制包队列缓存大小：1 = 不限，0 = 限，-1 = 实时流自动不限
enum ShowMode show_mode = SHOW_MODE_NONE;                // -showmode 显示模式（视频 / 波形 / 频谱）；NONE = 有视频显示视频，否则显示频谱
const char *audio_codec_name;                            // -acodec 强制指定音频解码器名；NULL = 按编码格式自动选
const char *subtitle_codec_name;                         // -scodec 强制指定字幕解码器名
const char *video_codec_name;                            // -vcodec 强制指定视频解码器名
double rdftspeed = 0.02;                                 // -rdftspeed 波形/频谱画面的刷新间隔（秒）
#if CONFIG_AVFILTER                                      // 下面三个只在编译了 libavfilter 时存在
const char **vfilters_list = NULL;                       // -vf 视频滤镜描述数组；可多次指定，播放时按 w 键循环切换
int nb_vfilters = 0;                                     // vfilters_list 里的滤镜个数
char *afilters = NULL;                                   // -af 音频滤镜描述；NULL = 不加滤镜
#endif
int autorotate = 1;                                      // -autorotate 按视频的旋转元数据（displaymatrix）自动旋转画面
int find_stream_info = 1;                                // -find_stream_info 打开文件后调用 avformat_find_stream_info 探测流信息
int filter_nbthreads = 0;                                // -filter_threads 滤镜图使用的线程数；0 = 自动

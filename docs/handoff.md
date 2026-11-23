# 交接说明：直播低延迟 / 可靠性改造（截至 2026-11-20）

> 给接手的会话或开发者。读完这一篇就能接着干，不需要之前的对话记录。
>
> **2026-11-23 更新**：§4.1~4.4 已完成——音频断音查清并解决（音频稳定晚到时接受滞后）、运行指标 `-metrics_file`、技术文档 [live_reliability.md](live_reliability.md)、完整测试集回归（含音频与指标检查）。另修了看门狗的三处误判。剩余事项只有 §4.5 的暂缓项和故障恢复瞬间偶发 1 次音频咔哒（live_reliability.md §3.5）。下文 §2~§4 保留原样作为背景。

## 1. 项目与目标

SimpleFFPlay6.0：把 FFmpeg 6.0 的 `ffplay.c` 拆成模块化的 C 工程（`src/` 下按 pipeline / demux / audio / video / sync 等分目录），在此基础上做**家庭安防监控直播播放器**：

- 端到端延迟目标 **300ms 以内**，播放器自身预算约 100ms；
- 稳定性 / 可靠性**向 WebRTC 接收端对齐**（只做接收端能做的；NACK / FEC / PLI / 拥塞控制需要摄像头配合，做不了，见 [live_reference_ijkplayer_webrtc.md](live_reference_ijkplayer_webrtc.md)）。

所有直播能力都挂在 `-lowlatency` 开关下，默认关闭，不影响普通文件播放。

## 2. 已完成（都已提交）

| 功能 | 关键位置 | 说明文档 |
|---|---|---|
| VideoToolbox 硬解（默认开，`-nohwaccel` 关） | [stream.c](../src/demux/stream.c) `setup_videotoolbox()`、[decoder.c](../src/pipeline/decoder.c) 硬件帧下载 | — |
| 追帧：WebRTC 式截止时间显示 / 丢帧 / 积压跳关键帧 / 音频跟视频 | [live/live_latency.c](../src/live/live_latency.c)、[video_refresh.c](../src/video/video_refresh.c)、[read_thread.c](../src/demux/read_thread.c) `live_on_packet()` | [live_catchup_algorithm.md](live_catchup_algorithm.md) |
| 断线重连 + 首次连接重试（0/1/2/3/4/5s，连续 6 次无进展放弃，同 ExoPlayer） | [read_thread.c](../src/demux/read_thread.c) `open_input()` / `reopen_input()` / `handle_disconnect()` | 同上 §7 |
| 丢包不花屏：冻结在最后一个好帧等关键帧 | [video_decoder_thread.c](../src/video/video_decoder_thread.c) `frame_integrity_ok()`、[decoder.c](../src/pipeline/decoder.c) 损坏标记传递 | 同上 §7.1 |
| 故障注入测试集（11 个场景） | [tests/live/](../tests/live/) | [tests/live/README.md](../tests/live/README.md) |
| **第 4 步** 看门狗：数据在到但 3 秒没新画面 → 清队列 → 重建解码器 → 重连 | [read_thread.c](../src/demux/read_thread.c) `watchdog_check()` | **待写**（见 §4） |
| 抖动估计 ±3.5σ 限幅（前 30 帧不限） | [live_latency.c](../src/live/live_latency.c) `live_latency_on_frame()` | **待补进** live_catchup_algorithm.md §3.2 |
| **第 5 步** 音频衔接：断音 5ms 渐弱、恢复 / 跳变 5ms 交叉淡化（NetEq Expand/Merge 简化版） | [live/audio_smooth.c](../src/live/audio_smooth.c)、[audio_output.c](../src/audio/audio_output.c) | **待写**（见 §4） |

最近的提交（`git log --oneline`）：`ff4d4ee` 看门狗 + 限幅 + 音频衔接，`2586999` 测试集，`a534b6d` 丢包不花屏，`566277e` 首次连接重试。本地领先 GitHub（`origin/main` 在 `40b65cf`），**未推送**。

## 3. 怎么编译、怎么测

```bash
# 编译（CLion 用 cmake-build-debug/；命令行可以建到别处）
cmake -S . -B cmake-build-debug && cmake --build cmake-build-debug -j8

# 全部故障注入场景（约 4 分钟，会弹窗口，要在桌面会话里跑）
python3 -I tests/live/run_live_tests.py
python3 -I tests/live/run_live_tests.py --only clean,decoder_hang     # 部分场景
python3 -I tests/live/run_live_tests.py --nohwaccel                   # 软解

# 音频咔哒 / 断音 A/B（输出 clicks 与 silent_gaps）
tests/live/audio_ab.sh clean_on --
tests/live/audio_ab.sh clean_off -- -noaudio_smooth
tests/live/audio_ab.sh stall_on --stall 8:1.5 --
```

测试专用环境变量（只在测试里用）：

| 变量 | 作用 |
|---|---|
| `SIMPLEFFPLAY_TEST_STALL_DECODER_AT=<秒>` | 启动这么多秒后让视频解码线程卡住，直到被关闭重建（验证看门狗） |
| `SIMPLEFFPLAY_TEST_AUDIO_DUMP=<文件>` | 把交给声卡的 PCM 写到文件（头 12 字节 `SFPA` + 采样率 + 声道数） |

**上一次完整测试集结果**（`ff4d4ee` 之前、音频改动之前）：11/11 通过。**`ff4d4ee` 之后还没有重跑完整测试集**——这是接手后的第一件事。

## 4. 剩余任务（按建议顺序）

### 4.1 查清剩余的音频断音（优先，问题是之前的改动引入的）

现象：低延迟模式下，**干净网络**里音频仍有静音断口。

| 版本 | 干净流 silent_gaps | 咔哒 |
|---|---|---|
| 关衔接（`-noaudio_smooth`），旧逻辑 | 约 7 段/秒 | 约 13 次/秒 |
| 开衔接，旧逻辑 | 约 7 段/秒 | 0 |
| 开衔接，`ff4d4ee`（只在开始出声时对齐截止时间） | **约 2.4 段/秒**（8.2 秒 20 段） | 0 |

根因（已修一部分）：[audio_output.c](../src/audio/audio_output.c) 的 `audio_frame_too_early()` 让音频等到视频的截止时间再出声。原来每一帧都检查，目标延迟 / base 的小波动让连续播放的音频被反复判成"早了"而插静音。现在只在 `audio_running == 0`（起播、断音后、接不上时）才检查。

还剩约 2.4 段/秒，没查。线索：
1. **先打点区分来源**：`VideoState` 里已有计数器 `audio_underruns`（正在出声时没数据）、`audio_waits`（等截止时间）、`audio_drops`（落后视频被丢），但没有输出。把它们加到 `-stats` 行，跑 `audio_ab.sh clean_on`，看是哪一个在涨。
2. `audio_decode_frame()` 里"接不上就当作重新开始"的判断用的是 `|af->pts − audio_clock| > 20ms`。MPEG-TS 里 AAC 帧时间戳若有抖动（一帧 1024 采样 ≈ 21.3ms），可能频繁误判 → `audio_running = 0` → 又去等截止时间。可以打日志确认，或把阈值改成按帧长比例。
3. `audio_frame_too_late()`（落后视频 80ms 就丢）也会造成断口：丢帧后衔接处理会淡入淡出，但如果频繁触发就是断音。看 `audio_drops`。
4. 本次 `clean_on` 的有效音频只有 8.2 秒（之前同样测试有 13.5 秒），说明起播变慢了，也值得看一眼是不是一直在等。

验收：`audio_ab.sh clean_on` 的 `silent_gaps` 接近 0，`stall_on` 只在卡顿处出现少量断口，`clicks` 保持 0；完整测试集全部通过。

### 4.2 第 6 步：运行指标（对应 WebRTC getStats）

目标：能回答"这一路昨晚卡了几次、断了几次、平均延迟多少"。

建议设计：
- 新模块 `src/live/live_metrics.{h,c}`，`LiveMetrics` 放进 `VideoState`。
- 选项：`-metrics_file <路径>`、`-metrics_interval <秒>`（默认 5）。每个间隔写一行 JSON，退出时（`stream_close`）写一份汇总。
- 指标：运行时长、显示帧数 / 当前帧率、码率（读线程累加 `pkt->size`）、丢帧（`frame_drops_early + frame_drops_late`）、冻结次数和**总时长**（在 `frame_integrity_ok()` 恢复处累加）、跳关键帧次数（`live.flushes`）、重连次数和断线总时长（`handle_disconnect()` 成功时累加）、看门狗触发（`wd_triggers`）、音频 `audio_underruns / audio_waits / audio_drops`、延迟 `lag` 的 p50/p95/p99（每显示一帧记一次，用 0~2000ms、5ms 一格的直方图，区间和全程各一份）。
- 计数器由不同线程写、主线程读，统计用途可以不加锁（64 位对齐读写在 arm64 上不会撕裂）。
- 在测试集里加校验：例如 `tcp_restart` 场景的汇总里重连次数 = 1，`blackout_300` 冻结次数 = 1。

### 4.3 技术文档（用户要求 4、5、6 都要有）

建议新建 `docs/live_reliability.md`，写：
- **看门狗**：判定条件（低延迟、有视频、未暂停、已出过画面、不在损坏冻结中、1 秒内有视频包、3 秒没新画面）、三级恢复动作和间隔、10 秒健康后清零、与 WebRTC `kMaxWaitForFrame` 的对应、测试场景 `decoder_hang` 的结果（13.7s 第 1 级、16.7s 第 2 级、恢复后 lag p95 64ms）、局限（真卡死在 libavcodec 内部的线程无法被 `stream_component_close` 等回来，会把读线程也卡住）。
- **抖动估计限幅**：为什么加（单个几秒的异常值让目标顶在 300ms 约 5 秒）、效果（`stall_1500` 尾部 lag p95 127→64ms，`decoder_hang` 323→64ms）。可以直接补进 [live_catchup_algorithm.md](live_catchup_algorithm.md) §3.2。
- **音频衔接**：镜像尾巴为什么不会在接缝处再跳、交叉淡化公式、只在开始出声时对齐截止时间的原因与实测数据（§4.1 的表）。
- **运行指标**：字段说明和示例。
- 同时更新 [live_catchup_algorithm.md](live_catchup_algorithm.md) 的参数表（`-audio_smooth`、`wd=`）和 [tests/live/README.md](../tests/live/README.md) 的场景表（`decoder_hang`）与结果。

### 4.4 收尾

重跑完整测试集（硬解 + `--nohwaccel`），全部通过后提交；用户同意后再 `git push origin main`。

### 4.5 暂缓（用户已决定）

- 第 3 步：12~24 小时挂机稳定性 + 内存泄漏（`leaks`）+ 句柄数趋势。
- 硬解帧直接上屏（NV12 纹理，省掉拷回内存 + 格式转换），多路同屏时再做。
- RTSP over UDP 丢包测试（需要 RTSP 服务端）、真实摄像头端到端延迟（需要用户的摄像头）。

## 5. 约定与环境里的坑

- **注释、文档用中文**；代码风格跟随 ffplay 原样。新加的状态字段在 `VideoState` 里注明"新增"。
- **提交**：git 身份已在本仓库配置（`TTTTOne`）；提交信息英文，结尾加 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。**用户说"提交"才提交，说"推送"才推送**。
- **用户的 shell 是 zsh**：`$VAR` 不会按空格拆分，带多个参数的命令放在 bash 脚本里跑（`tests/live/*.sh` 都是 bash）。
- 机器装了 rtk，`git`、`grep`、`cat` 等命令的输出会被压缩改写；需要原始输出时用 `rtk proxy <命令>`。
- 编辑器里 clang 报的 `file not found` / `undeclared identifier` 是 VS Code 没配头文件路径，用 CMake 编译正常，可以忽略。
- 本机 FFmpeg 6.0 在 `/Users/dev/Documents/AV/FFmpeg-6.0/install_macos`（有源码可查，在 `/Users/dev/Documents/AV/FFmpeg-6.0`），没有 libx264，测试推流用 `h264_videotoolbox`。
- 参考源码（只读）：ijkplayer `/Users/dev/Documents/AV/ijkplayer_64kb`、WebRTC `/Users/dev/Documents/AV/webRtc/src`、ExoPlayer `/Users/dev/Documents/AV/ExoPlayer-media`。

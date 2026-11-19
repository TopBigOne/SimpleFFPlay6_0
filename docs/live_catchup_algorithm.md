# 低延迟追帧算法（-lowlatency）

> 本文描述 SimpleFFPlay6.0 已实现的追帧算法：它怎么测延迟、怎么定目标、怎么丢帧和跳关键帧追回延迟，以及音频怎么跟上。
> 背景与选型见 [live_reference_ijkplayer_webrtc.md](live_reference_ijkplayer_webrtc.md)；ffplay 原有机制见 [live_streaming_tuning.md](live_streaming_tuning.md)。
> 断线重连是另一部分，见第 7 节。

## 1. 目标与思路

场景是家庭安防监控，端到端延迟要求 300ms 以内。播放器能用的预算大约 100ms（其余给摄像头编码、网络、解码、显示）。

原版 ffplay 的问题：
- 包队列固定攒约 1 秒才停读，稳态延迟就有 1 秒；
- 以音频为主时钟、1.0 倍速播放，卡顿造成的延迟**永远追不回来**；
- 唯一的调速机制（外部时钟 1.01 倍）只对 RTP/RTSP/UDP 生效，而且太慢。

本实现借鉴 **WebRTC** 的视频接收端（`modules/video_coding/timing`、`video/frame_decode_timing.cc`）：

1. **不靠缓冲多少来控延迟，而是给每一帧算一个"截止显示时刻"**；
2. 到点才显示，过点的帧丢掉，只看最新的——追帧是**丢帧追**，不是变速追；
3. 截止时刻 = 帧的时间戳 + 网络固定延迟 + 目标延迟，目标延迟按实测抖动自适应；
4. 积压太多（例如解码跟不上）时，等下一个关键帧直接跳过去。

为什么不像 ExoPlayer 那样变速追：安防场景"看到此刻"优先，丢帧跳过去比 ±3% 变速慢慢追（追回 1 秒要 30 多秒）合适得多；而且很多摄像头没有音频，变速的意义不大。

## 2. 总体流程

```
              读线程                      视频解码线程                    主线程（显示）
网络 ──► av_read_frame ──► videoq ──► 解码 ──► queue_picture ──► pictq ──► video_refresh ──► 屏幕
            │                                     │                         │
            │ ① 记录到达时刻                       │ ② 记录解码完成时刻        │ ③ 按截止时刻显示 / 丢帧
            ▼                                     ▼                         ▼
      更新 base（网络固定延迟）         更新抖动估计 → target            render_time = pts + base + target
            │
            │ ④ 关键帧到达且积压 > 2×latency_max → 清空队列，从这个关键帧开始
            ▼
         跳关键帧

音频：以视频为主时钟；⑤ 没到截止时刻先放静音，落后视频 80ms 以上直接丢
```

所有估计值集中在 [src/live/live_latency.c](../src/live/live_latency.c) 的 `LiveLatency` 里，三个线程通过一把互斥锁读写。

## 3. 测延迟：base 与抖动

### 3.1 base：网络的固定延迟（读线程）

`live_latency_on_packet()`（[live_latency.c:50](../src/live/live_latency.c#L50)），由 [read_thread.c:47](../src/demux/read_thread.c#L47) 的 `live_on_packet()` 对**参考流**（有视频用视频，否则用音频）的每个包调用：

```
o    = 到达时刻 − pts                  // 这个包"从时间戳到到达"花了多久（含摄像头时钟偏差）
base = 最近约 2 秒内 o 的最小值         // 最顺利的那个包
```

- `o` 的绝对值没有意义（摄像头时钟和本机时钟不同源），但**差值有意义**：`o − base` 就是这个包比最顺利的包晚到了多少。
- 不需要摄像头墙钟——这是相对 ExoPlayer 的关键优势，ExoPlayer 拿不到墙钟（RTMP/FLV/RTSP）时完全不做延迟控制。
- 窗口实现：两个 1 秒的半窗，`base = min(当前半窗最小值, 上一个半窗最小值)`；半窗滚动时旧最小值过期，所以摄像头和本机时钟的慢速漂移能被跟上。对应 WebRTC `packet_arrival_history`（`kPacketHistorySizeMs = 2000`）。
- `|o − base| > 10 秒`（`AV_NOSYNC_THRESHOLD`）视为时间戳跳变（换源、摄像头重启），整体重置。

### 3.2 抖动与目标延迟（视频解码线程）

`live_latency_on_frame()`（[live_latency.c:83](../src/live/live_latency.c#L83)），在 [video_decoder_thread.c:22](../src/video/video_decoder_thread.c#L22) 每解出一帧、**进帧队列之前**调用（进队列可能阻塞，要在阻塞前记时间）：

```
e      = max(解码完成时刻 − pts − base, 0)       // 这帧比"最顺利情况"晚了多少：网络抖动 + 排队 + 解码耗时
mean  += α·(e − mean)                            // α = 1/16，25fps 下时间常数约 0.6 秒
var    = (1 − α)·(var + α·(e − mean)²)
wanted = mean + 2.33·√var + 5ms                  // 2.33σ ≈ 99% 分位；5ms 渲染余量

if wanted ≥ target:  target = wanted                          // 变大：立刻生效
else:                target = max(wanted, target − 0.1·Δt)    // 变小：每秒最多降 100ms
target = clamp(target, latency_min, latency_max)              // 默认 [30ms, 300ms]
```

| 设计点 | 原因 | WebRTC 对应 |
|---|---|---|
| 用解码完成时刻而不是到达时刻算 `e` | 把解码耗时也算进去，不用单独估计解码 p95 | `VCMTiming`：target = jitter + decode_p95 + render_delay |
| 2.33σ | 约 99% 的帧能在截止前就绪 | `JitterEstimator` 输出 `2.33σ` |
| 涨快降慢 | 刚抖过就多留余量；平稳后慢慢压低，画面不会突然快进 | `kDelayMaxChangeMsPerS = 100` |

## 4. 追帧：按截止时刻显示和丢帧（主线程）

[video_refresh.c:80](../src/video/video_refresh.c#L80)，每次检查队头帧 `vp`：

```
render_time = vp.pts + base + target

if 现在 < render_time:
    等到 render_time 再来
elif 队列里还有下一帧 next，且同一 serial，且 现在 ≥ next.render_time:
    丢掉 vp（frame_drops_late++），继续看 next          // 当前帧已过时，看更新的
else:
    显示 vp
```

- 积压时（例如网络卡顿后数据涌入），队里一串帧的截止时刻都已经过了，一次刷新就会连续丢到"最新一个已到时刻的帧"，**画面直接跳到当前**，这就是追帧。
- 只要还有更新的帧就丢旧的；**最后一帧永远会显示**，不会因为都迟到而黑屏。对应 WebRTC `frame_decode_timing.cc`（迟到且不是最后一个可解码帧就跳过）。
- 估计还没就绪（刚起播、刚重连，`base` 还没有）时 `render_time` 为 NAN，退回原版 ffplay 的显示逻辑。
- 逐帧步进（`s` 键）时不走这段逻辑。

## 5. 兜底：积压太多就跳关键帧（读线程）

截止时刻丢帧只能丢**已经解码出来**的帧。如果积压在解码之前（解码跟不上、卡顿后涌入大量数据），帧要一个个解出来才能丢，追得慢。

[read_thread.c:47](../src/demux/read_thread.c#L47) `live_on_packet()`：每个**视频关键帧**到达时检查

```
积压 = 关键帧 pts − 当前显示画面的 pts
if 积压 > 2 × latency_max（默认 600ms）:
    清空音频 / 视频 / 字幕三个包队列，然后让这个关键帧入队
```

- `packet_queue_flush` 会让 serial + 1，解码器、帧队列里的旧数据、时钟都按 ffplay 原有的 serial 机制自动作废，不需要额外处理。
- 必须等关键帧：从非关键帧开始解码会花屏。所以跳转时机取决于摄像头的关键帧间隔，**建议摄像头设 1~2 秒一个关键帧**。
- 刚跳过、新画面还没显示（`vidclk.serial ≠ videoq.serial`）时不判断，避免连环清空。
- 对应 WebRTC：缓冲满时遇到关键帧就清空、需要关键帧时一直丢到关键帧（`ForceKeyFrameReleaseImmediately`）；也对应 ExoPlayer 掉出直播窗口后跳回直播点。

## 6. 音频怎么跟上

低延迟模式**以视频为主时钟**（[stream.c:385](../src/demux/stream.c#L385)），音频去追视频（[audio_output.c](../src/audio/audio_output.c)）：

| 情况 | 处理 | 位置 |
|---|---|---|
| 音频帧还没到截止时刻 | 不取走这帧，先输出约 11ms 静音 | `audio_frame_too_early()`，[audio_output.c:39](../src/audio/audio_output.c#L39) |
| 音频帧结束时刻比视频时钟早 80ms 以上 | 直接丢掉 | `audio_frame_too_late()`，[audio_output.c:20](../src/audio/audio_output.c#L20) |
| 其余小偏差 | ffplay 原有 `synchronize_audio` 的 ±10% 采样补偿 | [av_sync.c](../src/sync/av_sync.c) |

- **为什么要"没到时刻先静音"**：视频要等到截止时刻才显示，音频如果解出就播，会比画面超前约 `target`。加上这条后实测音画差从 +50ms 降到 ±10ms 以内。原则同 WebRTC 音画同步：谁快就给谁加延迟。
- 判断"到没到"时减去了约 2 个声卡缓冲的时长：这次写进 SDL 的数据要过这么久才真正响。
- 声卡缓冲从约 2048 采样（≈46ms）降到 512 采样（≈11ms）：每秒回调 120 次（[audio_output.c:246](../src/audio/audio_output.c#L246)）。

## 7. 其他配套改动

| 改动 | 作用 | 位置 |
|---|---|---|
| 读线程不停读（`infinite_buffer = 1`） | 数据不积在 socket 里，延迟统一由显示端控制 | [read_thread.c:469](../src/demux/read_thread.c#L469) |
| `fflags nobuffer` | 探测阶段少缓存 | [read_thread.c](../src/demux/read_thread.c) |
| `analyzeduration` 默认 1 秒（原 5 秒） | 起播和重连快约 4 秒 | [read_thread.c:320](../src/demux/read_thread.c#L320) |
| `AV_CODEC_FLAG_LOW_DELAY` | 解码器不为 B 帧攒帧，**摄像头必须关闭 B 帧** | [stream.c:220](../src/demux/stream.c#L220) |
| 断线自动重连 | 重连后 `live_latency_reset()` 重建 base，新连接的时间戳可能从头开始 | [read_thread.c](../src/demux/read_thread.c) `handle_disconnect()` |

用户自己给了 `-fflags`、`-analyzeduration` 时以用户的为准。

## 8. 参数与观测

```bash
./SimpleFFPlay6_0 -lowlatency -stats rtsp://摄像头地址
```

| 参数 | 默认 | 含义 |
|---|---|---|
| `-lowlatency` | 关 | 打开本算法 |
| `-latency_min` | 30 | 目标延迟下限（ms） |
| `-latency_max` | 300 | 目标延迟上限（ms）；积压超过它的 2 倍就跳关键帧 |

`-stats` 状态行末尾会多出：

```
lag=  42ms tgt= 30ms jump=1
```

| 字段 | 含义 |
|---|---|
| `lag` | 当前画面比"最顺利情况"晚了多少 = 现在 − 画面 pts − base |
| `tgt` | 当前目标延迟 |
| `jump` | 累计跳关键帧次数 |
| `fd`（原有字段） | 累计丢帧数 |

注意 `lag` **不是端到端延迟**：它不含摄像头编码和网络最低传输时间（这部分在 base 里，播放器测不到）。端到端延迟要用真实摄像头对着秒表拍屏幕来测。

## 9. 实测结果

环境：本机回环，FFmpeg 实时推 640×360 25fps H.264（无 B 帧，1 秒一个关键帧）+ AAC 的 UDP MPEG-TS 流，中间用转发脚本制造卡顿。

| 场景 | 结果 |
|---|---|
| 稳态 | `lag` 中位数 54~71ms，95% 分位 88ms，目标约 30~55ms，无误跳、无误丢 |
| 卡顿 1.5 秒后数据涌入 | 积压超过 600ms 触发一次跳关键帧，`lag` 回到约 320ms，随后 `tgt` 按每秒约 100ms 回落，约 3 秒后稳定在 40~55ms |
| 音画同步 | 加"没到时刻先静音"前 A-V ≈ +50ms，加后 ±10ms 以内 |
| UDP 中断 6 秒（触发重连） | 数据恢复后连续跳关键帧消化积压，最终 `lag` 42ms |

## 10. 已知限制

1. **只适用于直播**：算法依赖"包按实时节奏到达"。本地文件、点播会被一口气读完，base 估计失真；误用时会打印警告。
2. **刚起播时 `tgt` 会先顶到上限**：探测阶段读到的包在探测结束后一次性交出，看起来像抖动，约 3 秒后回落。
3. **摄像头必须关闭 B 帧**，否则 `LOW_DELAY` 会导致花屏。
4. **跳关键帧的及时性取决于关键帧间隔**：间隔 4 秒的摄像头，最坏要多积压 4 秒才能跳。
5. **只测过合成流**：还没在真实摄像头 RTSP 上验证。
6. 抖动估计只用了均值和方差的指数平均，没有实现 WebRTC 的 Kalman 滤波（帧大小 vs 延迟模型）；局域网 TCP/RTSP 场景下收益不大，若在弱网 UDP 场景表现不佳可以再升级。

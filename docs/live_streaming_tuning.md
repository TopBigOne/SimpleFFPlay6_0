# ffplay_common.h 调优常量与直播：延迟、卡顿、断流、追帧

> 对应代码：[src/common/ffplay_common.h](../src/common/ffplay_common.h)，常量值与 FFmpeg 6.0 原版 ffplay.c 一致。
> 文中 `文件:行号` 均指本工程拆分后的源码。

## 0. 先说结论

ffplay 的设计目标是**能正确地播放**各种文件和流，而不是做**低延迟直播播放器**。对直播来说：

| 问题 | ffplay 有没有处理 | 怎么处理的 |
|---|---|---|
| 起播慢 | 部分 | 靠命令行参数（`-probesize`、`-fflags nobuffer` 等），常量不管 |
| 稳态延迟 | 有上限、无下限控制 | 包队列"够了"就停读（≥25 包且 >1s），这 1s 就是固定延迟 |
| 网络抖动导致卡顿 | 只做被动应对 | 队列空了就等；视频落后就丢帧追**音频** |
| 卡顿恢复后的累积延迟（追帧） | **只在一种情况下有** | `-sync ext` + RTP/RTSP/UDP 源时，外部时钟最快 1.01 倍速追 |
| 断流重连 | **没有** | 读出错后读线程直接退出，画面停在最后一帧；HTTP 只能靠协议层的 `-reconnect` 选项 |
| 重连后时间戳跳变 | 有 | `max_frame_duration`、`AV_NOSYNC_THRESHOLD`、`serial` 三道保护 |

下面先看延迟藏在哪，再逐个讲常量，最后按场景串起来。

---

## 1. 延迟藏在哪：一条直播数据的路径

```
网络 ──► [内核 socket 缓冲] ──► read_thread ──► PacketQueue(音/视/字) ──► 解码线程 ──► FrameQueue ──► 显示 / 声卡
              ①                    av_read_frame      ②                     avcodec          ③          ④
```

| 环节 | 典型大小 | 受谁控制 |
|---|---|---|
| ① 协议/socket 缓冲 | 不定 | 读线程停读时数据会堆在这里（或服务器端） |
| ② PacketQueue | 约 **1 秒**（≥25 包且时长 >1s 才停读），上限 15MB | `MIN_FRAMES`、`MAX_QUEUE_SIZE`、`-infbuf` |
| ③ FrameQueue | 视频 3 帧、音频 9 帧、字幕 16 个 | `frame_queue.h` 里的 `VIDEO_PICTURE_QUEUE_SIZE` 等 |
| ④ 声卡缓冲 | 约 2048 采样 ≈ 46ms（44.1kHz） | `SDL_AUDIO_MIN_BUFFER_SIZE`、`SDL_AUDIO_MAX_CALLBACKS_PER_SEC` |

端到端延迟 ≈ 推流端/服务器缓冲 + ①②③④。其中 ② 是播放器侧最大的一块，**也是唯一一块 ffplay 会主动"控制大小"的**。

---

## 2. 常量逐个解释

### 2.1 队列容量：决定"缓多少再停读"

| 常量 | 值 | 用在哪 | 对直播的意义 |
|---|---|---|---|
| `MAX_QUEUE_SIZE` | 15 MB | [read_thread.c:285-286](../src/demux/read_thread.c#L285-L286) | 三个包队列字节数之和的上限，超过就停读 10ms 再看。主要防内存爆，直播码率下一般碰不到 |
| `MIN_FRAMES` | 25 | [read_thread.c:18-23](../src/demux/read_thread.c#L18-L23) `stream_has_enough_packets()` | 每个流都满足"包数 > 25 **且** 总时长 > 1s"才算够，读线程才停。**这就是直播约 1 秒固定延迟的来源** |
| `EXTERNAL_CLOCK_MIN_FRAMES` | 2 | [av_sync.c:44-46](../src/sync/av_sync.c#L44-L46) | 外部时钟模式下，任一队列 ≤2 包 → 外部时钟减速（快饿死了，放慢消耗） |
| `EXTERNAL_CLOCK_MAX_FRAMES` | 10 | [av_sync.c:47-49](../src/sync/av_sync.c#L47-L49) | 外部时钟模式下，所有队列都 >10 包 → 外部时钟加速（堆积了，加快消耗 = **追帧**） |

停读逻辑（[read_thread.c:284-293](../src/demux/read_thread.c#L284-L293)）：

```c
if (infinite_buffer < 1 &&
      (三个队列总字节 > MAX_QUEUE_SIZE
    || (音频够了 && 视频够了 && 字幕够了))) {
    等 10ms 或被解码线程唤醒;   // 队列取空时解码线程会 signal continue_read_thread
    continue;
}
```

`-infbuf`（`infinite_buffer = 1`）会让这段判断失效，读线程有多少读多少。对 RTP/RTSP/UDP 源它会**自动打开**（[read_thread.c:221-222](../src/demux/read_thread.c#L221-L222)），因为这类基于 UDP 的流如果不及时读，内核 socket 缓冲满了会直接丢包。

### 2.2 音频输出：声卡那一段的缓冲

| 常量 | 值 | 用在哪 | 意义 |
|---|---|---|---|
| `SDL_AUDIO_MIN_BUFFER_SIZE` | 512 采样 | [audio_output.c:202](../src/audio/audio_output.c#L202)、[:142](../src/audio/audio_output.c#L142) | SDL 声卡缓冲的下限；解码失败时也用它输出一段静音 |
| `SDL_AUDIO_MAX_CALLBACKS_PER_SEC` | 30 | [audio_output.c:202](../src/audio/audio_output.c#L202) | 每秒最多回调 30 次，据此算缓冲：`samples = 2 << log2(freq / 30)`。44.1kHz → 2048 采样 ≈ **46ms** |
| `SDL_VOLUME_STEP` | 0.75 dB | 音量键 | 与延迟无关 |

声卡缓冲大小还决定了两件事：
- **音频时钟的修正量**：[audio_output.c:167](../src/audio/audio_output.c#L167) 把音频时钟往回拨 `2 × 硬件缓冲 + 未写完的部分`（假设驱动有两段缓冲），这样时钟代表的是"此刻喇叭里正在响的声音"，而不是"刚交给声卡的声音"。
- **音频同步的死区**：`audio_diff_threshold = 硬件缓冲字节数 / 每秒字节数`（[stream.c:279](../src/demux/stream.c#L279)），即约 46ms 以内的音频偏差不纠正——因为声卡缓冲本身就有这么大的测量误差。

### 2.3 音画同步：卡顿时谁等谁、谁丢谁

ffplay 有三个时钟：音频时钟 `audclk`、视频时钟 `vidclk`、外部时钟 `extclk`。`-sync` 选一个当主时钟（默认音频），其余的去追它。

| 常量 | 值 | 用在哪 | 意义 |
|---|---|---|---|
| `AV_SYNC_THRESHOLD_MIN` | 0.04s | [av_sync.c:70](../src/sync/av_sync.c#L70) | 视频比主时钟差多少才纠正，下限 |
| `AV_SYNC_THRESHOLD_MAX` | 0.1s | [av_sync.c:70](../src/sync/av_sync.c#L70)、[video_refresh.c:85](../src/video/video_refresh.c#L85) | 纠正阈值的上限；另外，如果显示时刻已经落后理想时刻超过 0.1s，`frame_timer` 直接重置为当前时间（不再试图"补回"那段时间） |
| `AV_SYNC_FRAMEDUP_THRESHOLD` | 0.1s | [av_sync.c:74](../src/sync/av_sync.c#L74) | 视频超前时：帧时长 > 0.1s 就把超前量加到这帧的等待时间上；否则把这帧显示时长翻倍（相当于重复一帧） |
| `AV_NOSYNC_THRESHOLD` | 10s | [clock.c:51](../src/sync/clock.c#L51)、[av_sync.c:100](../src/sync/av_sync.c#L100)、[video_decoder_thread.c:60](../src/video/video_decoder_thread.c#L60) | 两个时钟差超过 10s 就认为**不是不同步，而是时间戳断了**：不再做渐进纠正，外部时钟直接跳到对方的值 |
| `SAMPLE_CORRECTION_PERCENT_MAX` | 10 | [av_sync.c:111-112](../src/sync/av_sync.c#L111-L112) | 音频不是主时钟时，靠增减采样数（重采样补偿）去追主时钟，每帧最多改 ±10% |
| `AUDIO_DIFF_AVG_NB` | 20 | [stream.c:275](../src/demux/stream.c#L275)、[av_sync.c:102](../src/sync/av_sync.c#L102) | 音频偏差先攒 20 次做指数加权平均，平均值超过死区才纠正，避免抖动 |

视频这一侧（`compute_target_delay()`，[av_sync.c:57-81](../src/sync/av_sync.c#L57-L81)）的规则，`diff = 视频时钟 − 主时钟`，`delay` = 本帧本该显示多久：

```
阈值 = clamp(delay, 0.04, 0.1)
diff ≤ −阈值             → 视频落后：delay = max(0, delay + diff)   // 少等甚至不等
diff ≥ 阈值 且 delay>0.1 → 视频超前：delay = delay + diff          // 多等
diff ≥ 阈值              → 视频超前：delay = 2 × delay              // 重复一帧
其他                     → 不纠正
```

视频落后太多时还有两处**丢帧**（`-framedrop`，默认在主时钟不是视频时开启）：
- **解码后早丢**（[video_decoder_thread.c:57-66](../src/video/video_decoder_thread.c#L57-L66)）：帧的 pts 已经早于主时钟，且包队列里还有包，直接扔掉不进 FrameQueue。
- **显示前晚丢**（[video_refresh.c:93-100](../src/video/video_refresh.c#L93-L100)）：轮到这帧时下一帧的显示时间都已经过了，跳过这帧。

### 2.4 外部时钟调速：ffplay 唯一的"追帧"机制

| 常量 | 值 | 意义 |
|---|---|---|
| `EXTERNAL_CLOCK_SPEED_MIN` | 0.900 | 最多减速到 0.9 倍 |
| `EXTERNAL_CLOCK_SPEED_MAX` | 1.010 | 最多加速到 1.01 倍 |
| `EXTERNAL_CLOCK_SPEED_STEP` | 0.001 | 每次调整的步长 |

`check_external_clock_speed()`（[av_sync.c:43-56](../src/sync/av_sync.c#L43-L56)）在每次 `video_refresh` 时调用（最多每 10ms 一次），**但只在两个条件同时满足时才调用**（[video_refresh.c:39](../src/video/video_refresh.c#L39)）：

1. 主时钟是外部时钟（`-sync ext`）；
2. `is->realtime` 为真——`is_realtime()`（[read_thread.c:25-39](../src/demux/read_thread.c#L25-L39)）**只认 rtp、rtsp、sdp 格式和 rtp:/udp: 地址**。

调速规则：

```
任一队列 ≤ 2 包         → 速度 −0.001（下限 0.9）   // 快断粮了，放慢
所有队列都 > 10 包      → 速度 +0.001（上限 1.01）  // 堆积了，加快
介于两者之间            → 速度向 1.0 回归 0.001
```

外部时钟一变快，视频（落后于外部时钟 → 丢帧）和音频（`synchronize_audio` 少输出采样）都会跟着加速消费，队列就被压回 2~10 包这个很小的区间。**这套机制的目标是让包队列保持在 2~10 包，从而维持低延迟。**

两个局限：
- **追得很慢**：1.01 倍速意味着每播 1 秒只追回 10ms。积压 1 秒延迟要约 100 秒才能消化掉。
- **RTMP / HTTP-FLV / HLS 都不算 realtime**，这些最常见的直播协议下这套机制根本不会启动。

### 2.5 显示与其他

| 常量 | 值 | 意义 |
|---|---|---|
| `REFRESH_RATE` | 0.01s | 主线程最长 10ms 检查一次是否该显示下一帧（[event_loop.c:52](../src/ui/event_loop.c#L52)），应小于 1/帧率 |
| `SAMPLE_ARRAY_SIZE` | 8×65536 | 波形/频谱显示用的采样缓存，与延迟无关 |
| `CURSOR_HIDE_DELAY` | 1s | 鼠标静止 1 秒隐藏 |
| `USE_ONEPASS_SUBTITLE_RENDER` | 1 | 字幕一次性整块渲染 |
| `FF_QUIT_EVENT` | 自定义 SDL 事件 | 读线程出错/播完时投递给主线程退出 |

---

## 3. 按场景串起来

### 3.1 起播：为什么打开直播流要等好几秒

起播延迟基本不受这些常量控制，主要来自：

1. `avformat_open_input` 探测格式（[read_thread.c:83](../src/demux/read_thread.c#L83)），受 `-probesize` 影响。
2. `avformat_find_stream_info` 读一段数据分析每个流的参数（[read_thread.c:104-108](../src/demux/read_thread.c#L104-L108)），受 `-analyzeduration`、`-probesize` 影响，直播流常常花 1~5 秒。
3. 包队列攒到 `MIN_FRAMES` 和 1 秒之前，读线程会一直读（这段时间其实已经在播了）。
4. 视频要等到第一个关键帧才能解出画面，取决于推流端的 GOP 长度。

### 3.2 稳态：延迟为什么"降不下来"

默认音频主时钟、非 realtime 源（例如 RTMP）的稳态：
- 读线程把每个包队列攒到"25 包且 1 秒"就停读，数据留在 socket/服务器端。
- 音频以 1.0 倍速匀速播放，视频跟着音频走。
- **播放器侧固定约 1 秒缓冲 + 声卡约 46ms + 帧队列几帧。**

播放器没有任何"缓冲太多就加速"的逻辑，所以一旦多出来的延迟进入队列，就会一直存在。

### 3.3 网络抖动：卡顿时发生了什么

以默认音频主时钟为例，网络卡住 3 秒：

1. 读线程 `av_read_frame` 阻塞或拿不到数据，包队列逐渐被解码线程取空。
2. 解码线程在 `packet_queue_get` 上阻塞等包；取空时会 signal `continue_read_thread` 叫读线程赶紧读。
3. **音频**：SDL 音频回调取不到帧。macOS/Linux 下 `frame_queue_peek_readable` 会阻塞等待（[audio_output.c:38](../src/audio/audio_output.c#L38)，Windows 下则等超过半个声卡缓冲就输出静音）。表现为声音中断。
4. **视频**：FrameQueue 空了，`video_refresh` 什么也不做，画面停在最后一帧。
5. 网络恢复，数据涌入：音频从中断处接着播（不是从"当前直播时刻"播），视频追着音频时钟走。

结果：**卡了 3 秒，延迟就永久增加了约 3 秒**（直到被下面的追帧机制消化，或者永远消化不掉）。

### 3.4 追帧：卡顿恢复后怎么回到"直播时刻"

| 配置 | 卡顿后会不会追 | 怎么追 |
|---|---|---|
| 默认（音频主时钟） | **不追** | 视频只会丢帧对齐音频，音频本身 1.0 倍速，累积的延迟一直在 |
| `-sync video` | 不追 | 同上，只是换成音频去对齐视频 |
| `-sync ext` + RTMP/HLS/HTTP-FLV | 不追 | `realtime` 为假，外部时钟不调速，固定 1.0 倍速 |
| `-sync ext` + RTSP/RTP/UDP | **慢慢追** | 队列 >10 包时外部时钟最快 1.01 倍速，视频丢帧、音频少输出采样去跟上 |

注意 `-framedrop` 的丢帧是在"视频追主时钟"，不是"追直播时刻"。主时钟本身落后了，丢再多视频帧也追不回来。

### 3.5 断流与重连

**ffplay 自身没有重连逻辑。**[read_thread.c:306-329](../src/demux/read_thread.c#L306-L329) 读包失败时：

- 返回 EOF：给每个队列塞一个空包，标记 `is->eof`，然后每 10ms 重试读一次（对文件来说就是"播完了"）。
- `ic->pb->error` 非零（网络错误）：
  - 带 `-autoexit`：读线程走到 `fail`，投递 `FF_QUIT_EVENT`，程序退出。
  - 不带 `-autoexit`：读线程 `break` 退出循环，且返回值为 0，**不会投递退出事件**——窗口还在，画面冻结在最后一帧，不会恢复。

想要断线重连，目前只能依赖协议层：
- **HTTP/HTTPS（HTTP-FLV、HLS 分片）**：FFmpeg 的 http 协议支持 `-reconnect 1 -reconnect_streamed 1 -reconnect_on_network_error 1 -reconnect_delay_max 5`，在协议内部重连，对 ffplay 透明。
- **RTMP / RTSP**：没有协议层自动重连，只能改播放器代码（见第 5 节）。
- `-rw_timeout <微秒>`：读写超时，让卡死的连接尽快报错，而不是永远阻塞在 `av_read_frame`。

### 3.6 重连 / 跳变后时间戳不连续怎么办

推流端重启、服务器切源后，pts 可能突然跳一大截或归零。ffplay 有三道保护：

1. **`max_frame_duration`**（[read_thread.c:130](../src/demux/read_thread.c#L130)）：格式带 `AVFMT_TS_DISCONT` 标志（如 MPEG-TS、HLS）时为 10s，否则 3600s。两帧间隔超过它就认为是时间戳跳变：`vp_duration()` 不用这个间隔（[video_refresh.c:13-22](../src/video/video_refresh.c#L13-L22)），`compute_target_delay()` 不做同步纠正（[av_sync.c:71](../src/sync/av_sync.c#L71)）。
2. **`AV_NOSYNC_THRESHOLD`（10s）**：时钟差超过 10s 不做渐进纠正，`sync_clock_to_slave()` 直接把外部时钟拨到音频/视频时钟上（[clock.c:47-52](../src/sync/clock.c#L47-L52)）。
3. **`serial` 机制**：每次 seek 或 flush 队列时 serial +1，所有带旧 serial 的包、帧、时钟都会被丢弃或视为无效（`get_clock` 返回 NAN）。这是 ffplay 里"整条管线一起切换到新数据"的标准手段，**自己实现重连时也应该复用它**。

---

## 4. 不改代码能做什么：直播常用参数

```bash
# 低延迟起播 + RTSP 走 TCP，外部时钟追帧（RTSP 属于 realtime，调速机制会生效）
./SimpleFFPlay6_0 -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 \
                  -rtsp_transport tcp -sync ext -framedrop rtsp://...

# HTTP-FLV / HLS：协议层自动重连 + 超时
./SimpleFFPlay6_0 -fflags nobuffer -flags low_delay \
                  -reconnect 1 -reconnect_streamed 1 -reconnect_on_network_error 1 -reconnect_delay_max 5 \
                  -rw_timeout 5000000 http://.../live.flv
```

| 参数 | 作用 | 代价 |
|---|---|---|
| `-fflags nobuffer` | 探测阶段少缓存包 | 极少数流探测不全 |
| `-flags low_delay` | 解码器不为 B 帧重排攒帧 | 有 B 帧的流可能出错 |
| `-probesize 32 -analyzeduration 0` | 起播几乎不探测 | 可能拿不到分辨率/采样率，打不开 |
| `-sync ext` | 用外部时钟当主时钟，配合 realtime 源才会调速 | 非 realtime 源没有收益 |
| `-framedrop` | 强制允许丢帧 | 画面跳帧 |
| `-infbuf` | 读线程不停读 | 不能减少延迟，只是不让数据堆在 socket 里 |

---

## 5. 想真正做好直播，需要改哪里

如果要做成"低延迟、卡顿后自动追回、断线自动重连"的直播播放器，以下是比较直接的改法：

1. **扩大 realtime 判定**：在 `is_realtime()` 里加入 `rtmp`、`flv`（url 以 `http` 开头）、`hls` 等，让外部时钟调速和 `-infbuf` 对它们也生效。
2. **按队列时长丢包追帧（最有效）**：在读线程入队前检查 `videoq.duration`，超过阈值（例如 2 秒）时丢弃包直到下一个关键帧，同时把音频队列裁到相同时间点。卡顿后一次性跳回直播时刻，而不是靠 1.01 倍速慢慢追。
3. **调高追帧倍速**：`EXTERNAL_CLOCK_SPEED_MAX` 改成 1.1~1.5，`SAMPLE_CORRECTION_PERCENT_MAX` 同步放大，否则音频跟不上外部时钟。超过约 1.1 倍时，靠增减采样会有明显的变调，应改用 `atempo` 滤镜做不变调变速。
4. **降低稳态缓冲**：直播时把 `stream_has_enough_packets` 里的 `MIN_FRAMES` 和"> 1.0 秒"改小（例如 0.3 秒），稳态延迟随之下降，代价是更容易卡顿。
5. **断线重连**：在读线程里，`ic->pb->error` 时不退出，而是关闭 `ic`、等待、重新 `avformat_open_input`；成功后对三个包队列做 `packet_queue_flush`（serial +1，旧数据自动作废），复用 3.6 的 serial 机制。

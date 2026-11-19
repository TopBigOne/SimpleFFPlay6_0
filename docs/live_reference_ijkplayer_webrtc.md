# 直播抗卡顿与追帧：ijkplayer、WebRTC、ExoPlayer 怎么做，SimpleFFPlay6.0 怎么借鉴

> 本文是 [live_streaming_tuning.md](live_streaming_tuning.md) 的续篇，那篇讲 ffplay 自己的机制。
> 源码位置：
> - ijkplayer（你的 fork）：`/Users/dev/Documents/AV/ijkplayer_64kb/ijkmedia/ijkplayer/`，下文简写 `ijk/`
> - WebRTC（约 M140）：`/Users/dev/Documents/AV/webRtc/src/`，下文简写 `rtc/`
> - ExoPlayer（Media3 1.11.1）：`/Users/dev/Documents/AV/ExoPlayer-media/libraries/exoplayer/src/main/java/androidx/media3/exoplayer/`，下文简写 `exo/`
>
> 文中常量和公式均对照本地代码核对过。

## 0. 一张表看四者差别

| 能力 | ffplay（本项目） | ijkplayer | WebRTC | ExoPlayer |
|---|---|---|---|---|
| 缓冲多少再播 | 固定：≥25 包且 >1s 停读 | 水位线：起播 100ms，卡一次升一级，最多 5s，不回落 | **自适应**：按抖动统计算目标延迟，抖动大就加、平稳后约 30s 慢慢减 | 起播 1s、卡顿后 2s，直播时取 min(目标延迟/2, 上述值) |
| 卡顿时 | 声音阻塞、时钟可能乱跑 | **缓冲状态机**：暂停三个时钟和声音，通知 App | 补偿音 + 渐弱，媒体时钟停住 | 进入 BUFFERING，时钟冻结；**目标延迟 +500ms** |
| 卡顿恢复 | 直接接着播 | 缓到水位线再播 | 先攒到目标的 50%，再衔接淡入 | 缓到门槛再播；4s 无进展判定卡死 |
| 追帧 | 仅 `-sync ext` + RTP/RTSP/UDP，最快 1.01x | **没有** | **阈值触发**不变调加速（约 1.1~1.3x）+ 积压过大整体清空 | **比例控制**调速：`1 + 0.1×误差秒`，限 0.97~1.03，每秒更新 |
| 视频落后 | 丢帧对齐主时钟 | 同 ffplay，限制连续丢帧数 | 按渲染截止时间丢未解码帧，必要时丢到关键帧 | 跟随音频调速 |
| 落后太多 | 无处理 | 无处理 | 200 包清空、旧 5s 的包丢弃 | 掉出直播窗口时报错，由 App 跳回直播默认位置 |
| 断线重连 | 没有，画面冻结 | `ijklivehook:` 前缀启用；重开流 + DISCONTINUITY 刷新 serial | 不适用 | 重试间隔 0/1/2/3/4/5s 封顶；连续 6 次**无进展**才放弃 |
| 测延迟靠什么 | 无 | 无 | 包到达时间 vs 时间戳 | **只靠墙钟**（HLS 的 PROGRAM-DATE-TIME、DASH 的时钟同步）；HTTP-FLV/RTMP **不做任何延迟控制** |

结论：
- **ijkplayer** 学结构：卡顿缓冲状态机、重连 + serial 刷新。
- **WebRTC** 学测量：用"到达时间 − 时间戳"测网络抖动，不依赖墙钟，RTMP/FLV 也能用。
- **ExoPlayer** 学控制：比例调速、卡顿后目标 +500ms 再限速回落、重试规则。规则少、参数直观，比 WebRTC 的直方图好实现。

---

## 1. ijkplayer：在 ffplay 上加了什么

### 1.1 缓冲状态机（`packet-buffering`，默认开）——最值得搬

**进入缓冲**由消费端触发：解码线程取包时先非阻塞取一次，取不到、而且这个队列是"指示队列"（有音频用 audioq，否则 videoq），就进入缓冲状态，再改为阻塞等待（`ijk/ff_ffplay.c:327-349`）。

**进入/退出缓冲时**（`ijk/ff_ffplay.c:4569-4596`、`:1175-1207`）：
- `暂停 = 用户暂停 || 正在缓冲`，统一冻结音频、视频、外部三个时钟，并暂停 SDL 音频。
- 向 App 发 `BUFFERING_START` / `BUFFERING_END`。
- 音频回调在暂停时填静音；视频不刷新，停在最后一帧。
- 恢复时 `frame_timer` 补上暂停的时长，所以恢复后**不会**快进丢帧。

**退出缓冲**由读线程每读一个包后按节流检查（首帧前每 50ms，之后每 500ms；`ijk/ff_ffplay.c:3602-3616`、`:4637-4755`）：
- 缓存时长 = min(音频队列时长, 视频队列时长)，与当前水位线比较。
- 达标，并且音视频队列都至少 2 个包，就结束缓冲。
- 每次达标后水位升级：100ms → 1s → 2s → 4s → 5s 封顶（`ijk/ff_ffplay_def.h:76-78`）。

**问题**：水位只在 seek 和首帧前重置，播放中卡几次后就固定在 5s，之后每次卡顿都要缓满 5 秒。卡顿时间全部变成永久延迟，没有追回机制。

### 1.2 停读条件被放宽

`stream_has_enough_packets` 删掉了 ffplay 的"时长 > 1s"条件，只剩 `nb_packets > min_frames`（`ijk/ff_ffplay.c:3025-3032`），而 `min_frames` 默认 50000（`ijk/ff_ffplay_def.h:90`）。**默认实际只靠 15MB 上限停读**——点播友好，直播会越积越多。

### 1.3 读错误不退出

ffplay 遇到网络错误就退出读循环；ijk 改为塞空包、记录错误、等 100ms 后继续尝试读（`ijk/ff_ffplay.c:3509-3562`）。读成功就继续播。

### 1.4 直播重连：`ijklivehook`

- 只有 URL 写成 `ijklivehook:<真实地址>` 才启用（`ijk/ijkavformat/ijklivehook.c`）。
- 内层 `av_read_frame` 出错：通知 App（可换 URL）→ 重新 `avformat_open_input` + `find_stream_info` → 下一个包打上 `AV_PKT_FLAG_DISCONTINUITY`（`ijklivehook.c:240-289`）。
- 读线程看到这个标志，给三个队列各塞一个 flush 包（`ijk/ff_ffplay.c:3563-3573`），serial 加 1，解码器和时钟全部重来。
- **缺**：没有退避等待、没有最大重试次数。

### 1.5 不建议照搬的

- `infbuf` 对实时流自动打开的逻辑**实际不生效**：先设成 -1，随后被选项默认值 0 覆盖（`ijk/ff_ffplay_def.h:760`、`ijk/ff_ffplay.c:3989`）。
- `compute_target_delay` 改成用 100s 的阈值判断，`max_frame_duration` 固定为 10s（`ijk/ff_ffplay.c:1255`、`:3181-3182`），对时间戳跳变的容忍反而比 ffplay 差。
- http 层的重连（`ijkurlhook`、`ijkiourlhook`）要求知道文件总大小，**对直播无效**。

---

## 2. WebRTC：自适应缓冲、追帧、卡顿恢复

### 2.1 音频 NetEq：目标延迟怎么算

**测量量**——每个包"比最顺利的包晚到了多少"（`rtc/modules/audio_coding/neteq/packet_arrival_history.cc:99-112`）：

```
delay = (到达时间 − 最早到达基准) − (RTP 时间戳 − 时间戳基准)     // 基准取 2 秒窗口内的最小值
```

**统计方法**——直方图 + 分位数（`underrun_optimizer.cc`、`histogram.cc`、`delay_manager.h:33-40`）：
- 100 个桶 × 20ms，覆盖 0~2s。
- 每 500ms 只把这段时间里**最大**的延迟写入一次（对峰值敏感）。
- 每次写入：所有桶 × 0.983，命中桶 + 0.017（指数遗忘，记忆约 30 秒）。
- `目标延迟 = (第 95 百分位所在桶 + 1) × 20ms`，初始 80ms（`delay_manager.cc:27`）。

**特性：涨得快、降得慢。** 一次大抖动立刻把目标抬高；网络平稳后约 30 秒才降回来。这正好对应"刚卡过就多缓一点，稳定了再压低延迟"。

### 2.2 音频 NetEq：每 10ms 的决策

`rtc/modules/audio_coding/neteq/decision_logic.cc`，每次输出 10ms 音频前决定怎么做：

| 条件 | 动作 |
|---|---|
| 缓冲里没包 | **Expand**：生成补偿音频，第 3 次、第 7 次后加速渐弱，约 2 秒后变成背景噪声 |
| 刚从 Expand 恢复，但缓冲 < 目标的 50% | 继续 Expand，**先攒到一半再播**，防止刚恢复又断（`kPostponeDecodingLevel=50`，`:33`） |
| 当前延迟 ≥ 目标 + 窗口内最大抖动 + 20ms，且距上次变速 ≥ 50ms | **Accelerate**：不变调加速 |
| 当前延迟 < 目标 | **PreemptiveExpand**：不变调减速 |
| 刚从 Expand 恢复，新数据到了 | **Merge**：新旧音频对齐后交叉淡化，音量从静音渐升 |
| 其他 | Normal |

缓冲水位先做平滑（时间常数约 0.4~1 秒，`buffer_level_filter.cc:44-60`），变速后要把增减的样本从水位里扣掉，防止重复计数。

### 2.3 追帧：不变调加速 + 硬兜底

- **Accelerate**（`accelerate.cc:49-99`、`time_stretch.cc`）：每 30ms 音频里找一个基音周期（2.5~15ms），删掉它并做交叉淡化。配合 50ms 冷却，平均约 **1.1~1.3 倍速**，听感上不变调。
- **硬兜底**：包缓冲到 200 个直接整体清空（`packet_buffer.cc:88-92`）；比播放点旧 5 秒以上的包丢弃。
- **视频**（`rtc/video/frame_decode_timing.cc:39-51`）：一帧的"最晚该开始解码时间"已经过了 5ms 以上，并且后面还有可解码帧，就跳过不解码；需要关键帧时一直丢，直到拿到完整关键帧。

### 2.4 视频目标延迟

`rtc/modules/video_coding/timing/timing.cc:264-267`：

```
目标延迟 = max(最小播放延迟, 网络抖动估计 + 解码耗时 p95 + 渲染延迟 10ms)
```

- 网络抖动用 Kalman 滤波估计（帧大小 vs 到达延迟），对 TCP 拉流的 RTMP/FLV 收益不大，可以简化。
- **当前延迟向目标逼近时限速：每秒最多变化 100ms**（`timing.h:60`），即 ±10% 速率，避免画面突然快进或停顿。
- 渲染时间偏离超过 10 秒，整体重置估计器（相当于"时间戳断了，重新开始"）。

### 2.5 音画同步

`rtc/video/stream_synchronization.cc`：每秒算一次音视频相对延迟差，4 点平滑；差值 < 30ms 不动；每次只修正一半、最多 80ms；**只给快的一方加延迟，不减**。

---

## 3. ExoPlayer：比例调速追帧、卡顿后调目标、重试规则

### 3.1 前提：只有带墙钟的直播才控制延迟

"直播延迟" = 当前墙钟 − 正在播放那一帧的墙钟时间（`exo/ExoPlayerImplInternal.java` 的 `getLiveOffsetUs`）。墙钟来自 HLS 的 `EXT-X-PROGRAM-DATE-TIME` 或 DASH 的 availabilityStartTime + 时钟同步。

拿不到墙钟就不调速。**HTTP-FLV / RTMP 这类 progressive 直播在 ExoPlayer 里完全没有延迟控制**——所以从它那里抄的是控制算法，测量要用 WebRTC 的方法（见 4.4）。

目标延迟的来源：HLS 依次取 `EXT-X-START`、`PART-HOLD-BACK`、`HOLD-BACK`、3 × 分片时长；DASH 取 ServiceDescription，默认 30s。

### 3.2 `DefaultLivePlaybackSpeedControl`：比例控制调速

默认常量（`exo/DefaultLivePlaybackSpeedControl.java:58-94`）：

| 常量 | 值 | 含义 |
|---|---|---|
| 速度范围 | 0.97 ~ 1.03 | 媒体没给时的回退值 |
| 更新间隔 | 1000ms | 每秒最多改一次速度 |
| 比例系数 | 0.1 | 误差每 1 秒，速度偏离 1.0 多 0.1 |
| 卡顿后目标增量 | 500ms | 每卡一次，目标延迟 +500ms |
| 平滑系数 | 0.999 | 约每 10ms 更新一次，时间常数约 10 秒 |
| 死区 | 20ms | 误差小于它保持 1.0 倍速 |

每次计算（约每 10ms 调一次，每秒真正改一次速度）：

```
// 1. 估计"网络允许的最低延迟"：变大立刻跟，变小慢慢降
minPossible = 当前延迟 − 已缓冲时长
smoothedMin = max(minPossible, 0.999·smoothedMin + 0.001·minPossible)
dev         = 0.999·dev + 0.001·|minPossible − smoothedMin|
safe        = smoothedMin + 3·dev

// 2. 调整当前目标：高于 safe 就降，每秒最多降约 60ms，不低于理想目标
// 3. 算速度
err   = 当前延迟 − 当前目标
speed = |err| < 20ms ? 1.0 : clamp(1 + 0.1·err秒, 0.97, 1.03)

// 4. 卡顿时
当前目标 += 500ms
```

和 WebRTC 的区别：

| | ExoPlayer | WebRTC NetEq |
|---|---|---|
| 方式 | 比例控制，误差越大越快 | 超过阈值才加速 |
| 速度 | ±3%，几乎听不出 | 约 1.1~1.3x，有轻微听感 |
| 追回 1 秒 | 约 33 秒 | 约 3~10 秒 |
| 目标延迟怎么变 | 卡一次 +500ms，然后限速下降 | 95 分位直方图，约 30s 记忆 |

两者估计的是同一个量：ExoPlayer 的 `smoothedMin + 3·dev` 约等于 NetEq 直方图的 95 分位。

### 3.3 `DefaultLoadControl`：起播与卡顿恢复门槛

`exo/DefaultLoadControl.java:812-828`：

```
门槛 = 刚卡顿过 ? 2000ms : 1000ms
直播时：门槛 = min(目标延迟 / 2, 门槛)
已缓冲 ≥ 门槛 → 开始播放
```

**起播门槛跟目标延迟联动**，不用单独维护一套水位升降。

### 3.4 卡顿与卡死

- 播放中没数据了就进入 BUFFERING：冻结时钟，并通知调速器把目标 +500ms（`exo/ExoPlayerImplInternal.java` 的 rebuffer 分支）。
- 缓冲中、不在加载、缓冲 < 500ms，持续 4 秒 → 判定卡死，抛出错误。

### 3.5 掉出直播窗口

HLS 要下载的分片已经被服务器移出播放列表时，抛 `BehindLiveWindowException`。播放器本身不恢复，官方 demo 的做法是 `seekToDefaultPosition()` + `prepare()`：跳到"窗口末尾 − 目标延迟"重新开始。

对 RTMP/FLV 来说，对应的是"延迟超过硬上限"——直接清空队列跳到下一个关键帧。

### 3.6 网络错误重试

`exo/upstream/DefaultLoadErrorHandlingPolicy.java`：
- **重试间隔** `min((第几次 − 1) × 1000, 5000)` ms，即 0、1、2、3、4、5、5… 秒（`:131`）。第一次立即重试。
- **progressive 直播最少重试 6 次**（普通 3 次，`:37-43`）；**只要这次连接读到过新数据，计数清零**——"连续 6 次没有任何进展"才放弃。
- **直接放弃不重试**：解析错误、文件不存在等。其他 IO 错误都重试。
- 403/404/410/416/500/503：有备用 CDN 就切过去，原地址排除 5 分钟。

### 3.7 变速时音频时钟怎么保持正确

`exo/audio/DefaultAudioSink.java`：
- 改速度时，在当前位置记一个检查点 `{速度, 媒体时间, 输出位置}`。
- 播放位置 = 检查点媒体时间 + 检查点以来实际播放时长 × 实际速度。
- 实际速度按变速器（Sonic）的"输入量 / 输出量"计算，而不是用设定值。

放到 ffplay：**音频时钟用 atempo 输入帧的 pts 计算**；SDL 缓冲里还没播的那部分换算回媒体时长时要**乘以当前速度**。

---

## 4. 移植到 SimpleFFPlay6.0：分阶段方案

按"收益 / 工作量"排序。每一阶段都能单独编译、单独验证。

### 阶段 1：断线重连（结构来自 ijkplayer，重试规则来自 ExoPlayer）

| 改哪里 | 做什么 |
|---|---|
| [read_thread.c](../src/demux/read_thread.c) 读错误分支 | 网络错误不退出：关闭 `ic` → 等待 `min((n−1)×1s, 5s)`（第一次立即重试）→ 重新打开；**连续 6 次没读到任何包**才投递退出事件，读到包就把 n 清零 |
| 错误分类 | `AVERROR_INVALIDDATA`、协议不支持、连续 404/403 → 直接放弃；超时、连接重置、EOF、EIO、5xx → 重试 |
| 同上，重开成功后 | 三个包队列 `packet_queue_flush`（serial + 1），旧包、旧帧、时钟自动作废——复用 ffplay 现有机制 |
| [stream.c](../src/demux/stream.c) | 新流的编码参数变了（分辨率、采样率）时，需要重开对应解码器 |
| 打开参数 | 设置 `rw_timeout` 和 `interrupt_callback`，防止 `av_read_frame` 永远阻塞 |

### 阶段 2：卡顿缓冲状态机（来自 ijkplayer，修掉水位不回落）

| 改哪里 | 做什么 |
|---|---|
| [video_state.h](../src/player/video_state.h) | 加 `buffering_on`、当前水位 `hwm_ms` |
| [decoder.c](../src/pipeline/decoder.c) 取包处 | 非阻塞取不到包 → 进入缓冲 |
| [player_control.c](../src/player/player_control.c) | `暂停 = 用户暂停 \|\| 缓冲中`，统一冻结时钟、暂停 SDL 音频 |
| [read_thread.c](../src/demux/read_thread.c) 主循环 | 每读一个包后节流检查：min(音频队列时长, 视频队列时长) ≥ 水位 → 结束缓冲 |
| 水位策略 | 采用 ExoPlayer 的联动方式：**水位 = clamp(目标延迟 / 2, 100ms, 1000ms)**；目标延迟先固定 400ms，阶段 4 再变成自适应 |
| 卡顿时 | 目标延迟 +500ms（封顶 3s），水位随之变大；之后由阶段 3/4 慢慢降回 |
| 卡死检测 | 缓冲中 4 秒没有任何新包 → 主动触发阶段 1 的重连 |

### 阶段 3：追帧（ExoPlayer 的比例控制 + WebRTC 的硬兜底，用 atempo 变速）

| 改哪里 | 做什么 |
|---|---|
| 延迟测量 | 先用简化版：当前延迟 ≈ `audioq.duration` + 音频帧队列中未播时长 + SDL 硬件缓冲（网络延迟当作常数） |
| [av_filters.c](../src/filter/av_filters.c) | 音频滤镜链插入 `atempo`（WSOLA 不变调变速，原理与 NetEq Accelerate、ExoPlayer 的 Sonic 相同） |
| 调速规则 | 每 500ms：`err = 当前延迟 − 目标`；`|err| < 50ms` 保持 1.0；否则 `speed = clamp(1 + 0.2·err秒, 0.95, 1.10)`；`err > 1.5s` 直接 1.25。速度变化 ≥ 0.01 才用 `avfilter_graph_send_command` 下发 |
| 系数为什么比 ExoPlayer 大 | ExoPlayer 的 ±3% 追回 1 秒要 33 秒，太慢；0.2 / 1.10 约 10 秒追回 1 秒，人声仍基本听不出。音乐类直播把上限收到 1.05 |
| [av_sync.c](../src/sync/av_sync.c) | 去掉 `synchronize_audio` 的 ±10% 重采样补偿，避免和 atempo 叠加 |
| [audio_output.c](../src/audio/audio_output.c) 音频时钟 | 按 atempo **输入帧**的 pts 计算；SDL 缓冲里未播部分换算成媒体时长时**乘以当前速度**（ExoPlayer 的检查点做法） |
| 视频 | 不用改：主时钟是音频，音频加速后视频自动跟上（现有丢帧逻辑生效） |
| 硬兜底 | 当前延迟 > max(3s, 3×目标)：清空队列，丢到下一个视频关键帧，重置时钟（对应 ExoPlayer 掉出直播窗口后的跳回、WebRTC 的整体清空） |

### 阶段 4：自适应目标延迟（WebRTC 的测量 + ExoPlayer 的平滑）

| 改哪里 | 做什么 |
|---|---|
| [read_thread.c](../src/demux/read_thread.c) 收到音频包时 | 计算 `d = (到达时间 − 基准到达) − (pts − 基准 pts)`，基准取 2 秒窗口最小值（WebRTC 的方法，不需要墙钟，RTMP/FLV 也能用） |
| 统计（推荐 ExoPlayer 方式） | 每 10ms：`smoothedMin = max(d, 0.999·smoothedMin + 0.001·d)`，`dev` 同样做 EWMA；`safe = smoothedMin + 3·dev`。若改为每 50ms 更新，系数用 0.995 |
| 目标调整 | 目标高于 safe 时每秒最多降约 60ms，不低于理想值；卡顿时 +500ms；夹在 [150ms, 3s] |
| 备选 | WebRTC 直方图：100 桶 × 20ms、遗忘 0.983、取 95 分位。效果接近，但实现更复杂 |
| 替换 | 用这个目标驱动阶段 2 的水位和阶段 3 的调速 |

这一步之后，网络好时延迟自动压到几百毫秒，网络差时自动多缓冲，卡顿后平滑回落。

### 推荐常量汇总

| 宏 | 值 | 出处 |
|---|---|---|
| `LIVE_TARGET_IDEAL_MS` | 400（HLS 用 3×分片时长） | ExoPlayer 思路 |
| `LIVE_TARGET_MIN_MS` / `MAX_MS` | 150 / 3000 | — |
| `REBUF_TARGET_INC_MS` | 500 | ExoPlayer |
| `START_HWM_MS` | clamp(target/2, 100, 1000) | ExoPlayer |
| `SMOOTH_FACTOR` | 0.999（每 10ms） | ExoPlayer |
| `SAFE_SIGMA` | 3 | ExoPlayer |
| `SPEED_KP` | 0.2 / 秒 | ExoPlayer 是 0.1，加快追帧 |
| `SPEED_MIN` / `SPEED_MAX` | 0.95 / 1.10（紧急 1.25） | 介于 ExoPlayer 与 NetEq 之间 |
| `SPEED_DEADBAND_MS` | 50 | ExoPlayer 是 20，ffplay 包粒度约 20ms，放宽 |
| `SPEED_UPDATE_MS` | 500 | — |
| `HARD_FLUSH_MS` | max(3000, 3×target) | WebRTC 思路 |
| `RETRY_DELAY_MS` | min((n−1)×1000, 5000) | ExoPlayer |
| `MAX_RETRY_NO_PROGRESS` | 6 | ExoPlayer |
| `STUCK_MS` | 4000 | ExoPlayer |

### 不建议移植的

- NetEq 的语音补偿音（PLC/LPC）、舒适噪声、DTMF、NACK、乱序优化：针对 UDP 丢包，RTMP/FLV/HLS 走 TCP 基本不丢包。
- 视频 Kalman 抖动估计、时间戳外推 Kalman：直播 TCP 场景收益小，用"首包锚定 + 长中断重新锚定"代替即可。
- WebRTC 的音画同步：ffplay 的音频主时钟模型在加了 atempo 后已经够用。

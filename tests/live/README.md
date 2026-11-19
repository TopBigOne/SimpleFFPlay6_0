# 直播可靠性测试集

对 `-lowlatency` 模式做故障注入测试：延迟、抖动、丢包不花屏、卡顿追帧、断线重连。

## 运行

```bash
# 先编译播放器（默认在 cmake-build-debug/ 或 build/ 下找 SimpleFFPlay6_0）
python3 -I tests/live/run_live_tests.py                       # 全部场景，约 3.5 分钟
python3 -I tests/live/run_live_tests.py --only clean,stall_6s # 只跑部分场景
python3 -I tests/live/run_live_tests.py --nohwaccel           # 播放器用软解
python3 -I tests/live/run_live_tests.py --player path/to/SimpleFFPlay6_0 --ffmpeg path/to/ffmpeg
```

- 播放器会弹出窗口，需要在桌面会话里运行。
- FFmpeg 默认取 `$FFMPEG_PREFIX/bin/ffmpeg`（与 CMakeLists.txt 相同的变量），找不到再用 PATH 里的；需要 libx264 或 h264_videotoolbox 编码器。
- 全部通过时退出码为 0。每个场景的带时间戳日志和 `report.json` 写到一个临时目录，路径在最后打印。

## 组成

| 文件 | 作用 |
|---|---|
| `run_live_tests.py` | 测试驱动：启动推流端、网络模拟器、播放器，解析日志和 `-stats`，按标准判定 |
| `netem_relay.py` | UDP 网络模拟器：随机丢包、连续丢包、抖动、乱序、卡顿后突发 |

测试源：FFmpeg `lavfi` 生成 640×360 25fps H.264（无 B 帧，1 秒一个关键帧）+ 48kHz AAC，封装 MPEG-TS，`-re` 实时推送。

## 场景与通过标准

| 场景 | 注入的故障 | 通过标准 |
|---|---|---|
| `clean` | 无 | 最后 6 秒 lag p95 ≤ 150ms；无冻结、无跳关键帧、无断线 |
| `jitter_80ms` | 每包随机延迟 0~80ms（不乱序） | lag p95 ≤ 300ms；无冻结（只有抖动不应判为损坏） |
| `blackout_300` | 第 8 秒起丢光 300ms | 检测到冻结；单次冻结 ≤ 1.5 秒（下一个关键帧恢复）；冻结占比 ≤ 15% |
| `loss_1pct` | 1% 随机丢包 | 检测到冻结且每次都恢复；单次 ≤ 8 秒；冻结占比 ≤ 60%；不触发 10 秒保护超时 |
| `reorder` | 0~20ms 抖动 + 1% 乱序 | 同上 |
| `stall_1500` | 第 8 秒起扣住 1.5 秒后突发放出 | 最后 5 秒 lag p95 ≤ 300ms，最终 lag ≤ 150ms |
| `stall_6s` | 第 6 秒起扣住 6 秒（超过 4 秒读超时） | 检测到断线并重连；追帧标准同上 |
| `tcp_restart` | TCP 推流端第 7 秒停止、第 11 秒恢复 | 检测到断线并重连；追帧标准同上 |
| `tcp_give_up` | TCP 推流端第 7 秒停止、不再恢复 | 重试 6 次后自行退出，退出时刻约 22.5 秒（20~28 秒） |
| `tcp_late` | TCP 推流端第 3 秒才上线 | 首次连接重试后连上并正常播放 |

`lag` 是 `-stats` 打印的"当前画面比最顺利情况晚了多少"，不含摄像头编码和网络最低传输时间。

## 当前结果（2026-11-19，macOS，h264_videotoolbox 推流）

硬解、软解下 10/10 通过。值得注意的数据：

| 场景 | 硬解 | 软解 |
|---|---|---|
| `clean` 稳态 lag p95 | 65ms | — |
| `jitter_80ms` 目标延迟 / lag p95 | 89ms / 135ms | — |
| `blackout_300` 冻结时长 | 763ms | 757ms |
| `loss_1pct` 冻结次数 / 冻结占比 | 5 次 / 48.5% | 14 次 / 49.6% |
| `stall_6s` 重连时刻 | 11.8 秒 | — |

**1% 随机丢包会让画面冻结约一半时间。** 原因是没有重传：一个关键帧约 40 个 UDP 包，被打坏的概率约三分之一，坏了就要再等 1 秒。这是"宁可冻结也不花屏"的代价，也说明 **UDP 直推不适合有丢包的网络——RTSP 请用 `-rtsp_transport tcp`**（TCP 重传相当于 WebRTC 的 NACK）。

软解检测到的冻结次数比硬解多：软解还能发现解码层面的错误，硬解只能依赖传输层的损坏标记，因此硬解下可能有少量坏帧未被拦截。

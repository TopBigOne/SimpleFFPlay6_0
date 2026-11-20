#!/usr/bin/env bash
# 音频咔哒 / 断音 A/B 测试：推一路 440Hz 正弦 + 测试画面（UDP MPEG-TS），可选经 netem_relay 注入故障，
# 播放器导出实际输出的 PCM，再用 audio_clicks.py 统计咔哒次数和静音断口。
#
# 用法：tests/live/audio_ab.sh <标签> [relay 参数...] -- [播放器额外参数...]
#   tests/live/audio_ab.sh clean_on --
#   tests/live/audio_ab.sh clean_off -- -noaudio_smooth
#   tests/live/audio_ab.sh stall_on --stall 8:1.5 --
# 环境变量：PLAYER（默认 cmake-build-debug/SimpleFFPlay6_0）、FFMPEG_PREFIX、OUT（PCM 输出目录，默认 /tmp）
# 输出一行 JSON：clicks = 咔哒次数，silent_gaps = ≥1ms 的全零静音段数（开播前的静音不算）
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PLAYER="${PLAYER:-$ROOT/cmake-build-debug/SimpleFFPlay6_0}"
FFMPEG="${FFMPEG_PREFIX:-/Users/dev/Documents/AV/FFmpeg-6.0/install_macos}/bin/ffmpeg"
OUT="${OUT:-/tmp}"

L=$1; shift
RA=(); while [ $# -gt 0 ] && [ "$1" != "--" ]; do RA+=("$1"); shift; done
[ $# -gt 0 ] && shift
PCM="$OUT/audio_ab_$L.pcm"

python3 -I "$HERE/netem_relay.py" 5000 5001 --duration 20 ${RA[@]+"${RA[@]}"} >/dev/null 2>&1 &
SIMPLEFFPLAY_TEST_AUDIO_DUMP="$PCM" perl -e 'alarm 18; exec @ARGV' "$PLAYER" -lowlatency "$@" -loglevel error -x 320 -y 180 \
  "udp://127.0.0.1:5001?fifo_size=1000000&overrun_nonfatal=1" >/dev/null 2>&1 &
PP=$!
perl -e 'alarm 18; exec @ARGV' "$FFMPEG" -v error -re -f lavfi -i testsrc2=size=640x360:rate=25 \
  -f lavfi -i sine=frequency=440:sample_rate=48000 -c:v h264_videotoolbox -bf 0 -g 25 -realtime 1 -b:v 1M \
  -c:a aac -f mpegts "udp://127.0.0.1:5000?pkt_size=1316" >/dev/null 2>&1
wait $PP 2>/dev/null; wait 2>/dev/null
echo "$L: $(python3 -I "$HERE/audio_clicks.py" "$PCM" --skip 3)"

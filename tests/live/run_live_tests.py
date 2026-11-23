#!/usr/bin/env python3
"""Fault-injection test suite for SimpleFFPlay6_0 -lowlatency (live latency, corruption freeze, reconnect).

Each scenario starts a synthetic live source (FFmpeg lavfi -> H.264 no B-frames, 1 keyframe/s, + AAC),
optionally routes it through netem_relay.py (UDP loss / jitter / reorder / stall), runs the player with
-lowlatency -stats, timestamps everything the player prints, and checks pass criteria.

Usage:
  python3 -I tests/live/run_live_tests.py [--player PATH] [--ffmpeg PATH] [--only NAME[,NAME...]] [--nohwaccel]
Exit code 0 = all selected scenarios passed. A JSON report is written next to the logs (path printed at the end).
The player opens a window, so run it in a desktop session.
"""
import argparse
import json
import os
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
RELAY = os.path.join(HERE, "netem_relay.py")
sys.path.insert(0, HERE)
import audio_clicks  # noqa: E402  (same directory)

STATS_RE = re.compile(r"lag=\s*(\d+)ms tgt=\s*(\d+)ms jump=(\d+) frz=(\d+)")
EVENTS = {
    "freeze":      re.compile(r"holding last good frame"),
    "recovered":   re.compile(r"recovered after (\d+)ms frozen, dropped (\d+) frames"),
    "no_keyframe": re.compile(r"no keyframe within"),
    "jump":        re.compile(r"jumping to keyframe"),
    "lost":        re.compile(r"live: (connection lost|cannot connect)"),
    "connected":   re.compile(r"live: (re)?connected"),
    "give_up":     re.compile(r"giving up after"),
    "watchdog":    re.compile(r"watchdog: .* recovery level (\d)"),
}


# --------------------------------------------------------------------------- helpers

def find_player():
    for d in ("cmake-build-debug", "cmake-build-release", "build"):
        p = os.path.join(ROOT, d, "SimpleFFPlay6_0")
        if os.access(p, os.X_OK):
            return p
    return None


def find_ffmpeg():
    prefix = os.environ.get("FFMPEG_PREFIX", "/Users/dev/Documents/AV/FFmpeg-6.0/install_macos")
    p = os.path.join(prefix, "bin", "ffmpeg")
    return p if os.access(p, os.X_OK) else shutil.which("ffmpeg")


def pick_encoder(ffmpeg):
    out = subprocess.run([ffmpeg, "-hide_banner", "-encoders"], capture_output=True, text=True).stdout
    if "libx264" in out:
        return ["-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency", "-bf", "0", "-g", "25"]
    if "h264_videotoolbox" in out:
        return ["-c:v", "h264_videotoolbox", "-realtime", "1", "-bf", "0", "-g", "25", "-b:v", "1M"]
    sys.exit("no H.264 encoder (libx264 / h264_videotoolbox) in " + ffmpeg)


class Sender:
    """Synthetic live source: 640x360 25fps H.264 (no B-frames, GOP 25) + 48kHz AAC in MPEG-TS."""

    def __init__(self, ffmpeg, enc, url):
        self.cmd = [ffmpeg, "-v", "error", "-re",
                    "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25",
                    "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
                    *enc, "-c:a", "aac", "-f", "mpegts", url]
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen(self.cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                self.proc.kill()


class Player:
    """Runs the player and timestamps every \\r- or \\n-terminated chunk it prints."""

    def __init__(self, player, url, extra, log_path, env=None):
        self.cmd = [player, "-lowlatency", "-stats", "-loglevel", "warning", "-x", "640", "-y", "360", *extra, url]
        self.env = {**os.environ, **(env or {})}
        self.log_path = log_path
        self.lines = []          # (t, text)
        self.t0 = None
        self.proc = None

    def start(self):
        self.t0 = time.time()
        self.proc = subprocess.Popen(self.cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=self.env)
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        buf = b""
        while True:
            ch = self.proc.stderr.read1(4096) if hasattr(self.proc.stderr, "read1") else self.proc.stderr.read(1)
            if not ch:
                break
            buf += ch
            parts = re.split(rb"[\r\n]", buf)
            buf = parts.pop()
            now = time.time() - self.t0
            for p in parts:
                s = p.decode("utf-8", "replace").strip()
                if s:
                    self.lines.append((now, s))

    def wait_or_kill(self, seconds):
        """Wait up to `seconds`; kill if still running. Returns (exited_by_itself, exit_time)."""
        try:
            self.proc.wait(seconds)
            return True, time.time() - self.t0
        except subprocess.TimeoutExpired:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            return False, time.time() - self.t0
        finally:
            time.sleep(0.2)
            with open(self.log_path, "w") as f:
                for t, s in self.lines:
                    f.write(f"{t:7.2f} {s}\n")


def analyze(lines):
    stats = []   # (t, lag, tgt, jump, frz)
    events = {k: [] for k in EVENTS}
    for t, s in lines:
        m = STATS_RE.search(s)
        if m:
            stats.append((t, *map(int, m.groups())))
        for k, rx in EVENTS.items():
            em = rx.search(s)
            if em:
                events[k].append((t, em.groups()))
    return stats, events


def lag_p95(stats, t_from, t_to):
    v = sorted(s[1] for s in stats if t_from <= s[0] <= t_to)
    if not v:
        return None
    return v[min(len(v) - 1, int(len(v) * 0.95))]


# --------------------------------------------------------------------------- scenarios
# Each scenario: transport, relay args, sender schedule, run time, and a check(stats, events, ctx) -> list of failures.

def check_clean(st, ev, ctx):
    f = []
    p95 = lag_p95(st, ctx["run"] - 6, ctx["run"])
    ctx["lag_p95_tail"] = p95
    if p95 is None or p95 > 150:
        f.append(f"steady lag p95 {p95}ms > 150ms")
    if ev["freeze"]:
        f.append(f"{len(ev['freeze'])} unexpected freezes")
    if ev["jump"]:
        f.append(f"{len(ev['jump'])} unexpected keyframe jumps")
    if ev["lost"]:
        f.append("unexpected disconnect")
    return f


def check_jitter(st, ev, ctx):
    f = []
    p95 = lag_p95(st, ctx["run"] - 6, ctx["run"])
    ctx["lag_p95_tail"] = p95
    ctx["target_tail"] = st[-1][2] if st else None
    if p95 is None or p95 > 300:
        f.append(f"lag p95 {p95}ms > 300ms under 80ms jitter")
    if ev["freeze"]:
        f.append(f"{len(ev['freeze'])} freezes (jitter without loss must not corrupt)")
    return f


def check_corruption(st, ev, ctx, max_recover_ms, max_frozen_pct):
    """Corrupted video must be detected (freeze instead of showing smeared frames) and always recover.
    Without retransmission a freeze lasts until the next intact keyframe, so with random loss several
    consecutive keyframes can be hit: we bound each freeze and the total frozen share instead of a tight limit."""
    f = []
    n_freeze, recs = len(ev["freeze"]), ev["recovered"]
    ctx["freezes"], ctx["recoveries"] = n_freeze, len(recs)
    ctx["recover_ms"] = [int(g[0]) for _, g in recs]
    ctx["frozen_pct"] = round(100.0 * sum(ctx["recover_ms"]) / (ctx["run"] * 1000), 1)
    if n_freeze == 0:
        f.append("loss injected but no freeze detected")
    if len(recs) < n_freeze - 1:              # the last freeze may still be in progress at the end
        f.append(f"only {len(recs)} recoveries for {n_freeze} freezes")
    slow = [ms for ms in ctx["recover_ms"] if ms > max_recover_ms]
    if slow:
        f.append(f"freezes longer than {max_recover_ms}ms: {slow}")
    if ctx["frozen_pct"] > max_frozen_pct:
        f.append(f"frozen {ctx['frozen_pct']}% of the time > {max_frozen_pct}%")
    if ev["no_keyframe"]:
        f.append("hit the 10s no-keyframe safety timeout")
    if not st or st[-1][0] < ctx["run"] - 2:
        f.append("playback stopped updating before the end")
    return f


def check_stall(st, ev, ctx):
    """After a disturbance: stay within the 300ms requirement over the last 5s and settle back to <=150ms."""
    f = []
    p95 = lag_p95(st, ctx["run"] - 5, ctx["run"])
    ctx["lag_p95_tail"] = p95
    if p95 is None or p95 > 300:
        f.append(f"lag p95 {p95}ms > 300ms in the last 5s (did not catch up after the stall)")
    if not st or st[-1][1] > 150:
        f.append(f"final lag {st[-1][1] if st else None}ms > 150ms")
    return f


def check_reconnect(st, ev, ctx):
    f = check_stall(st, ev, ctx)
    if not ev["lost"]:
        f.append("disconnect not detected")
    if not ev["connected"]:
        f.append("did not reconnect")
    else:
        ctx["reconnect_at"] = round(ev["connected"][-1][0], 2)
    return f


def check_watchdog(st, ev, ctx):
    """Decoder hangs at 8s (only killing the decoder thread helps): level 1 at ~11s must not be enough,
    level 2 at ~14s must bring the picture back, and latency must recover."""
    f = check_stall(st, ev, ctx)
    levels = [int(g[0]) for _, g in ev["watchdog"]]
    ctx["watchdog_levels"] = levels
    ctx["watchdog_at"] = [round(t, 1) for t, _ in ev["watchdog"]]
    if levels[:2] != [1, 2]:
        f.append(f"expected watchdog levels [1, 2], got {levels}")
    if 3 in levels:
        f.append("escalated to reconnect; recreating the decoder should have been enough")
    if ev["lost"]:
        f.append("unexpected disconnect")
    return f


def check_metrics(name, path, ctx):
    """-metrics_file must be written and agree with what the logs show."""
    f = []
    try:
        rows = [json.loads(l) for l in open(path) if l.strip()]
    except (OSError, ValueError) as e:
        return [f"metrics file unreadable: {e}"]
    if not rows:
        return ["metrics file empty"]
    last = rows[-1]
    ctx["metrics"] = {k: last.get(k) for k in ("type", "fps", "lag_ms", "freezes", "frozen_ms", "reconnects", "disconnected_ms", "watchdog")}
    if name == "tcp_restart" and last.get("reconnects") != 1:
        f.append(f"metrics reconnects={last.get('reconnects')}, expected 1")
    # 冻结从"解出第一个坏帧"算到下一个关键帧，断流期间本来就没画面不计入，所以时长在 0~1 个 GOP 之间
    if name == "blackout_300" and not (last.get("freezes") == 1 and 0 < last.get("frozen_ms", 0) <= 2000):
        f.append(f"metrics freezes={last.get('freezes')} frozen_ms={last.get('frozen_ms')}, expected 1 freeze of 0~2s")
    if name == "decoder_hang" and last.get("watchdog", 0) < 2:
        f.append(f"metrics watchdog={last.get('watchdog')}, expected >= 2")
    if name == "tcp_give_up" and last.get("type") != "summary":
        f.append("no summary line written on exit")
    if name == "clean" and not (20 <= last.get("fps", 0) <= 30):
        f.append(f"metrics fps={last.get('fps')}, expected ~25")
    return f


def check_give_up(st, ev, ctx):
    f = []
    ctx["exit_at"] = round(ctx["exit_time"], 2)
    if not ev["give_up"]:
        f.append("did not give up")
    if not ctx["exited"]:
        f.append("player did not exit by itself")
    elif not (20 <= ctx["exit_time"] <= 28):  # down at ~7s + 0+1+2+3+4+5s of retries
        f.append(f"exit at {ctx['exit_time']:.1f}s, expected ~22.5s")
    return f


def check_late_start(st, ev, ctx):
    f = []
    if not ev["connected"]:
        f.append("never connected")
    else:
        ctx["connected_at"] = round(ev["connected"][0][0], 2)
    if not st or st[-1][0] < ctx["run"] - 2:
        f.append("no playback")
    return f


SCENARIOS = [
    # name, transport, relay args, sender plan, run seconds, check
    ("clean",        "udp", [],                                 "always",     15, check_clean),
    ("jitter_80ms",  "udp", ["--jitter", "80"],                 "always",     18, check_jitter),
    # one burst: must freeze once and recover at the next keyframe (GOP = 1s)
    ("blackout_300", "udp", ["--blackout", "8:0.3"],            "always",     15, lambda s, e, c: check_corruption(s, e, c, 1500, 15)),
    # random loss / reorder over raw UDP (no retransmission): freezes are expected, must always recover
    ("loss_1pct",    "udp", ["--loss", "0.01", "--seed", "7"],  "always",     20, lambda s, e, c: check_corruption(s, e, c, 8000, 60)),
    ("reorder",      "udp", ["--jitter", "20", "--reorder", "0.01", "--seed", "3"], "always", 18,
                                                                                  lambda s, e, c: check_corruption(s, e, c, 8000, 60)),
    ("stall_1500",   "udp", ["--stall", "8:1.5"],               "always",     20, check_stall),
    ("stall_6s",     "udp", ["--stall", "6:6"],                 "always",     24, check_reconnect),
    ("decoder_hang", "udp", [],                                 "hang_decoder", 26, check_watchdog),
    ("tcp_restart",  "tcp", None,                               "restart",    22, check_reconnect),
    ("tcp_give_up",  "tcp", None,                               "die",        35, check_give_up),
    ("tcp_late",     "tcp", None,                               "late",       12, check_late_start),
]


def run_scenario(idx, sc, args, enc, outdir):
    name, transport, relay_args, plan, run, check = sc
    base = 21000 + idx * 4
    procs = []
    ctx = {"run": run}
    if transport == "udp":
        in_port, out_port = base, base + 1
        relay = subprocess.Popen([sys.executable, "-I", RELAY, str(in_port), str(out_port), "--duration", str(run + 4), *relay_args],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        procs.append(relay)
        sender = Sender(args.ffmpeg, enc, f"udp://127.0.0.1:{in_port}?pkt_size=1316")
        url = f"udp://127.0.0.1:{out_port}?fifo_size=1000000&overrun_nonfatal=1"
    else:
        relay = None
        sender = Sender(args.ffmpeg, enc, f"tcp://127.0.0.1:{base}?listen=1")
        url = f"tcp://127.0.0.1:{base}"

    env = {}
    if plan == "hang_decoder":
        env["SIMPLEFFPLAY_TEST_STALL_DECODER_AT"] = "8"
    audio_dump = None
    if transport == "udp":      # 检查音频：输出 PCM 里不能有咔哒声
        audio_dump = os.path.join(outdir, f"{name}.pcm")
        env["SIMPLEFFPLAY_TEST_AUDIO_DUMP"] = audio_dump
    metrics_path = os.path.join(outdir, f"{name}.metrics.jsonl")
    player = Player(args.player, url, args.player_extra + ["-metrics_file", metrics_path, "-metrics_interval", "1"],
                    os.path.join(outdir, f"{name}.log"), env)
    if plan in ("always", "restart", "die", "hang_decoder"):
        sender.start()
        time.sleep(0.5)
    player.start()
    t_start = time.time()

    def at(t):
        time.sleep(max(0, t - (time.time() - t_start)))

    if plan == "late":
        at(3); sender.start()
    elif plan in ("restart", "die"):
        at(7); sender.stop()
        if plan == "restart":
            at(11); sender.start()

    exited, exit_time = player.wait_or_kill(max(0, run - (time.time() - t_start)))
    ctx["exited"], ctx["exit_time"] = exited, exit_time
    sender.stop()
    if relay:
        relay.terminate()
        try:
            out, _ = relay.communicate(timeout=3)
            ctx["relay"] = json.loads(out.strip().splitlines()[-1]) if out.strip() else None
        except Exception:
            ctx["relay"] = None

    st, ev = analyze(player.lines)
    ctx["events"] = {k: len(v) for k, v in ev.items() if v}
    if st:
        ctx["final"] = {"lag": st[-1][1], "tgt": st[-1][2], "jump": st[-1][3], "frz": st[-1][4]}
    failures = check(st, ev, ctx)
    failures += check_metrics(name, metrics_path, ctx)
    if audio_dump and os.path.exists(audio_dump):
        try:
            a = audio_clicks.analyze(audio_dump, skip=3)
            ctx["audio"] = {"clicks": a["clicks"], "silent_gaps": a["silent_gaps"], "seconds": a["seconds"]}
            # 干净 / 抖动场景必须 0 次；故障恢复的瞬间（解码器重置、采样补偿）偶发 1 次是已知遗留问题
            max_clicks = 0 if name in ("clean", "jitter_80ms") else 1
            if a["clicks"] > max_clicks:
                failures.append(f"{a['clicks']} audio clicks at {a['click_times']} (allowed {max_clicks})")
            if name == "clean" and a["silent_gaps"] > 2:
                failures.append(f"{a['silent_gaps']} audio dropouts on a clean network")
        except ValueError as e:
            failures.append(f"audio dump unreadable: {e}")
    return failures, ctx


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--player", default=find_player())
    ap.add_argument("--ffmpeg", default=find_ffmpeg())
    ap.add_argument("--only", help="comma-separated scenario names")
    ap.add_argument("--nohwaccel", action="store_true", help="run the player with software decoding")
    ap.add_argument("--out", help="directory for logs and report (default: a new temp dir)")
    args = ap.parse_args()
    if not args.player or not os.access(args.player, os.X_OK):
        sys.exit("player binary not found; build it or pass --player")
    if not args.ffmpeg:
        sys.exit("ffmpeg not found; set FFMPEG_PREFIX or pass --ffmpeg")
    args.player_extra = ["-nohwaccel"] if args.nohwaccel else []
    enc = pick_encoder(args.ffmpeg)
    outdir = args.out or tempfile.mkdtemp(prefix="live_tests_")
    os.makedirs(outdir, exist_ok=True)

    selected = [s for s in SCENARIOS if not args.only or s[0] in args.only.split(",")]
    print(f"player: {args.player}\nffmpeg: {args.ffmpeg} ({enc[1]})\nlogs:   {outdir}\n")
    report, n_fail = [], 0
    for i, sc in enumerate(SCENARIOS):
        if sc not in selected:
            continue
        print(f"[{sc[0]:<13}] running {sc[4]}s ...", end="", flush=True)
        failures, ctx = run_scenario(i, sc, args, enc, outdir)
        ok = not failures
        n_fail += not ok
        print(" PASS" if ok else " FAIL")
        for msg in failures:
            print(f"    - {msg}")
        summary = {k: v for k, v in ctx.items() if k not in ("run", "exited", "exit_time")}
        print("    " + json.dumps(summary, ensure_ascii=False))
        report.append({"scenario": sc[0], "pass": ok, "failures": failures, **summary})
        time.sleep(1)

    with open(os.path.join(outdir, "report.json"), "w") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    print(f"\n{len(report) - n_fail}/{len(report)} passed. report: {os.path.join(outdir, 'report.json')}")
    sys.exit(1 if n_fail else 0)


if __name__ == "__main__":
    main()

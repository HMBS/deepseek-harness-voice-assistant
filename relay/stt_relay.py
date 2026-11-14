#!/usr/bin/env python3
"""Host-side speech relay for the ESP32 voice gadget.

The board records audio and uploads it here; this relay forwards it to
Deepgram for speech-to-text, and synthesises replies back through Deepgram
text-to-speech.

Why a relay instead of calling Deepgram from the board:

1. **Key handling.** The Deepgram key stays on the host, not on the device.
2. **Signal level.** This board's microphones are quiet (speech RMS ~120-250,
   noise floor ~60). The relay applies gain before sending, which is far more
   effective than trying to raise it on-device.
3. **Simplicity.** The firmware just POSTs JSON and gets text back.

Endpoints (both POST, JSON in / JSON out):

    /transcribe   {"audio_b64": "...", "sample_rate": 16000}
                  -> {"transcript": "..."}  or  {"error": "..."}
    /speak        {"text": "..."}
                  -> {"pcm_b64": "..."}    or  {"error": "..."}

Run it on the same LAN as the board:

    export DEEPGRAM_API_KEY=...
    ./stt_relay.py
"""

from __future__ import annotations

import argparse
import base64
import binascii
import json
import math
import glob
import os
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_PORT = 8799
HARNESS_TIMEOUT = 50
HARNESS_REPLY_MAX = 2000
DEEPGRAM = "https://api.deepgram.com/v1/listen"
MAX_AUDIO = 8 * 1024 * 1024


def log(*args, **kwargs) -> None:
    """Print with a wall-clock prefix, so stage latency is visible."""
    print(time.strftime("%H:%M:%S"), *args, **kwargs)


def find_dsh() -> str:
    """Locate the dsh binary: explicit env, newest npx cache, then PATH."""
    explicit = os.environ.get("DSH_BIN")
    if explicit and os.path.exists(explicit):
        return explicit
    candidates = glob.glob(os.path.expanduser("~/.npm/_npx/*/node_modules/.bin/dsh"))
    if candidates:
        candidates.sort(key=os.path.getmtime, reverse=True)
        return candidates[0]
    return "dsh"


def apply_gain(pcm: bytes, gain: float) -> bytes:
    """Scale s16le PCM by `gain`, clipping at the int16 limits."""
    if gain == 1.0 or len(pcm) < 2:
        return pcm
    usable = len(pcm) // 2 * 2
    samples = struct.unpack(f"<{usable // 2}h", pcm[:usable])
    out = []
    for v in samples:
        s = int(v * gain)
        if s > 32767:
            s = 32767
        elif s < -32768:
            s = -32768
        out.append(s)
    return struct.pack(f"<{len(out)}h", *out)


def split_wav(data: bytes) -> tuple[bytes, int, int, int]:
    """Return (pcm, rate, channels, bits), stripping a RIFF header if present.

    The board uploads a complete WAV file. Gain must be applied to the PCM
    payload only: scaling the header bytes turns a valid file into garbage
    and Deepgram rejects it as corrupt.
    """
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return data, 16000, 1, 16

    rate, channels, bits = 16000, 1, 16
    pos = 12
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = pos + 8
        if body + size > len(data):
            size = len(data) - body
        if cid == b"fmt " and size >= 16:
            _fmt, channels, rate, _br, _ba, bits = struct.unpack("<HHIIHH", data[body:body + 16])
        elif cid == b"data":
            return data[body:body + size], rate, channels, bits
        pos = body + size + (size & 1)
    return data, rate, channels, bits


def make_wav(pcm: bytes, rate: int, channels: int, bits: int) -> bytes:
    """Wrap raw PCM in a canonical RIFF/WAVE header."""
    block = max(1, channels * bits // 8)
    header = (
        b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE"
        + b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate,
                                rate * block, block, bits)
        + b"data" + struct.pack("<I", len(pcm))
    )
    return header + pcm


def pcm_peak(pcm: bytes) -> int:
    usable = len(pcm) // 2 * 2
    if not usable:
        return 0
    return max(abs(v) for v in struct.unpack(f"<{usable // 2}h", pcm[:usable]))


def pcm_rms(pcm: bytes) -> float:
    if len(pcm) < 2:
        return 0.0
    usable = len(pcm) // 2 * 2
    n = usable // 2
    if n == 0:
        return 0.0
    total = 0
    for v in struct.unpack(f"<{n}h", pcm[:usable]):
        total += v * v
    return math.sqrt(total / n)


def transcribe(pcm: bytes, rate: int, key: str, timeout: int = 30) -> str:
    """Send PCM to Deepgram and return the transcript (may be empty)."""
    url = f"{DEEPGRAM}?model=nova-2&language=en&smart_format=true"
    req = urllib.request.Request(
        url,
        data=pcm,
        method="POST",
        headers={
            "Authorization": f"Token {key}",
            "Content-Type": "audio/wav",
            "Content-Length": str(len(pcm)),
        },
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        payload = json.loads(resp.read())
    channels = payload.get("results", {}).get("channels") or []
    if not channels:
        return ""
    alts = channels[0].get("alternatives") or []
    return (alts[0].get("transcript") or "") if alts else ""


SPEAK_URL = ("https://api.deepgram.com/v1/speak"
             "?model=aura-2-thalia-en&encoding=linear16&container=none&sample_rate=16000")


def local_time_text() -> str:
    """Host local time, phrased for a spoken assistant."""
    import datetime
    now = datetime.datetime.now().astimezone()
    return now.strftime("%A, %B %-d, %Y at %-I:%M %p")


def speak(text: str, key: str, timeout: int = 30) -> bytes:
    """Synthesise `text` and return raw s16le PCM at 16 kHz mono."""
    body = json.dumps({"text": text}).encode()
    req = urllib.request.Request(
        SPEAK_URL,
        data=body,
        method="POST",
        headers={
            "Authorization": f"Token {key}",
            "Content-Type": "application/json",
        },
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def speak_open(text: str, key: str, timeout: int = 60):
    """Start synthesis and return the streaming response object."""
    body = json.dumps({"text": text}).encode()
    req = urllib.request.Request(
        SPEAK_URL,
        data=body,
        method="POST",
        headers={
            "Authorization": f"Token {key}",
            "Content-Type": "application/json",
        },
    )
    return urllib.request.urlopen(req, timeout=timeout)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    deepgram_key = ""
    deepseek_key = ""
    gain = 4.0
    dsh_bin = ""
    dsh_cwd = ""
    dsh_profile = "headless"
    dsh_timeout = HARNESS_TIMEOUT

    def log_message(self, *_args) -> None:
        pass

    def send_json(self, code: int, obj: dict) -> None:
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        self.wfile.write(body)

    def read_json(self) -> dict | None:
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            self.send_json(400, {"error": "bad Content-Length"})
            return None
        if length <= 0:
            self.send_json(400, {"error": "empty body"})
            return None
        if length > MAX_AUDIO * 2:
            self.send_json(413, {"error": "payload too large"})
            return None
        try:
            return json.loads(self.rfile.read(length))
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            self.send_json(400, {"error": f"bad JSON: {exc}"})
            return None

    def do_POST(self) -> None:  # noqa: N802
        if self.path.rstrip("/") == "/transcribe":
            self.handle_transcribe()
        elif self.path.rstrip("/") == "/speak":
            self.handle_speak()
        elif self.path.rstrip("/") == "/speak_stream":
            self.handle_speak_stream()
        elif self.path.rstrip("/") in ("/v1/chat/completions", "/chat/completions"):
            self.handle_chat()
        elif self.path.rstrip("/") == "/time":
            self.handle_time()
        else:
            self.send_json(404, {"error": "unknown endpoint"})

    def do_GET(self) -> None:  # noqa: N802
        if self.path.rstrip("/") == "/time":
            self.handle_time()
        else:
            self.send_json(404, {"error": "unknown endpoint"})

    def handle_time(self) -> None:
        """Return the host's local time, so the board can tell the time."""
        self.send_json(200, {
            "epoch": int(time.time()),
            "text": local_time_text(),
        })

    def handle_transcribe(self) -> None:
        msg = self.read_json()
        if msg is None:
            return
        try:
            pcm = base64.b64decode(msg["audio_b64"], validate=True)
        except (KeyError, binascii.Error, ValueError) as exc:
            self.send_json(400, {"error": f"bad audio_b64: {exc}"})
            return

        rate = int(msg.get("sample_rate") or 16000)
        pcm, wav_rate, channels, bits = split_wav(pcm)
        rate = wav_rate or rate
        before = pcm_rms(pcm)

        # The board's mics are quiet; bring the level up before sending, but
        # never amplify into clipping (loud audio, e.g. TTS round-trips).
        gain = self.gain
        peak = pcm_peak(pcm)
        if peak > 0:
            gain = min(gain, 30000.0 / peak)
        pcm = apply_gain(pcm, gain)
        after = pcm_rms(pcm)
        log(f"stt: {len(pcm)} bytes @ {rate} Hz {channels}ch {bits}bit, "
              f"rms {before:.0f} -> {after:.0f}, peak {peak}, gain {gain:.1f}x",
              flush=True)
        body = make_wav(pcm, rate, channels, bits)

        # Keep the last upload for diagnosis: it is the only way to tell a
        # silent microphone from a quiet one.
        try:
            with open("/tmp/stt_last.wav", "wb") as fh:
                fh.write(body)
        except OSError:
            pass

        try:
            transcript = transcribe(body, rate, self.deepgram_key)
        except urllib.error.HTTPError as exc:
            detail = exc.read()[:200].decode("utf-8", "replace")
            log(f"stt: deepgram HTTP {exc.code}: {detail}", flush=True)
            self.send_json(502, {"error": f"deepgram {exc.code}: {detail}"})
            return
        except Exception as exc:  # noqa: BLE001
            log(f"stt: error: {exc}", flush=True)
            self.send_json(502, {"error": str(exc)})
            return

        log(f"stt: transcript={transcript!r}", flush=True)
        # Piggyback the host clock: the board uses it for "what time is it?"
        # without a second round trip.
        self.send_json(200, {"transcript": transcript, "now": local_time_text()})

    def handle_chat(self) -> None:
        """OpenAI-compatible shim that answers through the local dsh harness.

        The gadget posts a normal chat-completions request; this handler turns
        it into one headless harness task and returns the answer in the shape
        the firmware expects.
        """
        msg = self.read_json()
        if msg is None:
            return

        messages = msg.get("messages") or []
        system = " ".join(
            m.get("content", "") for m in messages
            if m.get("role") == "system" and isinstance(m.get("content"), str)
        )
        user = ""
        for m in reversed(messages):
            if m.get("role") == "user" and isinstance(m.get("content"), str):
                user = m["content"]
                break

        if not user.strip():
            self.send_json(400, {"error": "no user message"})
            return

        task = (system + "\n\n" + user).strip()
        env = os.environ.copy()
        if self.deepseek_key:
            env["DEEPSEEK_API_KEY"] = self.deepseek_key

        started = time.time()
        log(f"chat: harness task ({len(task)} chars)", flush=True)
        try:
            proc = subprocess.run(
                [self.dsh_bin, "--profile", self.dsh_profile,
                 "--approval-policy", "never", "-"],
                input=task, capture_output=True, text=True,
                timeout=self.dsh_timeout,
                cwd=self.dsh_cwd or None, env=env,
            )
        except subprocess.TimeoutExpired:
            log(f"chat: harness timed out after {self.dsh_timeout}s", flush=True)
            self.send_json(504, {"error": "harness timed out"})
            return
        except OSError as exc:
            log(f"chat: cannot run harness: {exc}", flush=True)
            self.send_json(502, {"error": f"harness: {exc}"})
            return

        reply = (proc.stdout or "").strip()
        elapsed = time.time() - started

        if proc.returncode != 0 or not reply:
            tail = (proc.stderr or "").strip().splitlines()
            reason = tail[-1][:200] if tail else f"exit {proc.returncode}"
            log(f"chat: harness failed in {elapsed:.1f}s: {reason}", flush=True)
            self.send_json(502, {"error": reason})
            return

        reply = reply[:HARNESS_REPLY_MAX]
        log(f"chat: harness reply in {elapsed:.1f}s ({len(reply)} chars)", flush=True)
        self.send_json(200, {
            "choices": [{"message": {"role": "assistant", "content": reply}}],
        })

    def handle_speak_stream(self) -> None:
        """Stream raw s16le PCM back as it is synthesised.

        The board plays chunks as they arrive, so the first words are heard
        while the rest is still being generated instead of after the whole
        clip has been downloaded.
        """
        msg = self.read_json()
        if msg is None:
            return
        text = (msg.get("text") or "").strip()
        if not text:
            self.send_json(400, {"error": "no text"})
            return

        try:
            resp = speak_open(text, self.deepgram_key)
        except urllib.error.HTTPError as exc:
            detail = exc.read()[:200].decode("utf-8", "replace")
            log(f"tts: deepgram HTTP {exc.code}: {detail}", flush=True)
            self.send_json(502, {"error": f"deepgram {exc.code}: {detail}"})
            return
        except Exception as exc:  # noqa: BLE001
            log(f"tts: error: {exc}", flush=True)
            self.send_json(502, {"error": str(exc)})
            return

        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

        total = 0
        try:
            with resp:
                while True:
                    chunk = resp.read(4096)
                    if not chunk:
                        break
                    self.wfile.write(f"{len(chunk):X}\r\n".encode() + chunk + b"\r\n")
                    self.wfile.flush()
                    total += len(chunk)
            self.wfile.write(b"0\r\n\r\n")
        except (BrokenPipeError, ConnectionResetError):
            log("tts-stream: client went away", flush=True)
        log(f"tts-stream: {total} bytes for {text[:60]!r}", flush=True)

    def handle_speak(self) -> None:
        msg = self.read_json()
        if msg is None:
            return
        text = (msg.get("text") or "").strip()
        if not text:
            self.send_json(400, {"error": "no text"})
            return

        try:
            pcm = speak(text, self.deepgram_key)
        except urllib.error.HTTPError as exc:
            detail = exc.read()[:200].decode("utf-8", "replace")
            log(f"tts: deepgram HTTP {exc.code}: {detail}", flush=True)
            self.send_json(502, {"error": f"deepgram {exc.code}: {detail}"})
            return
        except Exception as exc:  # noqa: BLE001
            log(f"tts: error: {exc}", flush=True)
            self.send_json(502, {"error": str(exc)})
            return

        log(f"tts: {len(pcm)} bytes for {text[:60]!r}", flush=True)
        self.send_json(200, {"pcm_b64": base64.b64encode(pcm).decode()})


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--gain", type=float, default=4.0,
                    help="linear gain for incoming audio (default 4.0)")
    ap.add_argument("--key", default=os.environ.get("DEEPGRAM_API_KEY", ""))
    ap.add_argument("--deepseek-key", default=os.environ.get("DEEPSEEK_API_KEY", ""))
    ap.add_argument("--dsh-bin", default=find_dsh())
    ap.add_argument("--dsh-cwd", default="")
    ap.add_argument("--dsh-profile", default="headless",
                    help="dsh profile used for voice answers (default: headless)")
    ap.add_argument("--dsh-timeout", type=int, default=HARNESS_TIMEOUT)
    args = ap.parse_args()

    if not args.key:
        print("error: set DEEPGRAM_API_KEY", file=sys.stderr)
        return 2

    Handler.deepgram_key = args.key
    Handler.deepseek_key = args.deepseek_key
    Handler.gain = args.gain
    Handler.dsh_bin = args.dsh_bin
    Handler.dsh_cwd = args.dsh_cwd
    Handler.dsh_profile = args.dsh_profile
    Handler.dsh_timeout = args.dsh_timeout
    log(f"harness: {args.dsh_bin} profile={args.dsh_profile} "
        f"(timeout {args.dsh_timeout}s)")

    server = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    log(f"voice relay on :{args.port} (gain {args.gain}x)", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("\nstopping", flush=True)
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
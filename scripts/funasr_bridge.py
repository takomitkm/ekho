#!/usr/bin/env python3
# ekho-linux: glue between an ASR side (FunASR or anything else) and ekho-server.
#
# ekho-server is already the local TTS service this fork ships
# (src/ekho_server_main.cpp): a plain HTTP daemon. A client POSTs UTF-8 text,
# content-length framed or chunked, and gets audio back:
#
#     POST /tts          chunked audio/L16 out (raw PCM as synthesis progresses)
#     POST /tts?wait=1   one whole WAV once the synthesis is done
#     GET  /health       status;  GET /voices
#
# Its default output is 48000 Hz / 16-bit stereo unless --samplerate/--channels
# said otherwise at startup. By design nothing server-side plays audio: the
# ALSA backend exists but is not wired (README, Known issues), so the last hop
# -- sound out of an actual speaker -- belongs to whoever owns the speaker.
# This script is that hop, in two shapes:
#
#   speak (default): POST text, play or save the reply where this script runs.
#
#       python3 funasr_bridge.py "识别出的文本"
#       echo "文本" | python3 funasr_bridge.py -
#       python3 funasr_bridge.py --server http://192.168.1.113:2046 "文本"
#       python3 funasr_bridge.py --save reply.wav "文本"     # no speaker needed
#       python3 funasr_bridge.py --stream "文本" \
#           --play "aplay -q -f S16_LE -r 48000 -c 2 -t raw" # PCM as it renders
#
#   listen: a small HTTP listener (default 127.0.0.1:2047) for the "throw the
#   text at this side" shape -- the FunASR side only sends, and whichever box
#   runs this listener fetches from ekho-server and sounds through its own
#   speaker (or files, with --save-dir):
#
#       python3 funasr_bridge.py listen [--bind 0.0.0.0 --port 2047]
#       [ FunASR side: ] curl -X POST --data-binary "文本" http://this-side:2047/
#
# FunASR hook (on the ASR side, stdlib import only):
#
#     import sys
#     sys.path.insert(0, "<this repo>/scripts")
#     from funasr_bridge import speak
#     from funasr import AutoModel
#     asr = AutoModel(model="paraformer-zh", vad_model="fsmn-vad")
#     ... after each recognition: speak(res[0]["text"])  # blocks until spoken
#
# The server itself is started as scripts/server-test.sh does (sections 0-8
# asserted the protocol):
#
#     export EKHO_DATA_PATH=<this repo>/ekho-data
#     out/ekho-server --port 2046 --out-dir /tmp
#
# --play takes a shell command; a `{}` in it is replaced by the quoted WAV
# path, otherwise the quoted path is the last argument. Without --play the
# script tries `aplay`, then `paplay`, and with neither it either --saves or
# fails loudly.
#
# The file uses an underscore on purpose: unlike every other script here it is
# imported as a module from the ASR side (`from funasr_bridge import speak`),
# and Python module names cannot carry a hyphen.

import argparse
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_SERVER = "http://127.0.0.1:2046"
DEFAULT_RATE = 48000
DEFAULT_CHANNELS = 2
PCM_CHUNK = 32768
DEFAULT_TIMEOUT = 120.0


def fetch_wait(server, text, timeout=DEFAULT_TIMEOUT):
    """POST text, return the complete WAV bytes (?wait=1)."""
    url = server.rstrip("/") + "/tts?wait=1"
    req = urllib.request.Request(
        url,
        data=text.encode("utf-8"),
        method="POST",
        headers={"Content-Type": "text/plain; charset=utf-8"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def fetch_stream(server, text, sink, timeout=DEFAULT_TIMEOUT):
    """POST text; hand every chunk of raw PCM (audio/L16) to sink()."""
    url = server.rstrip("/") + "/tts"
    req = urllib.request.Request(
        url,
        data=text.encode("utf-8"),
        method="POST",
        headers={"Content-Type": "text/plain; charset=utf-8"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        while True:
            chunk = resp.read(PCM_CHUNK)
            if not chunk:
                break
            sink(chunk)


def resolve_player(name):
    """--play wins; else first of aplay/paplay found; else None."""
    if name:
        return name
    for cand in ("aplay", "paplay"):
        if shutil.which(cand):
            return cand
    return None


def player_command(player, wav_path):
    """Shell string that plays one WAV file."""
    if "{}" in player:
        return player.format(shlex.quote(wav_path))
    return "%s %s" % (player, shlex.quote(wav_path))


def play_file(player, wav_path):
    """Play one WAV; raise RuntimeError on a non-zero player exit."""
    cmd = player_command(player, wav_path)
    proc = subprocess.run(cmd, shell=True)
    if proc.returncode != 0:
        raise RuntimeError("player exited %d: %s" % (proc.returncode, cmd))


def speak(text, server=DEFAULT_SERVER, player=None, save=None, stream=False,
          rate=DEFAULT_RATE, channels=DEFAULT_CHANNELS,
          timeout=DEFAULT_TIMEOUT):
    """Speak one text through ekho-server. Returns the WAV path if saved.

    Player resolution: explicit --play > autodetect (aplay, paplay) >
    save-only when `save` is given. In stream mode the player command must
    read raw PCM from stdin (no file path involved).
    """
    if not text or not text.strip():
        raise ValueError("empty text")
    t0 = time.time()
    if stream:
        cmd_player = player if player is not None else resolve_player(None)
        if cmd_player is None:
            raise RuntimeError("no aplay/paplay found; use --save (wait mode) "
                               "or --play with a raw-PCM command")
        proc = subprocess.Popen(cmd_player, shell=True, stdin=subprocess.PIPE)
        total = [0]

        def sink(chunk):
            proc.stdin.write(chunk)
            total[0] += len(chunk)

        try:
            fetch_stream(server, text, sink, timeout)
        finally:
            try:
                proc.stdin.close()
            except BrokenPipeError:
                pass
            proc.wait()
        print("streamed %d B PCM (%.2fs)" % (total[0], time.time() - t0))
        if proc.returncode != 0:
            raise RuntimeError("player exited %d: %s"
                               % (proc.returncode, cmd_player))
        return None
    # plain mode: the whole WAV comes back at once
    cmd_player = player if player is not None else resolve_player(None)
    if cmd_player is None and save is None:
        raise RuntimeError("no aplay/paplay found on this box; "
                           "pass --save PATH or --play CMD")
    wav = fetch_wait(server, text, timeout)
    path = None
    if save:
        with open(save, "wb") as fh:
            fh.write(wav)
        path = save
        print("saved %d B -> %s (%.2fs)" % (len(wav), save, time.time() - t0))
    if cmd_player is None:
        return path
    if path is None:
        fh = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
        fh.write(wav)
        fh.close()
        path = fh.name
    try:
        play_file(cmd_player, path)
    finally:
        if save is None:
            try:
                os.unlink(path)
            except OSError:
                pass
    if save is None:
        print("played %d B (%.2fs)" % (len(wav), time.time() - t0))
    return path


# ---------------------------------------------------------------------------
# listen mode: FunASR's side only POSTs; this side synthesises-and-sounds.
# ---------------------------------------------------------------------------

class ListenHandler(BaseHTTPRequestHandler):
    # filled in by listen_main
    upstream = DEFAULT_SERVER
    player = None
    save_dir = None
    timeout = DEFAULT_TIMEOUT

    def _reply(self, code, msg):
        body = (msg + "\n").encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        # convenience: /health and /voices pass through to ekho-server
        if self.path.split("?")[0] in ("/health", "/voices"):
            try:
                req = urllib.request.Request(self.upstream + self.path)
                with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                    data = resp.read()
            except urllib.error.HTTPError as e:
                self._reply(502, "ekho-server %s" % e.code)
                return
            except Exception as e:
                self._reply(502, "upstream: %s" % e)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        else:
            self._reply(404, "POST text to this endpoint")

    def do_POST(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(n) if n > 0 else b""
            text = raw.decode("utf-8", "replace").strip()
            if not text:
                self._reply(400, "empty body")
                return
            wav = fetch_wait(self.upstream, text, self.timeout)
        except urllib.error.HTTPError as e:
            self._reply(502, "ekho-server %s" % e.code)
            return
        except Exception as e:
            self._reply(502, "upstream: %s" % e)
            return
        ts = time.strftime("%Y%m%d-%H%M%S")
        if self.save_dir:
            path = "%s/%s.wav" % (self.save_dir.rstrip("/"), ts)
            with open(path, "wb") as fh:
                fh.write(wav)
            self._reply(200, "saved %d B -> %s" % (len(wav), path))
            return
        if self.player is None:
            self._reply(500, "no aplay/paplay and no --save-dir on this box")
            return
        fh = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
        fh.write(wav)
        fh.close()
        try:
            play_file(self.player, fh.name)
        except RuntimeError as e:
            self._reply(500, str(e))
            return
        finally:
            try:
                os.unlink(fh.name)
            except OSError:
                pass
        self._reply(200, "played %d B (%s)" % (len(wav), ts))

    def log_message(self, fmt, *args):  # keep stderr informative but short
        BaseHTTPRequestHandler.log_message(self, fmt, *args)


def listen_main(flags):
    ap = argparse.ArgumentParser(
        prog="funasr_bridge.py listen",
        description="HTTP listener: text in, this box speaks it out.")
    ap.add_argument("--bind", default="127.0.0.1",
                    help="bind address (use 0.0.0.0 for other hosts)")
    ap.add_argument("--port", type=int, default=2047)
    ap.add_argument("--server", default=DEFAULT_SERVER,
                    help="ekho-server base URL")
    ap.add_argument("--play", default=None, help="player command; {} = wav path")
    ap.add_argument("--save-dir", default=None,
                    help="write WAVs here instead of playing")
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    args = ap.parse_args(flags)
    ListenHandler.upstream = args.server
    ListenHandler.player = resolve_player(args.play)
    ListenHandler.save_dir = args.save_dir
    ListenHandler.timeout = args.timeout
    if ListenHandler.player is None and args.save_dir is None:
        print("no aplay/paplay found and no --save-dir; refusing to run "
              "(every POST would 500)", file=sys.stderr)
        return 2
    httpd = ThreadingHTTPServer((args.bind, args.port), ListenHandler)
    print("funasr_bridge listening on http://%s:%d (ekho-server %s, %s)"
          % (args.bind, args.port, args.server,
             "save-dir" if args.save_dir else "player"))
    sys.stdout.flush()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
    return 0


def speak_main(flags):
    ap = argparse.ArgumentParser(
        prog="funasr_bridge.py",
        description="POST text to ekho-server; speak/save the reply.")
    ap.add_argument("text", nargs="*", help="text to speak; '-' reads stdin")
    ap.add_argument("--server", default=DEFAULT_SERVER)
    ap.add_argument("--save", default=None)
    ap.add_argument("--play", default=None)
    ap.add_argument("--stream", action="store_true",
                    help="POST /tts and pipe chunked PCM into --play stdin")
    ap.add_argument("--rate", type=int, default=DEFAULT_RATE)
    ap.add_argument("--channels", type=int, default=DEFAULT_CHANNELS)
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    args = ap.parse_args(flags)
    text = " ".join(args.text).strip()
    if text == "-":
        text = sys.stdin.read().strip()
    if not text:
        print("no text given", file=sys.stderr)
        return 2
    try:
        speak(text, server=args.server, player=args.play, save=args.save,
              stream=args.stream, rate=args.rate, channels=args.channels,
              timeout=args.timeout)
    except ValueError as e:
        print(str(e), file=sys.stderr)
        return 2
    except RuntimeError as e:
        print(str(e), file=sys.stderr)
        return 2
    except urllib.error.HTTPError as e:
        print("ekho-server %s %s" % (e.code, e.reason), file=sys.stderr)
        return 3
    except urllib.error.URLError as e:
        print("cannot reach ekho-server: %s" % e.reason, file=sys.stderr)
        return 3
    return 0


def main():
    flags = sys.argv[1:]
    if flags and flags[0] == "listen":
        return listen_main(flags[1:])
    return speak_main(flags)


if __name__ == "__main__":
    sys.exit(main())

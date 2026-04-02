#!/usr/bin/env python3
"""
tcp_monitor.py - TCP server for RP2350-ETH MSC->TCP streaming bridge.

Listens on TCP port, displays parsed weight records and debug frames
from the board. Use this when the real host device writes the log file
(no simulated writer).

Usage:
  tcp_monitor.py [port]   (default port: 2000)

Ctrl+C to stop.
All events written to tcp_monitor.log with timestamps.
"""

import socket
import threading
import time
import os
import sys
from datetime import datetime

sys.stdout.reconfigure(line_buffering=True)

# -- Config ------------------------------------------------------------
TCP_HOST = "0.0.0.0"
TCP_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 2000

if getattr(sys, "frozen", False):
    _BASE = os.path.dirname(sys.executable)
else:
    _BASE = os.path.dirname(os.path.abspath(__file__))
LOG_FILE = os.path.join(_BASE, "tcp_monitor.log")

# -- State -------------------------------------------------------------
stop_event = threading.Event()
stats_lock = threading.Lock()
stats = {"received": 0, "dbg": 0, "errors": 0}

# -- Log ---------------------------------------------------------------
_log_lock = threading.Lock()
_log_fh   = open(LOG_FILE, "a", encoding="utf-8", buffering=1)

def log(msg: str):
    line = f"[{datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]}] {msg}"
    print(line, flush=True)
    with _log_lock:
        _log_fh.write(line + "\n")

# -- TCP Server --------------------------------------------------------
def server_thread():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind((TCP_HOST, TCP_PORT))
    except OSError as e:
        log(f"SERVER  ERROR bind :{TCP_PORT}: {e}")
        stop_event.set()
        return

    srv.listen(1)
    srv.settimeout(1.0)
    log(f"SERVER  listening on :{TCP_PORT}")

    while not stop_event.is_set():
        try:
            conn, addr = srv.accept()
        except socket.timeout:
            continue
        except OSError:
            break

        log(f"SERVER  client connected {addr}")
        conn.settimeout(1.0)
        buf = b""

        while not stop_event.is_set():
            try:
                chunk = conn.recv(1024)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break

            buf += chunk
            i = 0
            while i < len(buf):
                b = buf[i]

                if b == 0x01:
                    # Debug frame [0x01][len][text]
                    if i + 2 > len(buf):
                        break
                    flen = buf[i + 1]
                    if i + 2 + flen > len(buf):
                        break
                    text = buf[i+2 : i+2+flen].decode("ascii", errors="replace")
                    log(f"DBG     {text}")
                    with stats_lock:
                        stats["dbg"] += 1
                    i += 2 + flen

                else:
                    # Raw text line (weight record)
                    nl = buf.find(b"\n", i)
                    if nl == -1:
                        break
                    line = buf[i:nl].rstrip(b"\r").decode("ascii", errors="replace")
                    if line:
                        with stats_lock:
                            stats["received"] += 1
                        log(f"RECV    {line}")
                    i = nl + 1

            buf = buf[i:]

        log(f"SERVER  client disconnected {addr}")

    srv.close()
    log("SERVER  stopped")


def stats_thread():
    while not stop_event.is_set():
        time.sleep(10)
        if stop_event.is_set():
            break
        with stats_lock:
            r, d, e = stats["received"], stats["dbg"], stats["errors"]
        log(f"STATS   received={r}  dbg_frames={d}  errors={e}")


# -- Main --------------------------------------------------------------
if __name__ == "__main__":
    log("=" * 56)
    log("  RP2350-ETH TCP Monitor (server only)")
    log(f"  port={TCP_PORT}  log -> {LOG_FILE}")
    log("=" * 56)

    for t in [
        threading.Thread(target=server_thread, daemon=True, name="server"),
        threading.Thread(target=stats_thread,  daemon=True, name="stats"),
    ]:
        t.start()

    try:
        while True:
            time.sleep(0.2)
    except KeyboardInterrupt:
        log("[MAIN] Stopping...")
        stop_event.set()
        time.sleep(1)
    log("[MAIN] Done.")

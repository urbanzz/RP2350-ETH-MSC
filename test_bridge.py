#!/usr/bin/env python3
"""
test_bridge.py - otladka RP2350-ETH MSC->TCP streaming bridge.

  1. WRITER  - pishet simulirovannyy log vesov na USB MSC disk (output_data/wc/)
  2. SERVER  - TCP server na portu 2000, prinimaet i otobrazhaet rasparsennyye zapisi

Ispolzovaniye:
  python test_bridge.py [bukva_diska]  (po umolchaniyu D)

Ctrl+C - ostanovka.
Vse sobytiya pishut v test_bridge.log s vremennymi metkami.
"""

import socket
import threading
import time
import os
import sys
import random
from datetime import datetime

sys.stdout.reconfigure(line_buffering=True)

# -- Nastroyki ---------------------------------------------------------
DRIVE           = (sys.argv[1].rstrip(":\\/") if len(sys.argv) > 1 else "D") + ":\\"
TCP_HOST        = "0.0.0.0"
TCP_PORT        = 2000

if getattr(sys, "frozen", False):
    _BASE = os.path.dirname(sys.executable)
else:
    _BASE = os.path.dirname(os.path.abspath(__file__))
LOG_FILE        = os.path.join(_BASE, "test_bridge.log")

WRITE_INTERVAL  = 2.0   # sekund mezhdu append-ami
LINES_PER_WRITE = 2     # strok za odin append
START_ENUM      = 891318
WEIGHT_BASE     = 79.0
WEIGHT_JITTER   = 3.0

# -- Sostoyanie --------------------------------------------------------
stop_event = threading.Event()
stats_lock = threading.Lock()
stats = {"written": 0, "received": 0, "errors": 0}

# -- Log ---------------------------------------------------------------
_log_lock = threading.Lock()
_log_fh   = open(LOG_FILE, "a", encoding="utf-8", buffering=1)

def log(msg: str):
    line = f"[{datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]}] {msg}"
    print(line, flush=True)
    with _log_lock:
        _log_fh.write(line + "\n")

# -- Helpers -----------------------------------------------------------
def rnd_weight():
    return round(WEIGHT_BASE + random.uniform(-WEIGHT_JITTER, WEIGHT_JITTER), 2)

# -- Writer thread -----------------------------------------------------
def writer_thread():
    folder = os.path.join(DRIVE, "output_data", "wc")
    fname  = f"Every_{datetime.now().strftime('%Y%m%d_%H%M%S')}_.txt"
    fpath  = os.path.join(folder, fname)

    log(f"WRITER  drive={DRIVE}  interval={WRITE_INTERVAL}s  lines/write={LINES_PER_WRITE}")
    log(f"WRITER  file={fpath}")

    try:
        os.makedirs(folder, exist_ok=True)
    except OSError as e:
        log(f"WRITER  ERROR mkdir: {e}")
        stop_event.set()
        return

    try:
        now = datetime.now()
        with open(fpath, "w", newline="\r\n") as f:
            f.write("START PACK WEIGHT LOG   \r\n")
            f.write(f"     {now.day}-{now.strftime('%b-%Y  %H:%M')}  \r\n")
            f.flush()
            os.fsync(f.fileno())
        log("WRITER  header written, starting log...")
    except OSError as e:
        log(f"WRITER  ERROR write header: {e}")
        stop_event.set()
        return

    enum_val = START_ENUM
    while not stop_event.is_set():
        time.sleep(WRITE_INTERVAL)
        if stop_event.is_set():
            break

        rows = []
        for _ in range(LINES_PER_WRITE):
            w1 = rnd_weight()
            w2 = rnd_weight()
            rows.append((enum_val, w1, w2))
            enum_val += 2

        try:
            with open(fpath, "a", newline="\r\n") as f:
                for (e, w1, w2) in rows:
                    f.write(f"{e:6d}   {w1:5.2f}    {w2:5.2f} \r\n")
                f.flush()
                os.fsync(f.fileno())
            with stats_lock:
                stats["written"] += len(rows)
        except OSError as e:
            log(f"WRITER  ERROR append: {e}")
            with stats_lock:
                stats["errors"] += 1
            continue

        if len(rows) == 1:
            e, w1, w2 = rows[0]
            log(f"WRITE   {e}:{w1:.2f}  {e+1}:{w2:.2f}")
        else:
            log(f"WRITE   {len(rows)} lines  enum {rows[0][0]}..{rows[-1][0]+1}")

    log("WRITER  stopped")


# -- TCP Server thread -------------------------------------------------
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
                    if i + 2 > len(buf): break
                    flen = buf[i + 1]
                    if i + 2 + flen > len(buf): break
                    text = buf[i+2 : i+2+flen].decode("ascii", errors="replace")
                    log(f"DBG     {text}")
                    i += 2 + flen

                elif b == 0x02:
                    # File frame (old mode)
                    if i + 5 > len(buf): break
                    sz = int.from_bytes(buf[i+1:i+5], "little")
                    if i + 5 + sz > len(buf): break
                    log(f"FILE    {sz} bytes (old mode)")
                    i += 5 + sz

                else:
                    # Raw text line
                    nl = buf.find(b"\n", i)
                    if nl == -1: break
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


# -- Stats thread ------------------------------------------------------
def stats_thread():
    while not stop_event.is_set():
        time.sleep(10)
        if stop_event.is_set(): break
        with stats_lock:
            w, r, e = stats["written"], stats["received"], stats["errors"]
        log(f"STATS   written={w}  received={r}  loss={w-r}  errors={e}")


# -- Main --------------------------------------------------------------
if __name__ == "__main__":
    log("=" * 56)
    log("  RP2350-ETH MSC->TCP Bridge Test")
    log(f"  log -> {LOG_FILE}")
    log("=" * 56)

    for t in [
        threading.Thread(target=server_thread, daemon=True, name="server"),
        threading.Thread(target=writer_thread, daemon=True, name="writer"),
        threading.Thread(target=stats_thread,  daemon=True, name="stats"),
    ]:
        t.start()

    try:
        while True:
            time.sleep(0.2)
    except KeyboardInterrupt:
        log("[MAIN] Stopping...")
        stop_event.set()
        time.sleep(2)
    log("[MAIN] Done.")

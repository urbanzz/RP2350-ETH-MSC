"""
RP2350-ETH debug/data server
============================
Слушает TCP :8888. Принимает framed-протокол от прошивки:

  [0x01][1-byte len][text]           — debug-сообщение
  [0x02][4-byte LE uint32][bytes]    — файл (raw TXT)

Файлы сохраняются в папку ./received/  с именем YYYYMMDD_HHMMSS_NNN.txt
Debug-сообщения выводятся в консоль.

Запуск:  python server.py [--port 8888] [--out ./received]
"""

import socket
import struct
import os
import sys
import time
import argparse
import threading

LISTEN_HOST = ""   # все интерфейсы
LISTEN_PORT = 8888
OUT_DIR     = "./received"

# ── ANSI цвета ────────────────────────────────────────────────────
def _c(code, s): return f"\033[{code}m{s}\033[0m" if sys.stdout.isatty() else s
RED    = lambda s: _c("31", s)
GREEN  = lambda s: _c("32", s)
YELLOW = lambda s: _c("33", s)
CYAN   = lambda s: _c("36", s)
GRAY   = lambda s: _c("90", s)

def ts():
    return time.strftime("%H:%M:%S")

# ── Надёжное чтение N байт ────────────────────────────────────────
def recvall(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf.extend(chunk)
    return bytes(buf)

# ── Обработка одного клиента ──────────────────────────────────────
def handle_client(conn, addr, out_dir, counters):
    peer = f"{addr[0]}:{addr[1]}"
    print(f"{ts()} {GREEN('CONNECT')} {peer}")
    file_index = 0
    try:
        while True:
            # Читаем тип фрейма (1 байт)
            frame_type = recvall(conn, 1)[0]

            if frame_type == 0x01:
                # ── Debug message ─────────────────────────────────
                length = recvall(conn, 1)[0]
                text   = recvall(conn, length).decode("utf-8", errors="replace")
                print(f"{ts()} {CYAN('[DBG]')} {GRAY(peer)} {text}")

            elif frame_type == 0x02:
                # ── File data ─────────────────────────────────────
                size_bytes = recvall(conn, 4)
                file_size  = struct.unpack("<I", size_bytes)[0]
                print(f"{ts()} {YELLOW('FILE')}  {GRAY(peer)}  "
                      f"receiving {file_size} bytes ...", end="", flush=True)

                t0   = time.monotonic()
                data = recvall(conn, file_size)
                dt   = time.monotonic() - t0

                # Сохраняем файл
                file_index += 1
                counters["files"] += 1
                fname = os.path.join(
                    out_dir,
                    f"{time.strftime('%Y%m%d_%H%M%S')}_{counters['files']:04d}.txt"
                )
                with open(fname, "wb") as f:
                    f.write(data)

                speed = file_size / dt / 1024 if dt > 0 else 0
                print(f"  {GREEN('OK')}  {dt*1000:.0f} ms  {speed:.1f} KB/s  -> {fname}")

            else:
                print(f"{ts()} {RED('ERR')}   {peer}  unknown frame 0x{frame_type:02X}")
                break

    except ConnectionError as e:
        print(f"{ts()} {RED('DISCONNECT')} {peer}  ({e})")
    except Exception as e:
        print(f"{ts()} {RED('ERROR')}     {peer}  {e}")
    finally:
        conn.close()

# ── Основной цикл ─────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description="RP2350-ETH framed TCP server")
    ap.add_argument("--port", type=int, default=LISTEN_PORT)
    ap.add_argument("--out",  default=OUT_DIR)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((LISTEN_HOST, args.port))
    srv.listen(4)

    counters = {"files": 0}
    print(f"{GREEN('Server')} listening on :{args.port}  ->  files -> {args.out}/")
    print(f"Protocol:  0x01 len text  (debug)  |  0x02 uint32LE bytes  (file)")
    print(f"Press Ctrl+C to stop\n")

    try:
        while True:
            conn, addr = srv.accept()
            t = threading.Thread(target=handle_client,
                                  args=(conn, addr, args.out, counters),
                                  daemon=True)
            t.start()
    except KeyboardInterrupt:
        print(f"\n{YELLOW('Stopped.')}  Received {counters['files']} file(s).")
    finally:
        srv.close()

if __name__ == "__main__":
    main()

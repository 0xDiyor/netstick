#!/usr/bin/env python3
"""LAN throughput server for the walking site survey.

Run this on a machine with a wired (or at least strong) connection to the same
router, then walk around with the dongle. It answers UDP discovery so the
dongle finds it by itself, and serves download / upload runs over TCP.

    python3 tools/survey_server.py            # port 7777

Protocol (one line from the client after connect):
    DL <ms>   server streams bytes for <ms> milliseconds, then closes
    UL <ms>   client streams bytes, half-closes; server replies "OK <bytes> <ms>"
"""
import os, socket, sys, threading, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7777
CHUNK = os.urandom(65536)        # incompressible, in case anything in the path compresses

def handle(conn, addr):
    conn.settimeout(5)
    try:
        line = b""
        while not line.endswith(b"\n") and len(line) < 32:
            c = conn.recv(1)
            if not c: return
            line += c
        parts = line.decode(errors="replace").split()
        if len(parts) != 2 or parts[0] not in ("DL", "UL"): return
        ms = max(200, min(int(parts[1]), 30000))
        if parts[0] == "DL":
            end = time.time() + ms / 1000
            sent = 0
            while time.time() < end:
                conn.sendall(CHUNK)
                sent += len(CHUNK)
            elapsed = ms / 1000
            print(f"{addr[0]}  download  {sent*8/elapsed/1e6:7.1f} Mbit/s  ({sent//1024} KB in {ms} ms)")
        else:
            t0 = time.time(); got = 0
            conn.settimeout(ms / 1000 + 3)
            while True:
                data = conn.recv(65536)
                if not data: break
                got += len(data)
                if time.time() - t0 > ms / 1000 + 2: break
            el = max(time.time() - t0, 1e-3)
            conn.sendall(f"OK {got} {int(el*1000)}\n".encode())
            print(f"{addr[0]}  upload    {got*8/el/1e6:7.1f} Mbit/s  ({got//1024} KB in {int(el*1000)} ms)")
    except Exception as e:
        print(f"{addr[0]}  error: {e}")
    finally:
        try: conn.close()
        except Exception: pass

def discovery():
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    u.bind(("", PORT))
    while True:
        data, addr = u.recvfrom(64)
        if data.startswith(b"NETSTICK?"):
            u.sendto(f"NETSTICK v1 {PORT}".encode(), addr)
            print(f"{addr[0]}  discovered us")

def main():
    threading.Thread(target=discovery, daemon=True).start()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", PORT)); s.listen(4)
    print(f"survey server listening on tcp/udp {PORT}  (ctrl-c to stop)")
    while True:
        conn, addr = s.accept()
        threading.Thread(target=handle, args=(conn, addr), daemon=True).start()

if __name__ == "__main__":
    try: main()
    except KeyboardInterrupt: pass

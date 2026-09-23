#!/usr/bin/env python3
"""Time HTTP uploads with no other active driver sockets or game timers.

Usage: python3 tools/test_http_idle_upload.py build-http/src/driver
Each size boots a fresh driver. After a one-shot boot delay, only the HTTP
exchange and its transport timers can wake the loop: the game tick is scheduled
after the subprocess watchdog. A Linux LD_PRELOAD fixture caps the TCP send
buffer and counts real chokes so loopback cannot hide missing POLLOUT rearming.
Requires Python's standard library and a C compiler. --trace also needs strace.
"""
import argparse
import contextlib
import hashlib
import http.server
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time


class UploadHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_POST(self):
        start = time.monotonic()
        size = int(self.headers["Content-Length"])
        received = 0
        digest = hashlib.sha256()
        # Only the peer waits. It then drains continuously, without sending
        # anything that could wake the driver's read side before the response.
        time.sleep(0.05)
        try:
            while received < size:
                chunk = self.rfile.read1(min(65536, size - received))
                if not chunk:
                    return
                received += len(chunk)
                digest.update(chunk)
                self.server.chunks.append((time.monotonic() - start, received))
            self.server.received = received
            self.server.digest = digest.hexdigest()
            payload = str(received).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass


def socket_fixture(repo, root, driver):
    library = root / "http_test_socket.so"
    subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                    "-o", str(library), str(repo / "tools/http_test_socket.c"), "-ldl"],
                   check=True)
    env = os.environ.copy()
    preload = [str(library)]
    # GCC's shared ASan runtime must precede the fixture. Clang normally links
    # its runtime into the executable and needs no entry here.
    linked = subprocess.run(["ldd", str(driver)], capture_output=True, text=True, check=True)
    asan = re.search(r"libasan\S* => (\S+)", linked.stdout)
    if asan:
        preload.insert(0, asan[1])
    if env.get("LD_PRELOAD"):
        preload.append(env["LD_PRELOAD"])
    env["LD_PRELOAD"] = ":".join(preload)
    return env


def run_uploads(driver, log_dir, sizes=(200000, 2000000), trace=False):
    if not sys.platform.startswith("linux"):
        print("SKIP idle upload backpressure fixture (requires Linux)")
        return True
    repo = Path(__file__).resolve().parent.parent
    log_dir.mkdir(parents=True, exist_ok=True)
    passed = True
    with tempfile.TemporaryDirectory(prefix="fluffos-http-idle-") as tmp:
        root = Path(tmp)
        env = socket_fixture(repo, root, driver)
        mudlib = root / "testsuite"
        shutil.copytree(repo / "testsuite", mudlib,
                        ignore=shutil.ignore_patterns("log", "*.o", "node_modules"))
        (mudlib / "log").mkdir(exist_ok=True)
        config = (mudlib / "etc/config.test").read_text()
        config = re.sub(r"(?m)^(?:port number|external_port_\d+(?:_tls)?|websocket http dir)\s*:.*\n", "", config)
        config = config.replace("master file : /single/master", "master file : /clone/http_idle_upload")
        config = config.replace("maximum buffer size : 400000", "maximum buffer size : 8000000")
        config = config.replace("gametick msec : 100", "gametick msec : 60000")
        config += "\nport number : 0\n"
        for size in sizes:
            with contextlib.ExitStack() as stack:
                server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), UploadHandler,
                                                        bind_and_activate=False)
                server.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32768)
                server.server_bind()
                server.server_activate()
                server.daemon_threads = True
                server.chunks = []
                server.received = 0
                server.digest = None
                threading.Thread(target=server.serve_forever, daemon=True).start()
                stack.callback(server.server_close)
                stack.callback(server.shutdown)
                cfg = root / "idle.cfg"
                cfg.write_text(config.replace("http allowed hosts :",
                    f"http allowed hosts : 127.0.0.1:{server.server_port}"))
                cmd = [str(driver), str(cfg), f"-fupload:{server.server_port}:{size}"]
                run_env = env
                if trace:
                    cmd = ["strace", "-ttt", "-T", "-s", "96", "-o",
                           str(log_dir / f"{size}.strace"),
                           "-E", f"LD_PRELOAD={env['LD_PRELOAD']}", "-e",
                           "trace=network,epoll_wait,epoll_pwait,epoll_pwait2,epoll_ctl,poll,read,write", *cmd]
                    run_env = os.environ.copy()
                log_path = log_dir / f"{size}.log"
                with log_path.open("w") as log:
                    try:
                        run = subprocess.run(cmd, cwd=mudlib, stdout=log,
                                             stderr=subprocess.STDOUT, timeout=10, env=run_env)
                        code = run.returncode
                    except subprocess.TimeoutExpired:
                        code = -1
                output = log_path.read_text(errors="replace")
                match = re.search(r"IDLE UPLOAD status=(\d+) bytes=(\d+) ms=(\d+)", output)
                chokes = re.search(r"HTTP_TEST_SOCKET chokes=(\d+)", output)
                expected = hashlib.sha256(b"A" + bytes(size - 2) + b"Z").hexdigest()
                ok = (code == 0 and match is not None and
                      tuple(map(int, match.groups()[:2])) == (200, size) and
                      int(match[3]) < 500 and chokes is not None and int(chokes[1]) > 0 and
                      server.received == size and server.digest == expected and
                      not re.search(r"Check [Ff]ailed|AddressSanitizer|runtime error:|Bad ref count|LeakSanitizer", output))
                passed &= ok
                timing = f"{match[3]} ms" if match else "no successful response"
                choke_count = chokes[1] if chokes else "unverified"
                print(f"{'PASS' if ok else 'FAIL'} idle POST {size} bytes: {timing}; "
                      f"socket chokes={choke_count}", flush=True)
                (log_dir / f"{size}.receive").write_text("".join(
                    f"{elapsed:.6f}s {count} bytes\n" for elapsed, count in server.chunks))
                if not ok:
                    print(output[-1500:])
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    parser.add_argument("--log-dir", type=Path, default=Path("/tmp/fluffos-http-idle"))
    parser.add_argument("--size", type=int, action="append", choices=(200000, 2000000))
    parser.add_argument("--trace", action="store_true")
    args = parser.parse_args()
    return 0 if run_uploads(args.driver.resolve(), args.log_dir.resolve(),
                           args.size or (200000, 2000000), args.trace) else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Check timeouts armed after LPC has run longer than them in one execution.

Usage: python3 tools/test_http_deadline.py build-http/src/driver
libevent measures a new timeout from the time it cached when the loop pass
began. LPC burns 2 s of CPU, then makes an http_request with a 1000 ms timeout
and a 0.5 s call_out_walltime. The request must reach the server and return
200, and the call_out must wait its full delay.
"""
import argparse
import contextlib
import http.server
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import threading


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_GET(self):
        self.server.seen.append(self.path)
        self.send_response(200)
        self.send_header("Content-Length", "2")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(b"ok")
        except (BrokenPipeError, ConnectionResetError):
            pass  # The unfixed driver drops the request as its deadline fires.


def run_deadline(driver, log_path):
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="fluffos-http-deadline-") as tmp, \
            contextlib.ExitStack() as stack:
        root = Path(tmp)
        mudlib = root / "testsuite"
        shutil.copytree(repo / "testsuite", mudlib,
                        ignore=shutil.ignore_patterns("log", "*.o", "node_modules"))
        (mudlib / "log").mkdir(exist_ok=True)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        server.seen = []
        threading.Thread(target=server.serve_forever, daemon=True).start()
        stack.callback(server.server_close)
        stack.callback(server.shutdown)
        config = (mudlib / "etc/config.test").read_text()
        config = re.sub(r"(?m)^(?:port number|external_port_\d+(?:_tls)?|websocket http dir)\s*:.*\n", "", config)
        config = config.replace("master file : /single/master", "master file : /clone/http_deadline")
        config = config.replace("http allowed hosts :",
                                f"http allowed hosts : 127.0.0.1:{server.server_port}")
        config += "\nport number : 0\n"
        cfg = root / "deadline.cfg"
        cfg.write_text(config)
        with log_path.open("w") as log:
            try:
                code = subprocess.run([str(driver), str(cfg), f"-fdeadline:{server.server_port}"],
                                      cwd=mudlib, stdout=log, stderr=subprocess.STDOUT,
                                      timeout=20).returncode
            except subprocess.TimeoutExpired:
                code = -1
        output = log_path.read_text(errors="replace")
        status = re.search(r'DEADLINE HTTP (status=\d+|rejected)(?:[^"]*"error" : "(\w+)")?', output)
        callout = re.search(r"DEADLINE CALLOUT ms=(\d+)", output)
        http_ok = code == 0 and status is not None and status[1] == "status=200" and \
            server.seen == ["/ok"]
        callout_ok = callout is not None and int(callout[1]) >= 450
        print(f"{'PASS' if http_ok else 'FAIL'} http_request after 2 s of LPC, timeout 1000 ms: "
              f"{' '.join(filter(None, status.groups())) if status else 'no result'}; server saw {len(server.seen)} request(s)")
        print(f"{'PASS' if callout_ok else 'FAIL'} call_out_walltime 0.5 s after 2 s of LPC: "
              f"fired after {callout[1] + ' ms' if callout else 'never'}")
        if not (http_ok and callout_ok):
            print(output[-1500:])
        return http_ok and callout_ok


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    parser.add_argument("--log", type=Path, default=Path("/tmp/fluffos-http-deadline.log"))
    args = parser.parse_args()
    return 0 if run_deadline(args.driver.resolve(), args.log) else 1


if __name__ == "__main__":
    raise SystemExit(main())

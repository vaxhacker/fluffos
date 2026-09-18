#!/usr/bin/env python3
"""Exercise http_request() uploads that force TCP backpressure on the driver.

A server that accepts the request and then reads the body slowly makes the
client's socket send buffer fill mid-upload. That is the condition under which
libwebsockets reports the write pipe as choked and the driver must re-arm the
writable callback; without that re-arm the exchange deadlocks and only the
deadline settles it.

This is a standalone checker for the PACKAGE_HTTP client, meant to be run
against a driver built with the http package (the CI harness tools/test_http.py
carries a single-case version of the same check):

    python3 tools/test_http_large_body.py build/src/driver

It is intentionally NOT wired into ctest: it needs a real event loop and real
sockets. `tools/test_http.py` covers the functional matrix; this one only
covers "can a body larger than the socket buffer actually be delivered".

The LPC side deliberately avoids `([ "k": v ])` mapping literals, because the
older test drivers in this tree reject that syntax.
"""

import argparse
import contextlib
import http.server
import os
import re
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

STALL_SECONDS = 4.0
RESULT_TIMEOUT = 30

# The probe LPC addresses these fixed ports; the servers below bind them.
PLAIN_PORT = 18099
TLS_PORT = 18100
SINK_PORT = 18101
# Ports in testsuite/etc/config.test can be taken by an unrelated process on a
# dev box; the listener is irrelevant to this check.
MUD_PORT = 18098


class StallingHandler(http.server.BaseHTTPRequestHandler):
    """Read the request body only after a delay, so the upload stalls."""

    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _serve(self):
        try:
            length = int(self.headers.get("Content-Length", 0) or 0)
            time.sleep(STALL_SECONDS)
            got = 0
            while got < length:
                chunk = self.rfile.read(min(65536, length - got))
                if not chunk:
                    break
                got += len(chunk)
            body = f"received {got}".encode()
            # A short body is a failed upload, whatever the client believes.
            self.send_response(200 if got == length else 400)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass

    do_POST = do_PUT = do_GET = _serve


class StallingServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class SinkServer:
    """Accept and never read: the upload can never complete."""

    def __init__(self, port):
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.thread = threading.Thread(target=self._accept_loop, daemon=True)
        self.thread.start()

    def _accept_loop(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self._hold, args=(conn,), daemon=True).start()

    @staticmethod
    def _hold(conn):
        time.sleep(120)

    def close(self):
        with contextlib.suppress(OSError):
            self.sock.close()


PROBE_LPC = r"""
int done = 0;
int total = 1;
int failures = 0;

int parse_size(string s) {
  int i = strsrch(s, ",");
  if (i < 0) i = strsrch(s, ":");
  int n = to_int(i >= 0 ? s[i + 1..] : s);
  return n > 0 ? n : 300000;
}

void one_done(mixed r) {
  done++;
  if (mapp(r) && r["status"] == 200)
    debug_message(sprintf("PROBE %d/%d ok status=%d\n", done, total, r["status"]));
  else {
    failures++;
    debug_message(sprintf("PROBE %d/%d REJECTED %O\n", done, total, r));
  }
  if (done >= total) {
    debug_message(sprintf("PROBE SUMMARY ok=%d fail=%d\n", total - failures, failures));
    shutdown(failures ? 1 : 0);
  }
}

int main(string arg) {
  int n = parse_size(arg);
  total = strsrch(arg, "seq") >= 0 ? 4 : 1;
  string url = strsrch(arg, "tls") >= 0 ? "https://localhost:18100/bigpost" :
               strsrch(arg, "sink") >= 0 ? "http://127.0.0.1:18101/bigpost" :
               "http://127.0.0.1:18099/bigpost";

  mapping hdrs = ([]);
  hdrs["Content-Type"] = "application/octet-stream";
  mapping opts = ([]);
  opts["method"] = "POST";
  opts["headers"] = hdrs;
  opts["body"] = repeat_string("x", n);
  opts["timeout"] = 15000;

  debug_message(sprintf("PROBE: %d x %d bytes -> %s\n", total, n, url));
  call_out(function() {
    debug_message(sprintf("PROBE: NO RESULT AFTER 25s (STALLED) done=%d/%d\n", done, total));
    shutdown(3);
  }, 25);
  for (int i = 1; i <= total; i++)
    promise_then(http_request(url, opts), (: one_done :), (: one_done :));
  return 1;
}
"""

MASTER_LPC = r"""
inherit "/inherit/master/valid";

string get_root_uid() { return "ROOT"; }
string get_bb_uid() { return "BACKBONE"; }
string creator_file(string str) { return "ROOT"; }
string author_file(string str) { return "ROOT"; }
string domain_file(string str) { return "ROOT"; }

mixed valid_http(string url, object caller) { return 1; }

void flag(string str) {
  mixed err = catch("/command/largebody"->main(str));
  if (err) { debug_message(sprintf("FLAG ERROR: %O\n", err)); shutdown(2); }
}
"""


def make_certificate(root):
    cert, key = root / "cert.pem", root / "key.pem"
    subprocess.run([
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
        "-keyout", str(key), "-out", str(cert),
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return cert, key


def build_mudlib(repo, root, allowed, cert):
    mudlib = root / "mudlib"
    shutil.copytree(repo / "testsuite", mudlib,
                    ignore=shutil.ignore_patterns("log", "*.o", "node_modules"))
    (mudlib / "log").mkdir(exist_ok=True)
    (mudlib / "premaster.lpc").write_text(MASTER_LPC)
    (mudlib / "command" / "largebody.lpc").write_text(PROBE_LPC)
    config = (mudlib / "etc" / "config.test").read_text()
    config = config.replace("master file : /single/master", "master file : /premaster")
    config = "\n".join(line for line in config.splitlines()
                       if not line.startswith("external_port_")
                       and not line.startswith("port number"))
    # The test config caps strings at 200000 bytes and repeat_string() clips
    # silently, so without this every "large" body would really be 200 KB.
    config = config.replace("maximum string length : 200000", "maximum string length : 20000000")
    config = config.replace("maximum buffer size : 400000", "maximum buffer size : 20000000")
    config += "\nhttp allowed hosts : " + " ".join(allowed) + "\n"
    config += "http default timeout : 20000\nhttp max body : 8000000\n"
    config += f"port number : {MUD_PORT}\n"
    cfg = root / "largebody.cfg"
    cfg.write_text(config)
    return mudlib, cfg


def run_case(driver, mudlib, cfg, flag, cert, expect_ok):
    env = os.environ.copy()
    env["SSL_CERT_FILE"] = str(cert)
    log = mudlib / "log" / "largebody.log"
    argv = [str(driver), str(cfg), f"-f-largebody:{flag}"]
    try:
        proc = subprocess.run(argv, cwd=mudlib, env=env, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=RESULT_TIMEOUT + 20,
                              text=True)
        output = proc.stdout
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or "") + "\n<driver did not exit>"
        proc = None
    summary = None
    for line in output.splitlines():
        match = re.search(r"PROBE SUMMARY ok=(\d+) fail=(\d+)", line)
        if match:
            summary = (int(match.group(1)), int(match.group(2)))
    # Everywhere but the sink each request must fulfil; with the sink the
    # exchange can only end in a rejection.
    ok = summary is not None and ((summary[1] == 0) if expect_ok else (summary[0] == 0))
    stalled = "STALLED" in output
    status = "PASS" if ok else "FAIL"
    tail = [line for line in output.splitlines() if line.startswith("PROBE")]
    detail = f"summary={summary}" if summary else "no PROBE SUMMARY line"
    print(f"[{status}] {flag:16s} {detail}")
    for line in tail[-5:]:
        print(f"          {line}")
    if stalled:
        print("          (the exchange never completed; only the deadline settled it)")
    if status == "FAIL":
        print(output[-2500:])
    return status == "PASS"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    parser.add_argument("--repo", type=Path,
                        default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    driver = args.driver.resolve()

    with tempfile.TemporaryDirectory(prefix="fluffos-largebody-") as tmp:
        root = Path(tmp)
        cert, key = make_certificate(root)

        plain = StallingServer(("127.0.0.1", PLAIN_PORT), StallingHandler)
        tls_server = StallingServer(("127.0.0.1", TLS_PORT), StallingHandler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        tls_server.socket = ctx.wrap_socket(tls_server.socket, server_side=True)
        sink = SinkServer(SINK_PORT)
        for server in (plain, tls_server):
            threading.Thread(target=server.serve_forever, daemon=True).start()

        allowed = [
            f"127.0.0.1:{PLAIN_PORT}",
            f"localhost:{TLS_PORT}",
            f"127.0.0.1:{SINK_PORT}",
        ]
        mudlib, cfg = build_mudlib(args.repo, root, allowed, cert)

        cases = [
            ("plain,1024", True),      # control: fits in one write
            ("plain,131072", True),    # below the socket buffer
            ("plain,3000000", True),   # far above it, server stalls 4s
            ("tls,5000000", True),     # same, over TLS
            ("seq,400000", True),      # four in a row, connection reuse
            ("sink,5000000", False),   # peer never reads: must reject, not hang
        ]
        results = [run_case(driver, mudlib, cfg, flag, cert, expect)
                   for flag, expect in cases]

        plain.shutdown()
        tls_server.shutdown()
        sink.close()

    print(f"\n{sum(results)}/{len(results)} cases behaved as expected")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

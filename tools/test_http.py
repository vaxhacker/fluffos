#!/usr/bin/env python3
"""Exercise PACKAGE_HTTP against local HTTP/TLS servers, outside the LPC suite.

Usage: python3 tools/test_http.py build-http/src/driver
Python's standard library and openssl are required; the Linux idle-upload cases
also use a C compiler to build a socket-buffer fixture.
The mudlib and certificates are temporary; no CouchDB or websocket listener is needed.
"""
import argparse
import contextlib
import http.server
import os
from pathlib import Path
import re
import shutil
import ssl
import subprocess
import tempfile
import threading
import time

from test_http_deadline import run_deadline
from test_http_idle_upload import run_uploads


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_request(self):
        path = self.path.split("?", 1)[0]
        self.server.seen.append(path)
        if path == "/upload":
            # Let the client's socket send buffer fill before draining it: a
            # body above that buffer used to stall after the first choked
            # write until the request deadline.
            time.sleep(1)
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if path.startswith("/gate-"):
            raise AssertionError("Access gate was bypassed")
        if path == "/slow":
            time.sleep(0.5)
        if path == "/linger":
            time.sleep(5)
        status = int(path.rsplit("/", 1)[1]) if path.startswith("/status/") else 200
        payload = b"hello"
        extra = {"X-Test": "custom", "ETag": '"revision"'}
        if path in ("/redirect", "/redirect307", "/redirect303", "/redirect-denied",
                    "/redirect-host", "/loop", "/cross-origin", "/downgrade"):
            status = 307 if path == "/redirect307" else 303 if path == "/redirect303" else 302
            extra["Location"] = {
                "/redirect": "./ok", "/redirect307": "/echo", "/redirect303": "/echo",
                "/redirect-denied": "/gate-denied", "/redirect-host": "http://denied.invalid/",
                "/loop": "/loop",
                "/cross-origin": f"http://127.0.0.1:{self.server.plain_port}/credentials",
                "/downgrade": f"http://127.0.0.1:{self.server.plain_port}/ok",
            }[path]
        elif path == "/echo":
            payload = body
            extra["X-Method"] = self.command
        elif path == "/upload":
            payload = str(len(body)).encode()
        elif path == "/binary":
            payload = b"\0\xffA"
        elif path == "/large":
            payload = b"x" * 250001
        elif path == "/credentials":
            payload = (self.headers.get("Authorization", "") + "|" +
                       self.headers.get("Cookie", "")).encode()
        elif path == "/eof":
            payload = b"until EOF"
        elif path == "/truncated":
            payload = b"short"
        self.send_response(status)
        for name, value in extra.items():
            self.send_header(name, value)
        self.send_header("Connection", "close")
        if path in ("/chunked", "/chunk-big"):
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            count = 70 if path == "/chunked" else 401
            for _ in range(count):
                self.wfile.write(b"3e8\r\n" + b"c" * 1000 + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            if path != "/eof":
                size = 500001 if path == "/oversize" else 100 if path == "/truncated" else len(payload)
                self.send_header("Content-Length", str(size))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(payload)
        self.close_connection = True

    def safe_request(self):
        try:
            self.do_request()
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass  # Timeout/size rejection intentionally closes the client.

    do_GET = do_POST = do_PUT = do_DELETE = do_HEAD = safe_request


def certificate(root, name):
    cert, key = root / f"{name}.pem", root / f"{name}.key"
    subprocess.run([
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
        "-keyout", str(key), "-out", str(cert),
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return cert, key


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    parser.add_argument("--log", type=Path, default=Path("/tmp/fluffos-http-integration.log"))
    args = parser.parse_args()
    driver = args.driver.resolve()
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="fluffos-http-") as tmp, contextlib.ExitStack() as stack:
        root = Path(tmp)
        mudlib = root / "testsuite"
        shutil.copytree(repo / "testsuite", mudlib,
                        ignore=shutil.ignore_patterns("log", "*.o", "node_modules"))
        (mudlib / "log").mkdir(exist_ok=True)
        trusted = certificate(root, "trusted")
        untrusted = certificate(root, "untrusted")
        servers = []
        for cert in (None, trusted, untrusted):
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            server.daemon_threads = True
            server.seen = []
            if cert:
                tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                tls.load_cert_chain(*cert)
                server.socket = tls.wrap_socket(server.socket, server_side=True)
            threading.Thread(target=server.serve_forever, daemon=True).start()
            stack.callback(server.server_close)
            stack.callback(server.shutdown)
            servers.append(server)
        port, tls_port, bad_port = [s.server_port for s in servers]
        for server in servers:
            server.plain_port = port
        config = (mudlib / "etc/config.test").read_text()
        # No websocket listener: the HTTP client must own its own boot context.
        config = re.sub(r"(?m)^(?:port number|external_port_\d+(?:_tls)?|websocket http dir)\s*:.*\n", "", config)
        config = config.replace("master file : /single/master", "master file : /clone/http_master")
        config = config.replace("http allowed hosts :", "http allowed hosts : " + " ".join([
            f"127.0.0.1:{port}", f"localhost:{port}", f"localhost:{tls_port}",
            f"localhost:{bad_port}", "127.0.0.1:1", "x.invalid:80",
        ]))
        # The upload check needs a request body far above the socket send
        # buffer; the response caps ("http max body") are left as configured.
        config = config.replace("maximum buffer size : 400000", "maximum buffer size : 8000000")
        config += "\nport number : 0\n"
        cfg = root / "http.cfg"
        cfg.write_text(config)
        env = os.environ.copy()
        env["SSL_CERT_FILE"] = str(trusted[0])
        with args.log.open("w") as log:
            run = subprocess.run([str(driver), str(cfg), f"-fhttp:{port}:{tls_port}:{bad_port}"],
                                 cwd=mudlib, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=45)
        output = args.log.read_text(errors="replace")
        forbidden = [p for server in servers for p in server.seen if p.startswith("/gate-")]
        if (run.returncode or "HTTP integration checks succeeded." not in output or forbidden or
                re.search(r"Check [Ff]ailed|AddressSanitizer|runtime error:|Bad ref count|LeakSanitizer", output)):
            print(output[-6000:])
            raise SystemExit(f"HTTP integration failed (exit {run.returncode}); see {args.log}")
        print(f"HTTP integration checks succeeded; log: {args.log}")
    if not run_uploads(driver, args.log.with_suffix(".idle")):
        raise SystemExit("Idle HTTP upload checks failed")
    if not run_deadline(driver, args.log.with_suffix(".deadline")):
        raise SystemExit("HTTP deadline checks failed")


if __name__ == "__main__":
    main()

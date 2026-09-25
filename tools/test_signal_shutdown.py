#!/usr/bin/env python3
"""Check that SIGTERM reaches master::signal_shutdown() from the game loop.

Usage: python3 tools/test_signal_shutdown.py build/src/driver
Starts the driver on testsuite/clone/signal_shutdown.lpc three times:
graceful - the apply schedules a call_out that calls shutdown(7) half a second
           later; the driver must run it and exit 7, without crash().
error    - the apply errors; the driver must take the crash path (fatal(),
           master::crash() outside DEBUG builds, abort).
wait     - the apply finishes nothing; a second SIGTERM must crash the driver.
"""
import argparse
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile
import time


def wait_for(log_path, marker, process, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if marker in log_path.read_text(errors="replace"):
            return True
        if process.poll() is not None:
            return False
        time.sleep(0.05)
    return False


def run_mode(driver, mudlib, cfg, mode, log_path):
    with log_path.open("w") as log:
        process = subprocess.Popen([str(driver), str(cfg), f"-fsignal:{mode}"], cwd=mudlib,
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            if not wait_for(log_path, "SIGNAL READY", process, 30):
                return None, log_path.read_text(errors="replace")
            process.send_signal(signal.SIGTERM)
            if mode == "wait":
                wait_for(log_path, "SIGNAL APPLY", process, 10)
                time.sleep(0.5)
                process.send_signal(signal.SIGTERM)
            code = process.wait(20)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            code = None
    return code, log_path.read_text(errors="replace")


def run_signals(driver, log_dir):
    repo = Path(__file__).resolve().parent.parent
    results = []
    with tempfile.TemporaryDirectory(prefix="fluffos-signal-") as tmp:
        root = Path(tmp)
        mudlib = root / "testsuite"
        shutil.copytree(repo / "testsuite", mudlib,
                        ignore=shutil.ignore_patterns("log", "*.o", "node_modules"))
        (mudlib / "log").mkdir(exist_ok=True)
        config = (mudlib / "etc/config.test").read_text()
        config = re.sub(r"(?m)^(?:port number|external_port_\d+(?:_tls)?|websocket http dir)\s*:.*\n", "", config)
        config = config.replace("master file : /single/master", "master file : /clone/signal_shutdown")
        config += "\nport number : 0\n"
        cfg = root / "signal.cfg"
        cfg.write_text(config)

        code, out = run_mode(driver, mudlib, cfg, "graceful", log_dir / "signal-graceful.log")
        results.append(("SIGTERM runs master::signal_shutdown() in the loop; its call_out exits 7",
                        code == 7 and "SIGNAL APPLY 15 this_player=0" in out
                        and "SIGNAL FINISHED 15" in out and "FATAL ERROR" not in out, code, out))

        code, out = run_mode(driver, mudlib, cfg, "error", log_dir / "signal-error.log")
        results.append(("an erroring signal_shutdown() falls back to the crash path",
                        code == -signal.SIGABRT and "SIGNAL APPLY 15" in out
                        and "FATAL ERROR: SIGTERM" in out, code, out))

        code, out = run_mode(driver, mudlib, cfg, "wait", log_dir / "signal-wait.log")
        results.append(("a second SIGTERM while shutting down crashes at once",
                        code == -signal.SIGABRT and out.count("SIGNAL APPLY") == 1
                        and "FATAL ERROR: SIGTERM" in out, code, out))

    for label, ok, code, out in results:
        print(f"{'PASS' if ok else 'FAIL'} {label} (exit {code})")
        if not ok:
            print(out[-1500:])
    return all(ok for _, ok, _, _ in results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    parser.add_argument("--log-dir", type=Path, default=Path(tempfile.gettempdir()))
    args = parser.parse_args()
    return 0 if run_signals(args.driver.resolve(), args.log_dir) else 1


if __name__ == "__main__":
    raise SystemExit(main())

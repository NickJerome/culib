"""Exercise a real macOS utun with the IP stack and example_net_virtual."""

import os
import pathlib
import re
import select
import signal
import subprocess
import sys
import time


ROOT = pathlib.Path(__file__).resolve().parents[1]


def finish(process):
    if process is None:
        return "", ""
    if process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    try:
        return process.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        return process.communicate()


def main():
    if sys.platform != "darwin" or len(sys.argv) != 2 or sys.argv[1] not in ("debug", "release"):
        raise RuntimeError("usage: python3 tests/utun_roundtrip.py debug|release on macOS")
    programs = list(ROOT.glob("build/macosx/*/" + sys.argv[1] + "/example_net_virtual"))
    if len(programs) != 1:
        raise RuntimeError("build exactly one macOS example_net_virtual target first")

    worker = subprocess.Popen(["sudo", "-n", str(programs[0]), "--icmp-echo"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, start_new_session=True)
    capture = None
    try:
        ready, _, _ = select.select([worker.stdout], [], [], 10)
        if not ready:
            raise RuntimeError("utun did not open within 10 seconds")
        opening = worker.stdout.readline().strip()
        match = re.fullmatch(r"opened (utun[0-9]+)", opening)
        if not match:
            raise RuntimeError("unexpected utun output: " + opening)
        name = match.group(1)
        print(opening, flush=True)

        subprocess.run(["sudo", "-n", "ifconfig", name, "inet",
                        "198.51.100.1", "198.51.100.2", "up"], check=True)
        interface = subprocess.run(["ifconfig", name], text=True,
                                   capture_output=True, timeout=5)
        print(interface.stdout, end="", flush=True)
        route = subprocess.run(["route", "-n", "get", "198.51.100.2"],
                               text=True, capture_output=True, timeout=5)
        print(route.stdout, end="", flush=True)
        if route.returncode != 0 or not re.search(
                r"^\s*interface:\s*" + re.escape(name) + r"\s*$",
                route.stdout, re.MULTILINE):
            raise RuntimeError("test route does not use " + name + ": " + route.stdout)

        capture = subprocess.Popen(["sudo", "-n", "tcpdump", "-n", "-U", "-l",
                                    "-vvv", "-i", name, "-c", "4", "icmp"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, start_new_session=True)
        time.sleep(0.3)
        ping = subprocess.run(["ping", "-c", "1", "-W", "5000", "198.51.100.2"],
                              text=True, capture_output=True, timeout=10)
        print(ping.stdout, end="", flush=True)
        if ping.returncode != 0:
            raise RuntimeError("utun ICMP ping failed: " + ping.stderr)
        if worker.wait(timeout=15) != 0:
            raise RuntimeError("utun ICMP responder failed")
    finally:
        captured, capture_errors = finish(capture)
        if captured:
            print("utun capture:\n" + captured, end="")
        if capture_errors:
            print(capture_errors, end="", file=sys.stderr)
        output, errors = finish(worker)
        if output:
            print(output, end="")
        if errors:
            print(errors, end="", file=sys.stderr)
    if "ICMP echo replied" not in output:
        raise RuntimeError("utun responder did not confirm a reply")
    print("macOS utun IP roundtrip passed")


if __name__ == "__main__":
    main()

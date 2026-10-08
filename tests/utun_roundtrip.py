"""Exercise a real macOS utun with the IP stack and example_net_virtual."""

import pathlib
import re
import select
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    if sys.platform != "darwin" or len(sys.argv) != 2 or sys.argv[1] not in ("debug", "release"):
        raise RuntimeError("usage: python3 tests/utun_roundtrip.py debug|release on macOS")
    programs = list(ROOT.glob("build/macosx/*/" + sys.argv[1] + "/example_net_virtual"))
    if len(programs) != 1:
        raise RuntimeError("build exactly one macOS example_net_virtual target first")

    worker = subprocess.Popen([str(programs[0]), "--icmp-echo"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True)
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
        route = subprocess.run(["route", "-n", "get", "198.51.100.2"],
                               text=True, capture_output=True, timeout=5)
        if route.returncode != 0 or not re.search(
                r"^\s*interface:\s*" + re.escape(name) + r"\s*$",
                route.stdout, re.MULTILINE):
            raise RuntimeError("test route does not use " + name + ": " + route.stdout)

        ping = subprocess.run(["ping", "-c", "1", "-W", "5000", "198.51.100.2"],
                              text=True, capture_output=True, timeout=10)
        print(ping.stdout, end="", flush=True)
        if ping.returncode != 0:
            raise RuntimeError("utun ICMP ping failed: " + ping.stderr)
        if worker.wait(timeout=15) != 0:
            raise RuntimeError("utun ICMP responder failed")
    finally:
        if worker.poll() is None:
            worker.terminate()
        try:
            output, errors = worker.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            worker.kill()
            output, errors = worker.communicate()
        if output:
            print(output, end="")
        if errors:
            print(errors, end="", file=sys.stderr)
    if "ICMP echo replied" not in output:
        raise RuntimeError("utun responder did not confirm a reply")
    print("macOS utun IP roundtrip passed")


if __name__ == "__main__":
    main()

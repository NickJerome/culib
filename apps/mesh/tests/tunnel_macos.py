"""Check encrypted forwarding between two real macOS utun interfaces."""

import os
import pathlib
import queue
import re
import select
import signal
import subprocess
import sys
import tempfile
import threading
import time


ROOT = pathlib.Path(__file__).resolve().parents[3]
PASSWORD = "mesh-macos-test-password\n"


def finish(process):
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


def run(*command, **kwargs):
    completed = subprocess.run(command, text=True, capture_output=True,
                               timeout=15, **kwargs)
    if completed.returncode != 0:
        raise RuntimeError("command failed: " + " ".join(map(str, command)) +
                           "\n" + completed.stdout + completed.stderr)
    return completed.stdout.strip()


def output_lines(process):
    lines = queue.Queue()

    def collect():
        for line in process.stdout:
            lines.put(line.strip())

    threading.Thread(target=collect, daemon=True).start()
    return lines


def wait_line(lines, prefix, timeout=20):
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError("timed out waiting for " + prefix)
        try:
            line = lines.get(timeout=remaining)
        except queue.Empty:
            raise RuntimeError("timed out waiting for " + prefix)
        print(line, flush=True)
        if line.startswith(prefix):
            return line[len(prefix):]


def start_peer(program, identity, peer_key, port, processes):
    process = subprocess.Popen(
        ["sudo", "-n", str(program), "tunnel", identity, peer_key,
         "127.0.0.1", str(port)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, start_new_session=True)
    processes.append(process)
    lines = output_lines(process)
    process.stdin.write(PASSWORD)
    process.stdin.close()
    process.stdin = None
    name = wait_line(lines, "virtual interface: ")
    if not re.fullmatch(r"utun[0-9]+", name):
        raise RuntimeError("mesh opened an unexpected interface: " + name)
    return process, lines, name


def configure(name, local, remote):
    run("sudo", "-n", "ifconfig", name, "inet", local, remote, "up")
    route = run("route", "-n", "get", remote)
    print(route, flush=True)
    if not re.search(r"^\s*interface:\s*" + re.escape(name) + r"\s*$",
                     route, re.MULTILINE):
        raise RuntimeError("test route does not use " + name)


def wait_capture_ready(process, interface):
    output = bytearray()
    ready = ("listening on " + interface).encode()
    deadline = time.monotonic() + 5
    while ready not in output:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([process.stderr], [], [], remaining)[0]:
            raise RuntimeError("tcpdump did not become ready on " + interface +
                               ": " + output.decode(errors="replace"))
        chunk = os.read(process.stderr.fileno(), 4096)
        if not chunk:
            raise RuntimeError("tcpdump exited before listening on " + interface +
                               ": " + output.decode(errors="replace"))
        output.extend(chunk)
    return output.decode(errors="replace")


def check_forward(source, destination, interface):
    capture = subprocess.Popen(
        ["sudo", "-n", "tcpdump", "-n", "-U", "-l", "-i", interface,
         "-c", "1", "icmp and dst host " + destination],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        start_new_session=True)
    try:
        startup_output = wait_capture_ready(capture, interface)
        ping = subprocess.run(["ping", "-c", "1", "-W", "1000", destination],
                              text=True, capture_output=True, timeout=5)
        print(ping.stdout, end="", flush=True)
        try:
            captured, errors = capture.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            captured, errors = finish(capture)
        print(captured, end="", flush=True)
        if startup_output or errors:
            print(startup_output + errors, end="", file=sys.stderr)
        if capture.returncode != 0 or source + " > " + destination not in captured or \
                "ICMP echo request" not in captured:
            raise RuntimeError("encrypted packet did not reach " + interface)
    finally:
        if capture.poll() is None:
            finish(capture)


def wait_interface_removed(name):
    for _ in range(50):
        if subprocess.run(["ifconfig", name], capture_output=True,
                          timeout=5).returncode != 0:
            return
        time.sleep(0.1)
    raise RuntimeError("mesh utun interface remained after shutdown: " + name)


def main():
    if sys.platform != "darwin" or len(sys.argv) != 2 or \
            sys.argv[1] not in ("debug", "release"):
        raise RuntimeError("usage: python3 tunnel_macos.py debug|release on macOS")
    programs = list(ROOT.glob("build/macosx/*/" + sys.argv[1] + "/mesh"))
    if len(programs) != 1:
        raise RuntimeError("build exactly one macOS mesh target first")
    program = programs[0]
    relay_port = 39011
    processes = []
    forced_shutdown = False
    with tempfile.TemporaryDirectory() as temporary:
        try:
            files = [str(pathlib.Path(temporary) / name)
                     for name in ("a.key", "b.key")]
            keys = [run(str(program), "identity", "create", name,
                        input=PASSWORD) for name in files]
            relay = subprocess.Popen([str(program), "serve", str(relay_port)],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, start_new_session=True)
            processes.append(relay)
            time.sleep(0.2)
            first, first_lines, first_name = start_peer(
                program, files[0], keys[1], relay_port, processes)
            configure(first_name, "10.250.1.1", "10.250.1.2")
            second, second_lines, second_name = start_peer(
                program, files[1], keys[0], relay_port, processes)
            configure(second_name, "10.250.2.1", "10.250.2.2")
            wait_line(first_lines, "authenticated UDP path:")
            wait_line(second_lines, "authenticated UDP path:")
            for _ in range(2):
                check_forward("10.250.1.1", "10.250.1.2", second_name)
                check_forward("10.250.2.1", "10.250.2.2", first_name)

            os.killpg(second.pid, signal.SIGTERM)
            second.wait(timeout=8)
            wait_interface_removed(second_name)

            replacement, replacement_lines, replacement_name = start_peer(
                program, files[1], keys[0], relay_port, processes)
            configure(replacement_name, "10.250.2.1", "10.250.2.2")
            wait_line(first_lines, "authenticated UDP path:", timeout=60)
            wait_line(replacement_lines, "authenticated UDP path:", timeout=20)
            if first.poll() is not None or replacement.poll() is not None:
                raise RuntimeError("mesh peer exited during reconnection")
            for _ in range(2):
                check_forward("10.250.1.1", "10.250.1.2", replacement_name)
                check_forward("10.250.2.1", "10.250.2.2", first_name)
            print("macOS two-utun forwarding and restart passed")
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    try:
                        os.killpg(process.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
            for process in reversed(processes):
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    forced_shutdown = True
                    print("mesh process exceeded shutdown deadline: " +
                          str(process.args), file=sys.stderr)
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()
                errors = process.stderr.read()
                if errors:
                    print(errors, end="", file=sys.stderr)
    if forced_shutdown:
        raise RuntimeError("mesh process did not stop after SIGTERM")
    for name in (first_name, replacement_name):
        wait_interface_removed(name)


if __name__ == "__main__":
    main()

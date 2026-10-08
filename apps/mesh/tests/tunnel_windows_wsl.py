"""Test a real Windows Wintun peer against a Linux TUN peer in WSL2.

Run from an elevated Windows terminal after building both mesh debug targets:
    python apps/mesh/tests/tunnel_windows_wsl.py C:\\path\\to\\amd64\\wintun.dll

The script uses a temporary Windows identity and one /tmp identity in WSL.
It gives the temporary interfaces documentation-only addresses, then pings
through the encrypted tunnel. No persistent route or interface is intended.
"""

import os
import pathlib
import queue
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import uuid


MESH_WIN = pathlib.Path(__file__).resolve().parents[1] / "build/windows/x64/debug/mesh.exe"
PASSWORD = "mesh-cross-test-password\n"


def lines_from(process):
    lines = queue.Queue()

    def read_output():
        for line in process.stdout:
            lines.put(line.strip())

    threading.Thread(target=read_output, daemon=True).start()
    return lines


def wait_line(lines, prefix, timeout=20):
    while True:
        try:
            line = lines.get(timeout=timeout)
        except queue.Empty:
            raise RuntimeError("timed out waiting for " + prefix)
        print(line)
        if line.startswith(prefix):
            return line[len(prefix):]


def run(*command, **kwargs):
    result = subprocess.run(command, text=True, capture_output=True, timeout=25, **kwargs)
    if result.returncode != 0:
        raise RuntimeError("command failed: " + " ".join(map(str, command)) +
                           "\n" + result.stdout + result.stderr)
    return result.stdout.strip()


def wsl_process(*command):
    process = subprocess.Popen(
        ["wsl", "-u", "root", "-e", "sh", "-c",
         'echo __PID__$$; exec "$@"', "sh"] + list(command),
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True)
    output = lines_from(process)
    pid = int(wait_line(output, "__PID__", 10))
    return process, output, pid


def main():
    if len(sys.argv) != 2 or not MESH_WIN.is_file():
        raise RuntimeError("pass the verified x64 Wintun DLL after building mesh debug")
    dll = pathlib.Path(sys.argv[1]).resolve(strict=True)
    if dll.name.lower() != "wintun.dll":
        raise RuntimeError("expected an amd64 wintun.dll")
    shell = shutil.which("pwsh") or "powershell"
    run(shell, "-NoProfile", "-Command",
        "if ((Get-AuthenticodeSignature -LiteralPath '" +
        str(dll).replace("'", "''") +
        "').Status -ne 'Valid') { exit 1 }")
    linux_binary = run(
        "wsl", "-u", "root", "-e", "wslpath", "-u",
        str(MESH_WIN.parents[3] / "linux/x86_64/debug/mesh"))
    relay_port = run(
        "wsl", "-u", "root", "-e", "python3", "-c",
        "import socket; s=socket.socket(); s.bind(('127.0.0.1', 0)); "
        "print(s.getsockname()[1]); s.close()")

    suffix = uuid.uuid4().hex
    windows_key = pathlib.Path(tempfile.gettempdir()) / (
        "culib-mesh-win-" + suffix + ".key")
    linux_key = "/tmp/culib-mesh-wsl-" + suffix + ".key"
    processes = []
    linux_pids = []
    adapter = None
    windows = None
    try:
        windows_pub = run(str(MESH_WIN), "identity", "create", str(windows_key),
                          input=PASSWORD)
        linux_pub = run("wsl", "-u", "root", "-e", linux_binary,
                        "identity", "create", linux_key, input=PASSWORD)
        addresses = run("wsl", "-u", "root", "-e", "ip", "-4", "-o",
                        "addr", "show", "dev", "eth0")
        match = re.search(r"inet ([0-9.]+)/", addresses)
        if not match:
            raise RuntimeError("WSL eth0 has no IPv4 address")
        wsl_ip = match.group(1)

        relay, _, relay_pid = wsl_process(linux_binary, "serve", relay_port)
        processes.append(relay)
        linux_pids.append(relay_pid)
        linux, linux_lines, linux_pid = wsl_process(
            linux_binary, "tunnel", linux_key, windows_pub, "127.0.0.1", relay_port)
        processes.append(linux)
        linux_pids.append(linux_pid)
        linux.stdin.write(PASSWORD)
        linux.stdin.close()
        linux.stdin = None

        tun = wait_line(linux_lines, "virtual interface: ")
        run("wsl", "-u", "root", "-e", "ip", "addr", "add",
            "10.250.1.2/32", "dev", tun)
        run("wsl", "-u", "root", "-e", "ip", "link", "set", tun, "up")
        run("wsl", "-u", "root", "-e", "ip", "route", "add",
            "10.250.1.1/32", "dev", tun, "src", "10.250.1.2")
        env = os.environ.copy()
        env["PATH"] = str(dll.parent) + os.pathsep + env["PATH"]
        windows = subprocess.Popen(
            [str(MESH_WIN), "tunnel", str(windows_key), linux_pub,
             wsl_ip, relay_port],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env=env, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        processes.append(windows)
        windows.stdin.write(PASSWORD)
        windows.stdin.close()
        windows.stdin = None
        windows_lines = lines_from(windows)
        adapter = wait_line(windows_lines, "virtual interface: ")
        configure = (
            "$ErrorActionPreference='Stop'; "
            "New-NetIPAddress -InterfaceAlias '" + adapter.replace("'", "''") +
            "' -IPAddress '10.250.1.1' -PrefixLength 30 "
            "-AddressFamily IPv4 -PolicyStore ActiveStore | Out-Null; "
            "for ($i=0; $i -lt 20; ++$i) { "
            "$a=Get-NetIPAddress -InterfaceAlias '" + adapter.replace("'", "''") +
            "' -IPAddress '10.250.1.1' -AddressFamily IPv4; "
            "if ($a.AddressState -eq 'Preferred') { exit 0 }; "
            "Start-Sleep -Milliseconds 250 }; exit 1")
        run(shell, "-NoProfile", "-Command", configure)
        wait_line(windows_lines, "authenticated UDP path: ", 30)
        wait_line(linux_lines, "authenticated UDP path: ", 30)
        for _ in range(2):
            print(run("ping.exe", "-n", "1", "-w", "5000", "10.250.1.2"))
            print(run("wsl", "-u", "root", "-e", "ping", "-c", "1", "-W", "5",
                      "10.250.1.1"))
        print("Windows Wintun <-> WSL Linux TUN encrypted pings passed")
    finally:
        if windows is not None and windows.poll() is None:
            try:
                windows.send_signal(signal.CTRL_BREAK_EVENT)
            except OSError:
                pass
        for pid in linux_pids:
            subprocess.run(["wsl", "-u", "root", "-e", "kill", "-TERM", str(pid)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for process in processes:
            try:
                process.wait(timeout=6)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            if process.stderr:
                errors = process.stderr.read()
                if errors:
                    print(errors, file=sys.stderr, end="")
        if windows_key.is_file():
            windows_key.unlink()
        subprocess.run(["wsl", "-u", "root", "-e", "rm", "-f", "--", linux_key],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()

"""Root-only Linux integration test for two encrypted mesh TUN peers.

Build apps/mesh in debug mode, then run this script in WSL/Linux as root.
Pass --restart to also restart one peer and verify that the other reconnects.
It creates three temporary network namespaces and removes them on exit.
"""

import os
import pathlib
import select
import subprocess
import sys
import tempfile
import time


MESH = pathlib.Path(__file__).resolve().parents[1] / "build/linux/x86_64/debug/mesh"


def run(*command):
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)


def wait_line(process, prefix, timeout):
    ready, _, _ = select.select([process.stdout], [], [], timeout)
    if not ready:
        raise RuntimeError("mesh did not report " + prefix)
    line = process.stdout.readline().strip()
    if not line.startswith(prefix):
        raise RuntimeError("unexpected mesh output: " + line)
    return line[len(prefix):]


def main():
    if os.geteuid() != 0 or not MESH.is_file():
        raise RuntimeError("run as root after building apps/mesh in Linux debug mode")
    suffix = str(os.getpid())
    hub, peer_a, peer_b = ("mesh-" + part + suffix for part in ("h", "a", "b"))
    namespaces = []
    processes = []
    try:
        for name in (hub, peer_a, peer_b):
            run("ip", "netns", "add", name)
            namespaces.append(name)
            run("ip", "-n", name, "link", "set", "lo", "up")
        for peer, hub_if, peer_if, hub_ip, peer_ip in (
            (peer_a, "ha", "ea", "198.18.1.1/30", "198.18.1.2/30"),
            (peer_b, "hb", "eb", "198.18.2.1/30", "198.18.2.2/30"),
        ):
            run("ip", "-n", hub, "link", "add", hub_if, "type", "veth",
                "peer", "name", peer_if)
            run("ip", "-n", hub, "link", "set", peer_if, "netns", peer)
            run("ip", "-n", hub, "addr", "add", hub_ip, "dev", hub_if)
            run("ip", "-n", peer, "addr", "add", peer_ip, "dev", peer_if)
            run("ip", "-n", hub, "link", "set", hub_if, "up")
            run("ip", "-n", peer, "link", "set", peer_if, "up")
            run("ip", "-n", peer, "route", "add", "default", "via",
                hub_ip.split("/")[0])
        run("ip", "netns", "exec", hub, "sysctl", "-q", "-w",
            "net.ipv4.ip_forward=1")

        with tempfile.TemporaryDirectory() as temporary:
            files = [str(pathlib.Path(temporary) / name) for name in ("a.key", "b.key")]
            keys = []
            for filename in files:
                created = subprocess.run(
                    [str(MESH), "identity", "create", filename],
                    input="mesh-test-password\n", text=True,
                    capture_output=True, timeout=20)
                if created.returncode != 0:
                    raise RuntimeError("identity create: " + created.stderr)
                keys.append(created.stdout.strip())
            relay = subprocess.Popen(
                ["ip", "netns", "exec", hub, str(MESH), "serve", "39011"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processes.append(relay)
            time.sleep(0.1)
            peers = []
            for name, identity, peer_key in (
                (peer_a, files[0], keys[1]), (peer_b, files[1], keys[0])
            ):
                process = subprocess.Popen(
                    ["ip", "netns", "exec", name, str(MESH), "tunnel",
                     identity, peer_key, "198.18.1.1", "39011"],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=True)
                processes.append(process)
                process.stdin.write("mesh-test-password\n")
                process.stdin.close()
                process.stdin = None
                peers.append(process)
            interfaces = [wait_line(peer, "virtual interface: ", 10) for peer in peers]
            for name, interface, local, remote in (
                (peer_a, interfaces[0], "10.250.0.1", "10.250.0.2"),
                (peer_b, interfaces[1], "10.250.0.2", "10.250.0.1"),
            ):
                run("ip", "-n", name, "addr", "add", local + "/32",
                    "dev", interface)
                run("ip", "-n", name, "link", "set", interface, "up")
                run("ip", "-n", name, "route", "add", remote + "/32",
                    "dev", interface, "src", local)
            for peer in peers:
                print("connected to " +
                      wait_line(peer, "authenticated UDP path: ", 15))
            result = subprocess.run(
                ["ip", "netns", "exec", peer_a, "ping", "-c", "2", "-W", "5",
                 "10.250.0.2"],
                text=True, capture_output=True, timeout=20)
            print(result.stdout, end="")
            if result.returncode != 0:
                raise RuntimeError("mesh TUN ping failed: " + result.stderr)
            if "--restart" in sys.argv:
                peers[1].terminate()
                peers[1].wait(timeout=5)
                replacement = subprocess.Popen(
                    ["ip", "netns", "exec", peer_b, str(MESH), "tunnel",
                     files[1], keys[0], "198.18.1.1", "39011"],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=True)
                processes.append(replacement)
                replacement.stdin.write("mesh-test-password\n")
                replacement.stdin.close()
                replacement.stdin = None
                new_interface = wait_line(replacement, "virtual interface: ", 10)
                run("ip", "-n", peer_b, "addr", "add", "10.250.0.2/32",
                    "dev", new_interface)
                run("ip", "-n", peer_b, "link", "set", new_interface, "up")
                run("ip", "-n", peer_b, "route", "add", "10.250.0.1/32",
                    "dev", new_interface, "src", "10.250.0.2")
                print("reconnected to " +
                      wait_line(peers[0], "authenticated UDP path: ", 40))
                print("reconnected to " +
                      wait_line(replacement, "authenticated UDP path: ", 10))
                result = subprocess.run(
                    ["ip", "netns", "exec", peer_a, "ping", "-c", "2", "-W", "5",
                     "10.250.0.2"],
                    text=True, capture_output=True, timeout=20)
                print(result.stdout, end="")
                if result.returncode != 0:
                    raise RuntimeError("mesh reconnect ping failed: " + result.stderr)
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
        for process in processes:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            errors = process.stderr.read() if process.stderr else ""
            if errors:
                print(errors, file=sys.stderr, end="")
        for name in reversed(namespaces):
            subprocess.run(["ip", "netns", "del", name], check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()

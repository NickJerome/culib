"""Run two peer processes through separate port-preserving Linux NAT namespaces.

Usage (as root, after building example_net_punch_pair):
    python3 tests/net_punch_nat.py

The rules preserve each peer's UDP source port across destinations and accept
inbound UDP only after a matching outbound flow. This models a NAT that supports
hole punching; generic MASQUERADE may assign a different port per destination.
"""

import os
import pathlib
import subprocess
import sys
import time


ROOT = pathlib.Path(__file__).resolve().parents[1]
PAIR = ROOT / "build/linux/x86_64/debug/example_net_punch_pair"
STUN = ROOT / "tests/fixtures/stun_echo.py"


def run(*command):
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)


def main():
    if os.geteuid() != 0:
        raise RuntimeError("run this integration test as root")
    if not PAIR.is_file():
        raise RuntimeError("build example_net_punch_pair for Linux first")

    suffix = str(os.getpid())
    hub, nat_a, nat_b, peer_a, peer_b = [
        "culib-" + name + "-" + suffix
        for name in ("hub", "na", "nb", "pa", "pb")
    ]
    namespaces = []
    processes = []
    try:
        for name in (hub, nat_a, nat_b, peer_a, peer_b):
            run("ip", "netns", "add", name)
            namespaces.append(name)

        for name in namespaces:
            run("ip", "-n", name, "link", "set", "lo", "up")
        for router, external, internal, peer, hub_if, router_if, inside_if, peer_if, port in (
            (nat_a, "198.18.1.2/30", "10.101.0.1/24", peer_a,
             "ha", "na", "ia", "ea", 44001),
            (nat_b, "198.18.2.2/30", "10.102.0.1/24", peer_b,
             "hb", "nb", "ib", "eb", 44002),
        ):
            run("ip", "-n", hub, "link", "add", hub_if, "type", "veth",
                "peer", "name", router_if)
            run("ip", "-n", hub, "link", "set", router_if, "netns", router)
            run("ip", "-n", router, "link", "add", inside_if, "type", "veth",
                "peer", "name", peer_if)
            run("ip", "-n", router, "link", "set", peer_if, "netns", peer)
            run("ip", "-n", hub, "addr", "add",
                "198.18.1.1/30" if router == nat_a else "198.18.2.1/30",
                "dev", hub_if)
            run("ip", "-n", router, "addr", "add", external, "dev", router_if)
            run("ip", "-n", router, "addr", "add", internal, "dev", inside_if)
            run("ip", "-n", peer, "addr", "add",
                "10.101.0.2/24" if router == nat_a else "10.102.0.2/24",
                "dev", peer_if)
            for namespace, interface in ((hub, hub_if), (router, router_if),
                                         (router, inside_if), (peer, peer_if)):
                run("ip", "-n", namespace, "link", "set", interface, "up")
            run("ip", "-n", router, "route", "add", "default", "via",
                "198.18.1.1" if router == nat_a else "198.18.2.1")
            run("ip", "-n", peer, "route", "add", "default", "via",
                "10.101.0.1" if router == nat_a else "10.102.0.1")
            run("ip", "netns", "exec", router, "sysctl", "-q", "-w",
                "net.ipv4.ip_forward=1")
            run("ip", "netns", "exec", router, "iptables", "-t", "nat",
                "-A", "POSTROUTING", "-o", router_if, "-j", "MASQUERADE")
            public_ip = external.split("/")[0]
            private_ip = "10.101.0.2" if router == nat_a else "10.102.0.2"
            run("ip", "netns", "exec", router, "iptables", "-t", "nat",
                "-I", "POSTROUTING", "1", "-s", private_ip, "-p", "udp",
                "--sport", str(port), "-j", "SNAT", "--to-source",
                public_ip + ":" + str(port))
            run("ip", "netns", "exec", router, "iptables", "-t", "nat",
                "-A", "PREROUTING", "-i", router_if, "-p", "udp",
                "--dport", str(port), "-j", "DNAT", "--to-destination",
                private_ip + ":" + str(port))
            run("ip", "netns", "exec", router, "iptables", "-I", "FORWARD",
                "1", "-i", router_if, "-o", inside_if, "-p", "udp",
                "-m", "conntrack", "!", "--ctstate", "ESTABLISHED,RELATED",
                "-j", "DROP")
        run("ip", "netns", "exec", hub, "sysctl", "-q", "-w",
            "net.ipv4.ip_forward=1")
        for namespace in (peer_a, peer_b):
            run("ip", "netns", "exec", namespace, "ping", "-c", "1",
                "-W", "1", "198.18.1.1")

        stun = subprocess.Popen(
            ["ip", "netns", "exec", hub, sys.executable, str(STUN)],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        processes.append(stun)
        relay = subprocess.Popen(
            ["ip", "netns", "exec", hub, str(PAIR), "relay", "39011"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        processes.append(relay)
        time.sleep(0.2)
        if stun.poll() is not None:
            raise RuntimeError("STUN responder failed to start: " + stun.stderr.read())
        peers = []
        for namespace, port in ((peer_a, 44001), (peer_b, 44002)):
            process = subprocess.Popen(
                ["ip", "netns", "exec", namespace, str(PAIR), "peer",
                 "198.18.1.1", "39011", "198.18.1.1", "3478", str(port)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processes.append(process)
            peers.append(process)
        failed = False
        expected = ("UDP data exchanged with: 198.18.2.2:44002",
                    "UDP data exchanged with: 198.18.1.2:44001", "offers relayed")
        for label, process, marker in zip(("peer A", "peer B", "relay"),
                                          peers + [relay], expected):
            output, errors = process.communicate(timeout=20)
            print(label + " (exit " + str(process.returncode) + "): " + output,
                  flush=True)
            if errors:
                print(label + ": " + errors, file=sys.stderr, flush=True)
            if process.returncode != 0 or marker not in output:
                failed = True
        if failed:
            raise RuntimeError("a NAT peer or relay failed")
        if stun.poll() is not None:
            raise RuntimeError("STUN responder exited early")
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
        if len(processes) > 0 and processes[0].stderr:
            diagnostics = processes[0].stderr.read()
            if diagnostics:
                print(diagnostics, file=sys.stderr, end="")
        for name in reversed(namespaces):
            subprocess.run(["ip", "netns", "del", name], check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()

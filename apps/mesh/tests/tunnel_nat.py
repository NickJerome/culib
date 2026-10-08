"""Root-only mesh test through two NATs.

Build apps/mesh in Linux debug mode first. This test needs iptables, IP
forwarding and network namespaces. The default tunnel test also needs
/dev/net/tun and changes one NAT mapping; --probe tests encrypted peer data
without creating a virtual interface. --non-preserving maps both peers to
public UDP ports different from their local ports. It cleans up its namespaces.
"""

import os
import pathlib
import subprocess
import sys
import tempfile
import time

from tunnel_ns import MESH, ping_pair, run, wait_line


ROOT = pathlib.Path(__file__).resolve().parents[3]
STUN = ROOT / "tests/fixtures/stun_echo.py"


def main():
    options = sys.argv[1:]
    if len(set(options)) != len(options) or any(
            option not in ("--probe", "--non-preserving") for option in options):
        raise RuntimeError("usage: tunnel_nat.py [--probe] [--non-preserving]")
    probe = "--probe" in options
    non_preserving = "--non-preserving" in options
    public_a = 54001 if non_preserving else 44001
    public_b = 54002 if non_preserving else 44002
    changed_a = 55001 if non_preserving else 45001
    if os.geteuid() != 0 or not MESH.is_file():
        raise RuntimeError("run as root after building apps/mesh in Linux debug mode")
    suffix = str(os.getpid())
    hub, nat_a, nat_b, peer_a, peer_b = [
        "mesh-" + name + suffix for name in ("h", "na", "nb", "pa", "pb")
    ]
    namespaces = []
    processes = []
    try:
        for name in (hub, nat_a, nat_b, peer_a, peer_b):
            run("ip", "netns", "add", name)
            namespaces.append(name)
            run("ip", "-n", name, "link", "set", "lo", "up")
        for router, peer, hub_if, router_if, inside_if, peer_if, hub_ip, public_ip, private_ip, port, mapped_port in (
            (nat_a, peer_a, "ha", "na", "ia", "ea", "198.18.1.1",
             "198.18.1.2", "10.101.0.2", 44001, public_a),
            (nat_b, peer_b, "hb", "nb", "ib", "eb", "198.18.2.1",
             "198.18.2.2", "10.102.0.2", 44002, public_b),
        ):
            run("ip", "-n", hub, "link", "add", hub_if, "type", "veth",
                "peer", "name", router_if)
            run("ip", "-n", hub, "link", "set", router_if, "netns", router)
            run("ip", "-n", router, "link", "add", inside_if, "type", "veth",
                "peer", "name", peer_if)
            run("ip", "-n", router, "link", "set", peer_if, "netns", peer)
            for name, address, interface in (
                (hub, hub_ip + "/30", hub_if),
                (router, public_ip + "/30", router_if),
                (router, "10.101.0.1/24" if router == nat_a else "10.102.0.1/24", inside_if),
                (peer, private_ip + "/24", peer_if),
            ):
                run("ip", "-n", name, "addr", "add", address, "dev", interface)
                run("ip", "-n", name, "link", "set", interface, "up")
            run("ip", "-n", router, "route", "add", "default", "via", hub_ip)
            run("ip", "-n", peer, "route", "add", "default", "via",
                "10.101.0.1" if router == nat_a else "10.102.0.1")
            run("ip", "netns", "exec", router, "sysctl", "-q", "-w",
                "net.ipv4.ip_forward=1")
            run("ip", "netns", "exec", router, "sysctl", "-q", "-w",
                "net.netfilter.nf_conntrack_udp_timeout=2")
            run("ip", "netns", "exec", router, "sysctl", "-q", "-w",
                "net.netfilter.nf_conntrack_udp_timeout_stream=2")
            run("ip", "netns", "exec", router, "iptables", "-t", "nat",
                "-A", "POSTROUTING", "-o", router_if, "-j", "MASQUERADE")
            run("ip", "netns", "exec", router, "iptables", "-t", "nat",
                "-I", "POSTROUTING", "1", "-s", private_ip, "-p", "udp",
                "--sport", str(port), "-j", "SNAT", "--to-source",
                public_ip + ":" + str(mapped_port))
            run("ip", "netns", "exec", router, "iptables", "-t", "nat",
                "-A", "PREROUTING", "-i", router_if, "-p", "udp",
                "--dport", str(mapped_port), "-j", "DNAT", "--to-destination",
                private_ip + ":" + str(port))
            run("ip", "netns", "exec", router, "iptables", "-I", "FORWARD",
                "1", "-i", router_if, "-o", inside_if, "-p", "udp", "-m",
                "conntrack", "!", "--ctstate", "ESTABLISHED,RELATED", "-j", "DROP")
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
            stun = subprocess.Popen(
                ["ip", "netns", "exec", hub, sys.executable, str(STUN),
                 "--bind", "198.18.1.1", "--port", "3478"],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            processes.append(stun)
            relay = subprocess.Popen(
                ["ip", "netns", "exec", hub, str(MESH), "serve", "39011"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processes.append(relay)
            time.sleep(0.2)
            peers = []
            for name, filename, peer_key, port, local, remote in (
                (peer_a, files[0], keys[1], 44001, "10.250.0.1", "10.250.0.2"),
                (peer_b, files[1], keys[0], 44002, "10.250.0.2", "10.250.0.1"),
            ):
                process = subprocess.Popen(
                    ["ip", "netns", "exec", name, str(MESH),
                     "peer" if probe else "tunnel",
                     filename, peer_key, "198.18.1.1", "39011",
                     "198.18.1.1", "3478", str(port)],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=True)
                processes.append(process)
                process.stdin.write("mesh-test-password\n")
                process.stdin.close()
                process.stdin = None
                peers.append(process)
                if probe:
                    continue
                interface = wait_line(process, "virtual interface: ", 10)
                run("ip", "-n", name, "addr", "add", local + "/32",
                    "dev", interface)
                run("ip", "-n", name, "link", "set", interface, "up")
                run("ip", "-n", name, "route", "add", remote + "/32",
                    "dev", interface, "src", local)
            if probe:
                for process, expected_port in zip(peers, (public_b, public_a)):
                    output, errors = process.communicate(timeout=35)
                    print(output, end="", flush=True)
                    if process.returncode != 0 or \
                            "authenticated UDP path:" not in output or \
                            "encrypted UDP probe: ok" not in output or \
                            ":" + str(expected_port) not in output:
                        raise RuntimeError("NAT encrypted peer probe failed: " + errors)
                print("two-NAT encrypted peer probe passed")
                return
            first = [wait_line(peer, "authenticated UDP path: ", 20) for peer in peers]
            print("initial paths:", first)
            if ":" + str(public_b) not in first[0] or ":" + str(public_a) not in first[1]:
                raise RuntimeError("initial NAT ports do not match the mapping")
            ping_pair(peer_a, peer_b, "initial NAT")

            # Change only NAT A's public UDP port. Existing conntrack state
            # expires after two seconds; both peers must discover a new session.
            prefix = ("ip", "netns", "exec", nat_a, "iptables", "-t", "nat")
            run(*prefix, "-D", "POSTROUTING", "-s", "10.101.0.2", "-p", "udp",
                "--sport", "44001", "-j", "SNAT", "--to-source",
                "198.18.1.2:" + str(public_a))
            run(*prefix, "-I", "POSTROUTING", "1", "-s", "10.101.0.2", "-p", "udp",
                "--sport", "44001", "-j", "SNAT", "--to-source",
                "198.18.1.2:" + str(changed_a))
            run(*prefix, "-D", "PREROUTING", "-i", "na", "-p", "udp",
                "--dport", str(public_a), "-j", "DNAT", "--to-destination",
                "10.101.0.2:44001")
            run(*prefix, "-A", "PREROUTING", "-i", "na", "-p", "udp",
                "--dport", str(changed_a), "-j", "DNAT", "--to-destination",
                "10.101.0.2:44001")
            second = [wait_line(peer, "authenticated UDP path: ", 50)
                      for peer in peers]
            print("reconnected paths:", second)
            if ":" + str(changed_a) not in second[1]:
                raise RuntimeError("peer did not learn NAT A's new public port")
            ping_pair(peer_a, peer_b, "changed NAT mapping")
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
            errors = (process.stderr.read() if process.stderr and
                      not process.stderr.closed else "")
            if errors:
                print(errors, file=sys.stderr, end="")
        for name in reversed(namespaces):
            subprocess.run(["ip", "netns", "del", name], check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()

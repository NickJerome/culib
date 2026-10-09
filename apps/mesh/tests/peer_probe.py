"""Exercise the mesh peer command without a virtual interface or privileges."""

import pathlib
import socket
import subprocess
import sys
import tempfile
import time


ROOT = pathlib.Path(__file__).resolve().parents[3]
PASSWORD = "mesh-probe-test-password\n"


def program_for(mode):
    platform = ("windows" if sys.platform == "win32" else
                "macosx" if sys.platform == "darwin" else "linux")
    programs = [path for suffix in ("mesh", "mesh.exe")
                for path in (ROOT / "build").glob(
                    platform + "/*/" + mode + "/" + suffix)
                if path.is_file()]
    if len(programs) == 1:
        return programs[0]
    raise RuntimeError("build exactly one mesh binary for " + mode)


def run(*command, **kwargs):
    completed = subprocess.run(command, text=True, capture_output=True,
                               timeout=25, **kwargs)
    if completed.returncode != 0:
        raise RuntimeError("command failed: " + " ".join(map(str, command)) +
                           "\n" + completed.stdout + completed.stderr)
    return completed.stdout.strip()


def finish(process, timeout=5):
    if process.poll() is None:
        process.terminate()
    try:
        return process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        return process.communicate()


def probe_pair(program, files, keys, port, stun_args, stun_unavailable,
               processes):
    peers = []
    for filename, peer_key in ((files[0], keys[1]), (files[1], keys[0])):
        peer = subprocess.Popen(
            [str(program), "peer", filename, peer_key,
             "127.0.0.1", str(port)] + stun_args,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
        processes.append(peer)
        peers.append(peer)
        peer.stdin.write(PASSWORD)
        peer.stdin.close()
        peer.stdin = None
    for peer in peers:
        output, errors = peer.communicate(timeout=30)
        print(output, end="", flush=True)
        if errors:
            print(errors, end="", file=sys.stderr, flush=True)
        if stun_unavailable and not errors:
            raise RuntimeError("unavailable STUN was not reported")
        if peer.returncode != 0 or \
                "authenticated UDP path:" not in output or \
                "encrypted UDP probe: ok" not in output:
            raise RuntimeError("encrypted mesh probe failed: " + errors)


def main():
    options = sys.argv[2:]
    if len(sys.argv) < 2 or sys.argv[1] not in ("debug", "release") or \
            len(options) != len(set(options)) or \
            set(options) - {"--stun-unavailable", "--serve"}:
        raise RuntimeError(
            "usage: peer_probe.py debug|release [--stun-unavailable] [--serve]")
    stun_unavailable = "--stun-unavailable" in options
    persistent = "--serve" in options
    program = program_for(sys.argv[1])
    with tempfile.TemporaryDirectory() as temporary, \
            socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as silent_stun:
        silent_stun.bind(("127.0.0.1", 0))
        stun_args = (["127.0.0.1", str(silent_stun.getsockname()[1])]
                     if stun_unavailable else [])
        files = [str(pathlib.Path(temporary) / name)
                 for name in ("a.key", "b.key")]
        keys = [run(str(program), "identity", "create", name, input=PASSWORD)
                for name in files]
        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            port = reserved.getsockname()[1]

        processes = []
        try:
            relay = subprocess.Popen([str(program),
                                      "serve" if persistent else "relay",
                                      str(port)],
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True)
            processes.append(relay)
            time.sleep(0.2)
            for _ in range(2 if persistent else 1):
                probe_pair(program, files, keys, port, stun_args,
                           stun_unavailable, processes)
                if persistent and relay.poll() is not None:
                    raise RuntimeError("mesh serve exited after one pair")
            if not persistent:
                output, errors = relay.communicate(timeout=5)
                if relay.returncode != 0:
                    raise RuntimeError("mesh relay failed: " + output + errors)
            elif sys.platform != "win32":
                output, errors = finish(relay, timeout=8)
                if relay.returncode != 0:
                    raise RuntimeError("mesh serve did not stop cleanly: " +
                                       output + errors)
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    finish(process)
    print(("two sequential encrypted mesh probes passed" if persistent else
           "two-process encrypted mesh probe passed") +
          (" with unavailable STUN" if stun_unavailable else ""))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print("error: " + str(error), file=sys.stderr)
        raise

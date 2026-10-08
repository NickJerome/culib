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


def finish(process):
    if process.poll() is None:
        process.terminate()
    try:
        return process.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        return process.communicate()


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("debug", "release"):
        raise RuntimeError("usage: peer_probe.py debug|release")
    program = program_for(sys.argv[1])
    with tempfile.TemporaryDirectory() as temporary:
        files = [str(pathlib.Path(temporary) / name)
                 for name in ("a.key", "b.key")]
        keys = [run(str(program), "identity", "create", name, input=PASSWORD)
                for name in files]
        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            port = reserved.getsockname()[1]

        processes = []
        try:
            relay = subprocess.Popen([str(program), "relay", str(port)],
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True)
            processes.append(relay)
            time.sleep(0.2)
            peers = []
            for filename, peer_key in ((files[0], keys[1]), (files[1], keys[0])):
                peer = subprocess.Popen(
                    [str(program), "peer", filename, peer_key,
                     "127.0.0.1", str(port)],
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
                if peer.returncode != 0 or \
                        "authenticated UDP path:" not in output or \
                        "encrypted UDP probe: ok" not in output:
                    raise RuntimeError("encrypted mesh probe failed: " + errors)
            output, errors = relay.communicate(timeout=5)
            if relay.returncode != 0:
                raise RuntimeError("mesh relay failed: " + output + errors)
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    finish(process)
    print("two-process encrypted mesh probe passed")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print("error: " + str(error), file=sys.stderr)
        raise

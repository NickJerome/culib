"""Minimal STUN Binding responder for controlled mesh tests."""

import argparse
import socket
import struct


MAGIC = 0x2112A442
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--bind", default="198.18.1.1",
                    help="numeric IPv4 address to listen on")
parser.add_argument("--port", type=int, default=3478)
options = parser.parse_args()
if not 1 <= options.port <= 65535:
    parser.error("--port must be between 1 and 65535")
server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
server.bind((options.bind, options.port))

while True:
    request, peer = server.recvfrom(2048)
    if len(request) != 20:
        continue
    message_type, length, cookie = struct.unpack_from("!HHI", request)
    if message_type != 0x0001 or length != 0 or cookie != MAGIC:
        continue
    transaction = request[8:20]
    masked_port = peer[1] ^ (MAGIC >> 16)
    masked_ip = bytes(left ^ right for left, right in zip(
        socket.inet_aton(peer[0]), struct.pack("!I", MAGIC)))
    attribute = struct.pack("!HHBBH4s", 0x0020, 8, 0, 1,
                            masked_port, masked_ip)
    response = struct.pack("!HHI", 0x0101, len(attribute), MAGIC)
    server.sendto(response + transaction + attribute, peer)

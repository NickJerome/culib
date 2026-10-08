"""Minimal local STUN Binding responder for the isolated NAT integration test."""

import socket
import struct


MAGIC = 0x2112A442
server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
server.bind(("198.18.1.1", 3478))

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

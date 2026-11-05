#!/usr/bin/env python3
"""Exercise actual bidirectional UDP delay, random loss and bandwidth shaping."""
import argparse
from contextlib import contextmanager
import json
import socket
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--binary", required=True)
args = parser.parse_args()


@contextmanager
def proxy(*options):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        server.bind(("127.0.0.1", 0))
        client.bind(("127.0.0.1", 0))
        server.settimeout(1)
        client.settimeout(1)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        process = subprocess.Popen([args.binary, "--listen-port", str(port), "--target-port", str(server.getsockname()[1]), "--duration-ms", "2500", *options], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            ready = json.loads(process.stdout.readline())
            assert ready["listening"] == port
            yield server, client, ("127.0.0.1", port)
        finally:
            process.terminate()
            process.communicate(timeout=5)


with proxy("--delay-ms", "15") as (server, client, address):
    started = time.monotonic()
    client.sendto(b"roundtrip", address)
    data, sender = server.recvfrom(1200)
    assert data == b"roundtrip"
    server.sendto(data, sender)
    assert client.recvfrom(1200)[0] == data
    elapsed = time.monotonic() - started
    assert 0.024 <= elapsed < 1, elapsed

with proxy("--loss", "1") as (server, client, address):
    server.settimeout(0.15)
    client.sendto(b"must disappear", address)
    try:
        server.recvfrom(1200)
        raise AssertionError("100% loss forwarded a packet")
    except socket.timeout:
        pass

with proxy("--mbps", "1") as (server, client, address):
    started = time.monotonic()
    for index in range(10):
        client.sendto(bytes([index]) * 1200, address)
    received = [server.recvfrom(1200)[0][0] for _ in range(10)]
    elapsed = time.monotonic() - started
    assert received == list(range(10)), received
    assert 0.06 <= elapsed < 1, elapsed

with proxy("--good-bad", "1", "--bad-good", "0", "--bad-loss", "1") as (server, client, address):
    server.settimeout(0.15)
    client.sendto(b"bad state", address)
    try:
        server.recvfrom(1200)
        raise AssertionError("Gilbert-Elliott bad state did not drop")
    except socket.timeout:
        pass

print("Actual UDP bidirectional delay, total loss, burst state and bandwidth-cap checks passed.")

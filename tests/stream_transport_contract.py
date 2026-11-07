"""Binary streaming, origin binding, latest-frame policy and tick independence."""
import json
import socket
import struct
import subprocess
import sys
import time
from urllib.parse import urlsplit


def connect(info, origin='http://127.0.0.1:9000'):
    address = urlsplit(info['url'])
    sock = socket.create_connection((address.hostname, address.port), timeout=3)
    request = (f'GET {address.path} HTTP/1.1\r\nHost: {address.netloc}\r\n'
               f'Origin: {origin}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
               'Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n')
    sock.sendall(request.encode())
    header = b''
    while not header.endswith(b'\r\n\r\n'):
        byte = sock.recv(1)
        if not byte:
            sock.close()
            return None
        header += byte
    assert b'101 Switching Protocols' in header
    assert b's3pPLMBiTxaQ9kYGzzhZRbK+xOo=' in header
    return sock


def exact(sock, length):
    output = bytearray()
    while len(output) < length:
        chunk = sock.recv(length-len(output))
        assert chunk, 'premature disconnect'
        output.extend(chunk)
    return output


def receive(sock):
    header = exact(sock, 2)
    length = header[1] & 127
    if length == 126:
        length = struct.unpack('!H', exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack('!Q', exact(sock, 8))[0]
    data = exact(sock, length)
    if header[0] & 15 == 8:
        assert data[2:] == b'STREAM_CLOSED'
        return None
    assert data[:4] == b'RWS'+bytes([stream_version])
    sequence = struct.unpack_from('<Q', data, 8)[0]
    header = 80 if stream_version == 2 else 40
    assert len(data) == header + first['maxBytes']
    if stream_version == 2:
        assert struct.unpack_from('<QQQdBB',data,40) == (99,9007199254740992+sequence,17,0.5,1,1)
    assert data[header:] == bytes([sequence & 255]) * first['maxBytes']
    return sequence


def ack(sock):
    sock.sendall(b'\x82\x81\x11\x22\x33\x44\x10')


process = subprocess.Popen(sys.argv[1:], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
try:
    first, second = json.loads(process.stdout.readline())
    stream_version = first.get('layoutVersion', 1)
    assert stream_version == (2 if sys.argv[-1]=='timing' else 1)
    assert connect(first, 'http://wrong.invalid') is None
    a, b = connect(first), connect(second)
    assert a is not None and b is not None
    assert connect(first) is None, 'tickets must be single use'
    seq_a, seq_b = receive(a), receive(b)
    # One consumer stops acknowledging while the other receives continuously.
    until = time.monotonic() + 0.4
    received = 0
    while time.monotonic() < until:
        ack(b)
        next_b = receive(b)
        assert next_b > seq_b
        seq_b = next_b
        received += 1
    if sys.argv[-1]=='video':assert received >= 4, ('720p transport stalled', received)
    ack(a)
    next_a = receive(a)
    assert next_a > seq_a + 10, (seq_a, next_a)
    a.close()
    while True:
        ack(b)
        sequence = receive(b)
        if sequence is None:
            break
        assert sequence > seq_b
        seq_b = sequence
    b.close()
    process.stdin.write('done\n')
    process.stdin.flush()
    process.stdin.close()
    assert process.wait(timeout=5) == 0
    diagnostics = json.loads(process.stdout.readline())
    assert diagnostics['streams'] == []
    assert 0 < diagnostics['frameLatencyMs'] < 30000
    assert diagnostics['transport'] == {'connections': 2, 'handshakeFailures': 2,
                                        'expiredTickets': 0, 'lastHandshakeError': 'TICKET_USED'}
    print('Binary transport, authentication, independent consumers and producer close passed')
finally:
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=5)

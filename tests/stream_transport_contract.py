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
    shared = first.get('sharedVideo') and sock is a
    assert data[5] == int(video and sequence % 3 == 1) | (2 if shared else 0), 'Video discontinuity flag leaked or was lost'
    header = 80 if stream_version == 2 else 40
    assert len(data) == header + (4 if shared else first['maxBytes'])
    if shared:
        assert struct.unpack_from('<Q', data, 32)[0] == first['maxBytes']
        slot = struct.unpack_from('<I', data, 40)[0]
        assert slot == shared_slots[0], 'Shared video reused an unacknowledged slot'
        shared_slots[0] ^= 1
        return sequence
    if stream_version == 2:
        assert struct.unpack_from('<QQQdBB',data,40) == (99,9007199254740992+sequence,17,0.5,1,1)
    assert data[header:] == bytes([sequence & 255]) * first['maxBytes']
    return sequence


def ack(sock):
    sock.sendall(b'\x82\x81\x11\x22\x33\x44\x10')


process = subprocess.Popen(sys.argv[1:], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
jitter = sys.argv[-1] == 'jitter'
burst = sys.argv[-1] == 'burst'
video = len(sys.argv) > 2 and sys.argv[2].startswith('video')
shared_slots = [0]
try:
    first, second = json.loads(process.stdout.readline())
    stream_version = first.get('layoutVersion', 1)
    assert stream_version == (2 if sys.argv[-1]=='timing' else 1)
    assert connect(first, 'http://wrong.invalid') is None
    a, b = connect(first), connect(second)
    assert a is not None and b is not None
    assert connect(first) is None, 'tickets must be single use'
    seq_a, seq_b = receive(a), receive(b)
    if burst:
        ack(a); ack(b)
        for first_sequence in range(2, 98, 3):
            process.stdin.write(str(first_sequence)+'\n'); process.stdin.flush()
            assert process.stdout.readline().strip() == 'published'
            for sequence in range(first_sequence, first_sequence + 3):
                assert receive(a) == sequence, 'An available send slot was ignored during a capture burst'
                ack(a)
                assert receive(b) == sequence
                ack(b)
            seq_b = first_sequence + 2
        process.stdin.write('close\n'); process.stdin.flush()
    elif jitter:
        def publish(sequence):
            ack(b)
            process.stdin.write(str(sequence)+'\n'); process.stdin.flush()
            assert process.stdout.readline().strip() == 'published'
            # This consumer proves that transport has consumed each packet.
            assert receive(b) == sequence
        for sequence in (2, 3): publish(sequence)
        ack(a)
        assert receive(a) == (2 if video else 3), 'Brief ACK jitter discarded an available frame'
        if video:
            ack(a); assert receive(a) == 3
        for sequence in (4, 5, 6, 7): publish(sequence)
        ack(a)
        if video:
            assert receive(a) == 4, 'The second in-flight frame must stay ordered'
            ack(a)
        next_a = receive(a)
        if video and not first.get('sharedVideo') and next_a == 5:
            # Large frames may fill the socket before the third send can begin.
            ack(a); next_a = receive(a)
        assert next_a == (6 if video else 7), 'A stalled consumer retained an unbounded backlog'
        if video:
            ack(a); assert receive(a) == 7
        seq_b = 7
        process.stdin.write('close\n'); process.stdin.flush()
    else:
        # One consumer stops acknowledging while the other receives continuously.
        until = time.monotonic() + 0.4
        received = 0
        while time.monotonic() < until:
            ack(b)
            next_b = receive(b)
            assert next_b > seq_b
            seq_b = next_b
            received += 1
        if video:assert received >= 4, ('720p transport stalled', received)
        ack(a)
        next_a = receive(a)
        if video:
            assert next_a > seq_a
            ack(a); next_a = receive(a)
            if not first.get('sharedVideo'):
                ack(a); next_a = receive(a)
        assert next_a > seq_a + 10, (seq_a, next_a)
    a.close()
    ack_pending = not burst  # Burst frames were already acknowledged inside the loop.
    while True:
        if ack_pending:
            ack(b)
        sequence = receive(b)
        if sequence is None:
            break
        assert sequence > seq_b
        seq_b = sequence
        ack_pending = True
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

"""A bounded set of partial socket frames must not starve native video consumers."""
import json
import socket
import struct
import subprocess
import sys
from urllib.parse import urlsplit


def exact(sock, count):
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        assert chunk, 'Consumer disconnected before completing its frame'
        data.extend(chunk)
    return data


def connect(info, slow=False):
    url = urlsplit(info['url'])
    sock = socket.socket()
    if slow:
        # Set before connect so TCP cannot negotiate a large receive window.
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    sock.settimeout(3)
    sock.connect((url.hostname, url.port))
    sock.sendall((f'GET {url.path} HTTP/1.1\r\nHost: {url.netloc}\r\n'
                  'Origin: http://127.0.0.1:9000\r\nUpgrade: websocket\r\n'
                  'Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n'
                  'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n').encode())
    header = bytearray()
    while not header.endswith(b'\r\n\r\n'):
        header.extend(exact(sock, 1))
    assert b'101 Switching Protocols' in header
    return sock


def frame_header(sock):
    header = exact(sock, 2)
    length = header[1] & 127
    if length == 126:
        length = struct.unpack('!H', exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack('!Q', exact(sock, 8))[0]
    assert header[0] == 0x82
    data = exact(sock, 40)
    assert data[:4] == b'RWS\x01'
    return length, struct.unpack_from('<Q', data, 8)[0], data[5]


process = subprocess.Popen([sys.argv[1], 'video-pressure'], stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE, text=True)
clients = []
try:
    tickets = json.loads(process.stdout.readline())
    assert len(tickets) == 8 and tickets[0]['sharedVideo']
    assert tickets[0]['maxBytes'] == 3840 * 2160 * 4, 'Pressure frame must exceed the socket send buffer even when doubled'
    fast = connect(tickets[0])
    clients.append(fast)

    def receive_fast(expected):
        try:
            length, sequence, flags = frame_header(fast)
        except TimeoutError as error:
            raise AssertionError(f'Native consumer starved at frame {expected} while slow sockets retained partial frames') from error
        assert length == 44 and flags & 2 and sequence == expected
        exact(fast, 4)
        fast.sendall(b'\x82\x81\x11\x22\x33\x44\x10')

    def publish(sequence):
        process.stdin.write(str(sequence) + '\n')
        process.stdin.flush()
        assert process.stdout.readline().strip() == 'published'
        receive_fast(sequence)

    receive_fast(1)
    for index, ticket in enumerate(tickets[1:]):
        slow = connect(ticket, True)
        clients.append(slow)
        if index < 6:
            length, sequence, flags = frame_header(slow)
            assert sequence == index + 1 and length == ticket['maxBytes'] + 40 and not flags & 2
            # Leave the full pixel body unread, pinning a different wire message.
        # The seventh socket may wait for admission instead of exhausting the byte budget.
        publish(index + 2)
    for sequence in range(9, 34):
        publish(sequence)
    assert process.poll() is None
    clients[1].close()
    length, sequence, flags = frame_header(clients[-1])
    assert sequence >= 32 and length == tickets[-1]['maxBytes'] + 40 and not flags & 2, ('Deferred socket did not resume with a recent frame', sequence, length, flags)
    for client in clients:
        client.close()
    clients.clear()
    process.stdin.write('close\ndone\n')
    process.stdin.flush()
    assert process.wait(timeout=5) == 0
    print('Seven staggered socket consumers cannot starve native video delivery')
finally:
    for client in clients:
        client.close()
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=5)

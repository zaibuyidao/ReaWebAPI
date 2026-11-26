"""External control protocol and coexistence, over real loopback sockets."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlsplit


def exact(sock, size):
    data = bytearray()
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise EOFError('Connection closed')
        data += part
    return bytes(data)


class WebSocket:
    def __init__(self, url, origin='null'):
        address = urlsplit(url)
        self.sock = socket.create_connection((address.hostname, address.port), timeout=3)
        self.sock.sendall((f'GET {address.path or "/"} HTTP/1.1\r\nHost: {address.netloc}\r\n'
                           f'Origin: {origin}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                           'Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n').encode())
        header = b''
        while not header.endswith(b'\r\n\r\n'):
            header += exact(self.sock, 1)
        assert b'101 Switching Protocols' in header and b's3pPLMBiTxaQ9kYGzzhZRbK+xOo=' in header
        self.sequence = 0
        self.events = []

    def frame(self, payload, opcode=1, final=True):
        if isinstance(payload, str):
            payload = payload.encode()
        first = (128 if final else 0) | opcode
        size = len(payload)
        header = bytes([first, 128 | size]) if size < 126 else (
            bytes([first, 254]) + struct.pack('!H', size) if size <= 65535 else bytes([first, 255]) + struct.pack('!Q', size))
        mask = os.urandom(4)
        self.sock.sendall(header + mask + bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload)))

    def receive_frame(self):
        header = exact(self.sock, 2)
        size = header[1] & 127
        if size == 126:
            size = struct.unpack('!H', exact(self.sock, 2))[0]
        elif size == 127:
            size = struct.unpack('!Q', exact(self.sock, 8))[0]
        assert header[0] & 128 and not header[1] & 128
        return header[0] & 15, exact(self.sock, size)

    def receive(self):
        opcode, payload = self.receive_frame()
        if opcode == 8:
            raise EOFError('WebSocket close')
        assert opcode == 1, opcode
        return json.loads(payload)

    def request(self, method, params=None, fragmented=False):
        self.sequence += 1
        message = json.dumps(dict(type='request', id=self.sequence, method=method, params=params or {}))
        if fragmented:
            self.frame(message[:20], final=False)
            self.frame(message[20:], opcode=0)
        else:
            self.frame(message)
        return self.sequence

    def response(self, request):
        while True:
            message = self.receive()
            if message['type'] == 'event':
                self.events.append(message)
                continue
            assert message['type'] == 'response' and message['id'] == request, message
            return message

    def call(self, method, params=None, error=None, fragmented=False):
        result = self.response(self.request(method, params, fragmented))
        if error:
            assert result.get('error', {}).get('code') == error, result
            return result['error']
        assert 'error' not in result, result
        return result['result']

    def closed(self):
        try:
            while True:
                self.receive()
        except (EOFError, ConnectionResetError, ConnectionAbortedError):
            pass
        finally:
            self.sock.close()

    def close(self):
        self.sock.close()


def check_external_methods(client):
    caps = client.call('system.getCapabilities')
    assert {'api.call', 'api.batch'}.isdisjoint(caps['methods'])
    assert {'api', 'batchMethods'}.isdisjoint(caps)
    assert {'batchCalls', 'valueBytes'}.isdisjoint(caps['limits'])
    client.call('api.call', {'name': 'CountTracks', 'args': [0]}, error='UNKNOWN_METHOD')
    client.call('api.batch', {'calls': [{'method': 'CountTracks', 'args': [0]}]}, error='UNKNOWN_METHOD')
    assert client.call('system.getInfo', fragmented=True)['protocolVersion'] == 1
    return caps


def subscribe_projects(client, event='projects-state-changed'):
    return client.call('events.subscribe', {'event': event})['subscriptionId']


def project_snapshot(client, subscription, after_epoch=0, event_name='projects-state-changed'):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        client.call('system.getInfo')
        for event in reversed(client.events):
            if event.get('subscriptionId') != subscription:
                continue
            assert event['event'] == event_name
            data = event['data']
            if data['projectEpoch'] <= after_epoch:
                continue
            ids = [p['id'] for p in data['projects']]
            assert data['activeProject'] and data['activeProject'] in ids, data
            assert len(ids) == len(set(ids)) and data['revision'] >= 1
            return data
        time.sleep(0.025)
    raise AssertionError(f'Missing {event_name} snapshot after epoch {after_epoch}')


def selection_snapshot(client, subscription, event_name, count=1):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        client.call('system.getInfo')
        for event in reversed(client.events):
            if event.get('subscriptionId') != subscription:
                continue
            assert event['event'] == event_name
            data = event['data']
            if data['count'] == count:
                assert data['projectEpoch'] >= 1 and data['revision'] >= 1
                assert data['invalidated'] is True and data['available'] is True
                return data
        time.sleep(0.025)
    raise AssertionError(f'Missing {event_name} selection snapshot for {count} tracks')


def web_event_snapshot(command, event_name):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        data = command('web', method='ReaWeb_Subscribe', args=[event_name])['result']
        if data is not None:
            return data
        time.sleep(0.025)
    raise AssertionError(f'Missing {event_name} WebUI snapshot')


def main():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix='reaweb-external-') as directory:
        process = subprocess.Popen([sys.argv[1], str(port), directory], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding='utf-8')
        clients = []
        try:
            config = json.loads(process.stdout.readline())
            url = f'ws://127.0.0.1:{config["port"]}'

            def command(cmd, expect=None, **params):
                process.stdin.write(json.dumps(dict(cmd=cmd, **params)) + '\n')
                process.stdin.flush()
                result = json.loads(process.stdout.readline())
                if expect:
                    assert result.get('error') == expect, result
                    return result
                assert 'error' not in result, result
                return result['result']

            def connect(token=None):
                client = WebSocket(url)
                clients.append(client)
                if token:
                    assert client.call('auth.authenticate', dict(token=token, protocolVersion=1)) == dict(authenticated=True, protocolVersion=1)
                return client

            a = connect()
            a.call('system.getInfo', error='AUTH_REQUIRED')
            a.call('auth.authenticate', dict(token='wrong', protocolVersion=1), error='AUTH_FAILED')
            a.closed()
            wrong_version = connect()
            wrong_version.call('auth.authenticate', dict(token=config['token'], protocolVersion=2), error='PROTOCOL_MISMATCH')
            wrong_version.closed()
            a, b = connect(config['token']), connect(config['token'])
            assert command('stats')['platforms'] == 0, 'External Client must not create a WebView'
            assert a.call('system.getInfo')['protocolVersion'] == 1
            runtime_query = {'service': 'runtime', 'method': 'getProjectSummary'}
            assert a.call('service.invoke', runtime_query) == {'name': 'Test project', 'trackCount': 1, 'playState': 0}
            a.call('service.invoke', dict(runtime_query, payload={'name': 'GetTrack'}), error='INVALID_ARGUMENT')
            assert a.call('service.invoke', {'service': 'runtime', 'method': 'getHostVersion'}) == '7.external-test'
            capabilities = check_external_methods(a)
            monitor_names = {
                'trackSelectionChanged': 'track-selection-changed',
                'trackStateChanged': 'track-state-changed',
                'transportChanged': 'transport-state-changed',
                'projectChanged': 'projects-state-changed',
                'markersChanged': 'marker-list-changed',
                'regionsChanged': 'region-list-changed',
                'currentRegionChanged': 'current-region-changed',
                'loopPointsChanged': 'loop-points-changed',
                'timeSelectionChanged': 'time-selection-changed',
            }
            assert sorted(capabilities['events']) == sorted([*monitor_names, *monitor_names.values()])
            for event in capabilities['events']:
                event_sub = a.call('events.subscribe', {'event': event})['subscriptionId']
                assert a.call('events.unsubscribe', {'subscriptionId': event_sub}) is True
            a.call('events.subscribe', {'event': 'selectionchange'}, error='INVALID_ARGUMENT')
            transport = 'transport-state-changed'
            legacy_transport = 'transportChanged'
            a.call('events.subscribe', {'event': 'transport-changed'}, error='INVALID_ARGUMENT')
            a.call('events.subscribe', {'event': 'transportchange'}, error='INVALID_ARGUMENT')
            transport_subscriptions = {
                a.call('events.subscribe', {'event': name})['subscriptionId']: name
                for name in (transport, legacy_transport)
            }
            external_transport = {}
            deadline = time.monotonic() + 5
            while len(external_transport) < len(transport_subscriptions) and time.monotonic() < deadline:
                a.call('system.getInfo')
                for event in a.events:
                    name = transport_subscriptions.get(event.get('subscriptionId'))
                    if name:
                        assert event['event'] == name
                        snapshot = event['data']
                        assert snapshot['revision'] >= 1 and snapshot['projectEpoch'] >= 1
                        external_transport[name] = snapshot
                if len(external_transport) < len(transport_subscriptions):
                    time.sleep(0.025)
            transport_fields = dict(available=True, state=0, position=6.5, cursor=3)
            native_fields = dict(transport_fields, playing=False, paused=False, recording=False, rate=0.75, loop=True)
            for name, expected in ((transport, dict(native_fields, tempo=128)), (legacy_transport, native_fields)):
                snapshot = external_transport[name]
                assert {key: value for key, value in snapshot.items() if key not in ('projectEpoch', 'revision')} == expected
                assert web_event_snapshot(command, name) == snapshot
                assert command('web', method='ReaWeb_Unsubscribe', args=[name])['result'] is True
            web_legacy_transport = web_event_snapshot(command, 'transportchange')
            assert web_legacy_transport.pop('projectEpoch') >= 1
            assert web_legacy_transport == dict(transport_fields, tempo=128)
            assert command('web', method='ReaWeb_Unsubscribe', args=['transportchange'])['result'] is True
            for subscription in transport_subscriptions:
                assert a.call('events.unsubscribe', {'subscriptionId': subscription}) is True
            selection = 'track-selection-changed'
            previous_selection = 'trackSelectionChanged'
            selection_sub = a.call('events.subscribe', {'event': selection})['subscriptionId']
            first_selection = selection_snapshot(a, selection_sub, selection)
            previous_sub = a.call('events.subscribe', {'event': previous_selection})['subscriptionId']
            assert selection_snapshot(a, previous_sub, previous_selection) == first_selection
            command('selection', count=0)
            next_selection = selection_snapshot(a, selection_sub, selection, count=0)
            assert next_selection['projectEpoch'] == first_selection['projectEpoch']
            assert next_selection['revision'] > first_selection['revision']
            assert selection_snapshot(a, previous_sub, previous_selection, count=0) == next_selection
            assert a.call('events.unsubscribe', {'subscriptionId': selection_sub}) is True
            a.events.clear()
            command('selection', count=1)
            assert selection_snapshot(a, previous_sub, previous_selection)['revision'] > next_selection['revision']
            assert all(event.get('subscriptionId') != selection_sub for event in a.events)
            assert a.call('events.unsubscribe', {'subscriptionId': previous_sub}) is True
            a.call('system.getInfo', {'extra': True}, error='INVALID_ARGUMENT')
            a.call('window.close', error='UNKNOWN_METHOD')
            with socket.socket() as occupied:
                occupied.bind(('127.0.0.1', 0)); occupied.listen()
                command('port', value=occupied.getsockname()[1], expect='TRANSPORT_UNAVAILABLE')
                assert a.call('system.getInfo')['protocolVersion'] == 1
            a.frame('ping', opcode=9)
            assert a.receive_frame() == (10, b'ping')
            payload = {'unicode': '外部客户端', 'buffer': {'type': 'binary', 'data': 'AAH/gA=='}}
            assert a.call('service.invoke', dict(service='test', method='echo', payload=payload)) == payload
            assert a.call('service.invoke', dict(service='test', method='consumer')) == {'windowId': 0}
            assert a.call('service.send', dict(service='test', method='input', payload=payload)) == {'accepted': True}
            a.call('service.invoke', dict(service='absent', method='echo'), error='SERVICE_NOT_FOUND')
            a.call('service.invoke', dict(service='test', method='absent'), error='METHOD_NOT_FOUND')
            assert command('web')['error']['code'] == 'UNKNOWN_API'
            assert command('web', method='ReaWeb_Batch', args=[[]])['error']['code'] == 'UNKNOWN_API'
            assert 'result' in command('web', method='ReaWeb_Subscribe', args=['trackStateChanged'])
            assert command('web', method='ReaWeb_Unsubscribe', args=['trackStateChanged'])['result'] is True
            web_selection = web_event_snapshot(command, selection)
            assert set(web_selection) == {'projectEpoch', 'count', 'revision'} and web_selection['count'] == 1
            for event in ('selectionchange', 'track-selected'):
                assert web_event_snapshot(command, event) == web_selection
            web_previous = web_event_snapshot(command, previous_selection)
            assert web_previous['invalidated'] is True and web_previous['available'] is True and web_previous['count'] == 1
            for event in (selection, 'selectionchange', 'track-selected', previous_selection):
                assert command('web', method='ReaWeb_Unsubscribe', args=[event])['result'] is True
            subscription = a.call('service.subscribe', dict(service='test', event='update'))['subscriptionId']
            command('emit', target=True)
            a.call('system.getInfo')
            assert not any(e.get('subscriptionId') == subscription for e in a.events), 'Window-targeted events leaked'
            command('emit')
            a.call('system.getInfo')
            assert any(e.get('subscriptionId') == subscription and e.get('service') == 'test' for e in a.events)
            legacy_project_event = 'projectChanged'
            sub_a, sub_b = subscribe_projects(a, legacy_project_event), subscribe_projects(b, legacy_project_event)
            old_a = project_snapshot(a, sub_a, event_name=legacy_project_event)
            old_b = project_snapshot(b, sub_b, event_name=legacy_project_event)
            assert old_a['activeProject'] != old_b['activeProject']
            second = subscribe_projects(a)
            assert project_snapshot(a, second) == old_a
            assert a.call('events.unsubscribe', dict(subscriptionId=sub_a)) is True
            a.events.clear()
            command('generation')  # Reload at the same native project address.
            new_a = project_snapshot(a, second, old_a['projectEpoch'])['activeProject']
            new_b = project_snapshot(b, sub_b, old_b['projectEpoch'], event_name=legacy_project_event)['activeProject']
            assert len({new_a, new_b, old_a['activeProject'], old_b['activeProject']}) == 4
            assert all(e.get('subscriptionId') != sub_a for e in a.events)
            b.call('service.unsubscribe', dict(subscriptionId=subscription), error='INVALID_ARGUMENT')
            stream = a.call('stream.open', dict(name='test.binary'))
            stream_socket = WebSocket(stream['endpoint'], 'http://arbitrary.external.origin')
            clients.append(stream_socket)
            try:
                WebSocket(stream['endpoint'])
                raise AssertionError('Stream ticket was reused')
            except (EOFError, ConnectionResetError):
                pass
            command('publish')
            opcode, packet = stream_socket.receive_frame()
            assert opcode == 2 and packet[:4] == b'RWS\x01' and packet[40:] == b'\0\xff\x80\1'
            stream_socket.frame(b'\1', opcode=2)
            b.call('stream.close', dict(consumerId=stream['consumerId']), error='INVALID_ARGUMENT')
            assert a.call('stream.close', dict(consumerId=stream['consumerId'])) is True
            stream_socket.closed()
            # A pending invoke may finish after a later synchronous request.
            pending_id = a.request('service.invoke', dict(service='test', method='pending'))
            info_id = a.request('system.getInfo')
            assert a.response(info_id)['result']['protocolVersion'] == 1
            command('complete')
            assert a.response(pending_id)['result'] == 42
            overflow = connect(config['token'])
            for _ in range(capabilities['limits']['pendingRequests'] + 1):
                overflow.request('service.invoke', dict(service='test', method='pending'))
            overflow.closed()
            duplicate = connect(config['token'])
            pending_id = duplicate.request('service.invoke', dict(service='test', method='pending'))
            duplicate.call('system.getInfo')
            duplicate.frame(json.dumps(dict(type='request', id=pending_id, method='system.getInfo', params={})))
            duplicate.closed()
            # Disconnect cancels invokes and removes only this client's consumers.
            a.call('stream.open', dict(name='test.binary'))
            b.call('stream.open', dict(name='test.binary'))
            a.request('service.invoke', dict(service='test', method='pending'))
            a.call('system.getInfo')
            a.close()
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                stats = command('stats')
                if stats['cancelled'] >= 2 and stats['streams']['consumers'] == 1:
                    break
                time.sleep(0.01)
            assert stats['cancelled'] >= 2 and stats['streams']['consumers'] == 1, stats
            assert command('web')['error']['code'] == 'UNKNOWN_API'
            token = command('rotate')['token']
            b.closed()
            assert command('stats')['streams']['consumers'] == 0
            revoked = connect()
            revoked.call('auth.authenticate', dict(token=config['token'], protocolVersion=1), error='AUTH_FAILED')
            revoked.closed()
            c = connect(token)
            assert project_snapshot(c, subscribe_projects(c))['activeProject'] not in {new_a, new_b}
            c.call('service.subscribe', dict(service='test', event='update'))
            c.request('service.invoke', dict(service='test', method='pending'))
            c.call('system.getInfo')
            command('unload')
            messages = [c.receive(), c.receive()]
            assert any(m.get('error', {}).get('code') == 'EXTENSION_UNLOADED' for m in messages)
            assert any(m.get('event') == 'unloaded' for m in messages)
            command('enabled', value=False)
            c.closed()
            assert command('web')['error']['code'] == 'UNKNOWN_API'
            command('enabled', value=True)
            d = connect(token)
            assert d.call('service.invoke', runtime_query)['trackCount'] == 1
            command('stop')
            assert process.wait(timeout=5) == 0
            assert 'WebViewColorProfile=sRGB' in Path(directory, 'ReaWebAPI.ini').read_text()
            print('External protocol, native reuse, ownership, authentication, binary stream and WebView coexistence passed')
        finally:
            for client in clients:
                client.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)


if __name__ == '__main__':
    main()

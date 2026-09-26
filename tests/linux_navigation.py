"""Run the instrumented WebKit helper without launching system applications."""
import functools
import http.server
import json
import os
from pathlib import Path
import select
import socket
import sys
import tempfile
import threading
import time


def run(helper):
    with tempfile.TemporaryDirectory(prefix='reaweb-navigation-') as directory:
        root = Path(directory)
        entry = root / 'index.html'
        entry.write_text('<!doctype html><title>Navigation test</title><body>Retained page</body>')
        handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=directory)
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        parent, child = socket.socketpair()
        log = root / 'opened.txt'
        log.touch()
        environment = {**os.environ, 'REAWEB_NAVIGATION_LOG': str(log), 'GDK_BACKEND': 'x11'}
        # Match the production helper's inherited channel descriptor.
        process = os.posix_spawn(str(helper), [str(helper), str(root / 'profile')], environment,
                                file_actions=[(os.POSIX_SPAWN_DUP2, child.fileno(), 3),
                                              (os.POSIX_SPAWN_CLOSE, child.fileno())])
        child.close()
        pending = b''
        events = []

        def send(**message):
            parent.sendall((json.dumps(message) + '\n').encode())

        def until(condition):
            nonlocal pending
            deadline = time.monotonic() + 20
            while not condition() and time.monotonic() < deadline:
                if not select.select([parent], [], [], .05)[0]:
                    continue
                chunk = parent.recv(65536)
                assert chunk, 'WebKit helper exited'
                pending += chunk
                while b'\n' in pending:
                    line, pending = pending.split(b'\n', 1)
                    event = json.loads(line)
                    assert event['op'] != 'error', event
                    if event['op'] == 'message':
                        event['message'] = json.loads(event['message'])
                    events.append(event)
            assert condition(), events[-5:]

        try:
            until(lambda: any(e['op'] == 'ready' for e in events))
            url = f'http://127.0.0.1:{server.server_port}/index.html'
            for window, uri in enumerate((entry.as_uri(), url, url + '?dev=1'), 1):
                script = '''window.retained=42;window.warnings=[];
                    console.warn=text=>warnings.push(text);
                    window.report=()=>webkit.messageHandlers.reaweb.postMessage(JSON.stringify({url:location.href,retained,warnings}));
                    addEventListener('DOMContentLoaded',report);'''
                send(id=window, op='open', uri=uri, script=script, lifecycleReload=True)
                reports = lambda: [e['message'] for e in events if e.get('id') == window and e['op'] == 'message']
                until(lambda: reports())
                navigations = sum(e['op'] == 'navigating' for e in events)

                def check(script, launches=0, warning=False):
                    before = len(log.read_text().splitlines())
                    count = len(reports())
                    report = (';(()=>{const timer=setInterval(()=>{if(warnings.length){clearInterval(timer);report();}},50);})();'
                              if warning else ';setTimeout(report,150);')
                    send(id=window, op='eval', script='warnings=[];' + script + report)
                    until(lambda: len(reports()) > count)
                    result = reports()[-1]
                    assert result['url'].split('#')[0] == uri and result['retained'] == 42, result
                    assert bool(result['warnings']) == warning, result
                    assert len(log.read_text().splitlines()) == before + launches, script
                    assert sum(e['op'] == 'navigating' for e in events) == navigations
                    assert not any(e['op'] == 'reload-request' for e in events)

                check("location.hash='section'")
                check("const a=document.createElement('a');a.href='https://example.com/link';a.click()", 1)
                assert log.read_text().splitlines()[-1] == 'https://example.com/link'
                check("location.href='http://example.com/href'", 1)
                check("location.assign('https://example.com/assign')", 1)
                check("location.assign('mailto:test@example.com?subject=Hi')", 1)
                check("location.assign('other.html')", warning=True)
                assert 'reaper.window.open(path)' in reports()[-1]['warnings'][0]
                check("location.search='?changed=1'", warning=True)
                check("window.open('https://example.com/popup')")
                check("const b=document.createElement('a');b.href='https://example.com/blank';b.target='_blank';b.click()")
                check("location.href='custom:unsupported'", warning=True)
                check("location.href='https://example.com/fail'", 1, warning=True)
                send(id=window, op='close')
            print('WebKitGTK navigation: file/HTTP/dev entries, external handlers, local/query/hash, popup rejection and failure recovery passed')
        finally:
            parent.close()
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if os.waitpid(process, os.WNOHANG)[0]:
                    break
                time.sleep(.02)
            else:
                os.kill(process, 9)
                os.waitpid(process, 0)
            server.shutdown()
            server.server_close()


if __name__ == '__main__':
    run(Path(sys.argv[1]).resolve())

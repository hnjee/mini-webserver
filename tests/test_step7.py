"""Step 7 TCP integration smoke. Run in the Linux test image with python3 tests/test_step7.py."""
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

SERVER = Path(os.environ.get("STEP7_SERVER", Path(__file__).resolve().parent.parent / "webserver"))
ROOT = None


def port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def dial(p):
    s = socket.create_connection(("127.0.0.1", p), 1)
    s.settimeout(7)
    return s


def start(base, route=None, binary=SERVER):
    p = port()
    out = open(base / "out.log", "w+")
    err = open(base / "err.log", "w+")
    args = [str(binary), "-p", str(p), "-r", str(ROOT)]
    if route:
        args += ["-c", str(route)]
    proc = subprocess.Popen(args, stdout=out, stderr=err)
    for _ in range(100):
        if proc.poll() is not None:
            raise AssertionError(f"startup failed: {err.read()}")
        try:
            s = dial(p)
            s.close()
            break
        except OSError:
            time.sleep(.01)
    else:
        raise AssertionError("startup timeout")
    return proc, p, out, err


def finish(proc, out, err, sig=signal.SIGTERM, timeout=7):
    t = time.monotonic()
    proc.send_signal(sig)
    rc = proc.wait(timeout=timeout)
    elapsed = time.monotonic() - t
    out.flush(); err.flush()
    out.seek(0); err.seek(0)
    text, errors = out.read(), err.read()
    out.close(); err.close()
    assert rc == 0, (rc, errors)
    assert "서버 종료" in text, text
    return elapsed, text, errors


def recv_end(s):
    chunks = []
    while True:
        part = s.recv(65536)
        if not part: break
        chunks.append(part)
    return b"".join(chunks)


def response(s):
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(4096)
    head, body = buf.split(b"\r\n\r\n", 1)
    length = int(next(line[16:] for line in head.split(b"\r\n") if line.lower().startswith(b"content-length: ")))
    while len(body) < length:
        body += s.recv(length - len(body))
    return head, body


def test_idle(base):
    for sig in (signal.SIGINT, signal.SIGTERM):
        proc, _, out, err = start(base)
        elapsed, text, _ = finish(proc, out, err, sig)
        assert elapsed < 1 and f"SIG{sig.name[3:]}" in text, (elapsed, text)
    print("TC-7-03 PASS")


def test_keep_idle(base):
    proc, p, out, err = start(base)
    a, b = dial(p), dial(p)
    b.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    assert response(b)[0].startswith(b"HTTP/1.1 200")
    elapsed, _, _ = finish(proc, out, err)
    assert elapsed < 1 and a.recv(1) == b"" and b.recv(1) == b""
    a.close(); b.close()
    print("TC-7-04 PASS")


def test_partial(base):
    proc, p, out, err = start(base)
    s = dial(p)
    s.sendall(b"GET / HTTP/1.1\r\n")
    time.sleep(.2)  # 서버가 부분 머리를 읽어 요청 중 상태에 진입할 시간
    proc.send_signal(signal.SIGTERM)
    time.sleep(.1)
    s.sendall(b"Host: x\r\n\r\n")
    s.shutdown(socket.SHUT_WR)
    data = recv_end(s)
    assert data.startswith(b"HTTP/1.1 200") and b"Connection: close\r\n" in data, data
    assert proc.wait(3) == 0
    out.close(); err.close(); s.close()
    print("TC-7-07 PASS")


def test_incomplete(base):
    proc, p, out, err = start(base)
    s = dial(p)
    s.sendall(b"GET / HTTP/1.1\r\n")
    time.sleep(.2)
    t = time.monotonic()
    proc.send_signal(signal.SIGTERM)
    assert proc.wait(7) == 0
    elapsed = time.monotonic() - t
    assert 4.5 < elapsed < 6.5, elapsed
    s.close(); out.close(); err.close()
    print("TC-7-08 PASS", round(elapsed, 2))


def test_backend(base):
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0)); listener.listen()
    route = base / "route.conf"
    route.write_text(f"route /api/ 127.0.0.1:{listener.getsockname()[1]}\n")
    proc, p, out, err = start(base, route)
    s = dial(p)
    s.sendall(b"GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n")
    time.sleep(.1)
    proc.send_signal(signal.SIGTERM)
    data = recv_end(s)
    assert data.startswith(b"HTTP/1.1 504") and b"Connection: close\r\n" in data, data
    assert proc.wait(7) == 0
    s.close(); listener.close(); out.close(); err.close()
    print("TC-7-09 PASS")


def test_logs(base):
    proc, p, out, err = start(base)
    s = dial(p)
    s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    h1, b1 = response(s)
    s.sendall(b"GET /missing HTTP/1.1\r\nHost: x\r\n\r\n")
    h2, b2 = response(s)
    s.close()
    malformed = dial(p)
    malformed.sendall(b"GET /bad HTTP/1.1\r\nHost: x\r\nBad\r\n\r\n")
    malformed.shutdown(socket.SHUT_WR)
    h3 = recv_end(malformed)
    malformed.close()
    echo = dial(p)
    echo.sendall(b"POST /echo HTTP/1.0\r\nContent-Length: 3\r\n\r\nabc")
    h4 = recv_end(echo)
    echo.close()
    _, text, errors = finish(proc, out, err)
    lines = [line for line in text.splitlines() if line.endswith(" ok") or line.endswith(" aborted")]
    assert len(lines) == 4, (lines, errors)
    assert all(" ok" in line for line in lines), lines
    by_request = {line.split('"')[1]: line for line in lines}
    assert 'GET / HTTP/1.1' in by_request and f" {len(h1)+4+len(b1)} " in by_request['GET / HTTP/1.1']
    assert 'GET /missing HTTP/1.1' in by_request and f" {len(h2)+4+len(b2)} " in by_request['GET /missing HTTP/1.1']
    assert '-' in by_request and ' 400 ' in by_request['-'] and h3.startswith(b"HTTP/1.1 400")
    assert 'POST /echo HTTP/1.0' in by_request and ' 200 ' in by_request['POST /echo HTTP/1.0'] and b"abc" in h4
    print("TC-7-11 PASS")


def early_413_backend(base):
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0)); listener.listen(); listener.settimeout(8)
    def serve():
        conn, _ = listener.accept()
        with conn:
            data = b""
            while b"\r\n\r\n" not in data:
                data += conn.recv(4096)
            conn.sendall(b"HTTP/1.0 413 Content Too Large\r\nContent-Length: 3\r\n\r\nBAD")
        listener.close()
    thread = threading.Thread(target=serve, daemon=True); thread.start()
    route = base / "route-413.conf"
    route.write_text(f"route /api/ 127.0.0.1:{listener.getsockname()[1]}\n")
    return route, thread


def test_early_proxy_log(base):
    """F2: 완송한 조기 프록시 응답은 남은 업로드가 끊겨도 ok 한 줄."""
    for complete_upload in (True, False):
        route, thread = early_413_backend(base)
        proc, p, out, err = start(base, route)
        s = dial(p)
        s.sendall(b"POST /api/x HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\na")
        head, body = response(s)
        assert head.startswith(b"HTTP/1.1 413") and body == b"BAD", (head, body)
        if complete_upload:
            s.sendall(b"123456789"); time.sleep(.2)
        s.shutdown(socket.SHUT_WR)
        recv_end(s); s.close(); thread.join(2)
        _, text, errors = finish(proc, out, err, signal.SIGINT)
        lines = [l for l in text.splitlines() if l.endswith(" ok") or l.endswith(" aborted")]
        assert len(lines) == 1 and '"POST /api/x HTTP/1.1" 413 56 ' in lines[0] and lines[0].endswith(" ok"), \
            (complete_upload, lines, errors)
    print("TC-7-11 early proxy 413 PASS")


def test_stop_after_proxy(base):
    """F3: 프록시 응답 뒤 같은 연결의 정적 요청은 머리 생성 전 정지면 Connection: close."""
    route, thread = early_413_backend(base)
    proc, p, out, err = start(base, route)
    s = dial(p)
    s.sendall(b"GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n")
    assert response(s)[0].startswith(b"HTTP/1.1 413")
    s.sendall(b"GET /index.html HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\na")
    time.sleep(.2)
    proc.send_signal(signal.SIGTERM)
    time.sleep(.2)
    s.sendall(b"bc")
    head, body = response(s)
    assert head.startswith(b"HTTP/1.1 200") and body == b"hello\n", (head, body)
    assert b"\r\nConnection: close" in head + b"\r\n", head
    assert s.recv(1) == b""
    s.close(); thread.join(2)
    assert proc.wait(5) == 0
    out.close(); err.close()
    print("TC-7-17 PASS")


def test_limits(base, binary):
    proc, p, out, err = start(base, binary=binary)
    requests = [(b"GET / HTTP/1.1\r\nHost: x\r\nX: " + b"a"*8192 + b"\r\n\r\n", b"431"),
                (b"GET /" + b"a"*2048 + b" HTTP/1.1\r\nHost: x\r\n\r\n", b"414"),
                (b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n", b"413")]
    for req, code in requests:
        s = dial(p)
        s.sendall(req); s.shutdown(socket.SHUT_WR)
        data = recv_end(s); s.close()
        assert data[9:12] == code, (code, data[:60])
    target = b"/" + b"/".join([b"a" * 127] * 16)[:2047]
    assert len(target) <= 2048
    s = dial(p)
    s.sendall(b"GET " + target + b" HTTP/1.0\r\n\r\n")
    data = recv_end(s); s.close()
    assert data.startswith(b"HTTP/1.1 404"), data[:80]
    _, text, errors = finish(proc, out, err)
    lines = [line for line in text.splitlines() if line.endswith(" ok")]
    assert len(lines) == 4 and target.decode() in lines[-1] and len(lines[-1]) < 2400, lines[-1:]
    assert "Sanitizer" not in errors, errors
    print("TC-7-15 PASS")


def test_start_fail(base, binary):
    bad = base / "bad.conf"; bad.write_text("bad\n")
    for config, occupied in ((bad, False), (None, True)):
        args = [str(binary), "-p", str(port_in_use if occupied else port()), "-r", str(ROOT)]
        if config: args += ["-c", str(config)]
        r = subprocess.run(args, capture_output=True, text=True, timeout=5)
        assert r.returncode == 1 and "Sanitizer" not in r.stderr, (r.returncode, r.stderr)
    print("TC-7-14 PASS")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="webserver-step7-") as tmp:
        base = Path(tmp); ROOT = base / "www"; ROOT.mkdir(); (ROOT / "index.html").write_text("hello\n")
        # Keep a listener live for the occupied-port start-failure check.
        occupied = socket.socket(); occupied.bind(("127.0.0.1", 0)); occupied.listen()
        port_in_use = occupied.getsockname()[1]
        only = sys.argv[1:]  # 예: python3 tests/test_step7.py test_early_proxy_log
        if only:
            for name in only:
                globals()[name](base)
            sys.exit(0)
        test_idle(base); test_keep_idle(base); test_partial(base); test_incomplete(base)
        test_backend(base); test_logs(base); test_early_proxy_log(base); test_stop_after_proxy(base)
        test_start_fail(base, SERVER.with_name("webserver-debug"))
        test_limits(base, SERVER.with_name("webserver-debug"))
        occupied.close()

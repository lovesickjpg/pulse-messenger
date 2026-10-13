import contextlib
import json
import os
import signal
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time


def frame(request_id, text="hello", conversation="lobby"):
    payload = json.dumps({
        "version": 1,
        "type": "message.send",
        "request_id": request_id,
        "payload": {
            "conversation_id": conversation,
            "client_message_id": request_id,
            "text": text,
        },
    }, ensure_ascii=False).encode("utf-8")
    return struct.pack("!I", len(payload)) + payload


def read_exact(peer, size):
    data = bytearray()
    while len(data) < size:
        chunk = peer.recv(size - len(data))
        if not chunk:
            raise AssertionError("unexpected EOF")
        data.extend(chunk)
    return data


def receive(peer):
    size, = struct.unpack("!I", read_exact(peer, 4))
    assert 0 < size <= 65536
    return json.loads(read_exact(peer, size))


def connect(port, slow=False):
    peer = socket.socket()
    peer.settimeout(5)
    peer.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    if slow:
        peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    try:
        peer.connect(("127.0.0.1", port))
        return peer
    except BaseException:
        peer.close()
        raise


def closed(peer):
    try:
        assert peer.recv(1) == b"", "connection should be closed"
    except ConnectionResetError:
        pass


@contextlib.contextmanager
def server(binary, run_for=None, extra_args=()):
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    args = [binary, str(port)]
    if run_for is not None:
        args += ["--run-for", str(run_for)]
    args += list(extra_args)
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen(args, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 10
            while True:
                if process.poll() is not None:
                    raise AssertionError("server exited during startup")
                try:
                    with connect(port):
                        break
                except ConnectionRefusedError:
                    if time.monotonic() >= deadline:
                        raise AssertionError("server did not start")
                    time.sleep(0.02)
            yield port, process
        except BaseException:
            log.seek(0)
            print(log.read().decode("utf-8", errors="replace"), file=sys.stderr)
            raise
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def exchange(peers, sender, request_id, text="hello", data=None):
    sender.sendall(data if data is not None else frame(request_id, text))
    result = receive(sender)
    assert result["type"] == "message.send.result"
    assert result["request_id"] == request_id
    for peer in peers:
        event = receive(peer)
        assert event["type"] == "message.received"
        assert event["request_id"] is None
        assert event["payload"]["message_id"] == result["payload"]["message_id"]
        assert event["payload"]["client_message_id"] == request_id
        assert event["payload"]["text"] == text


def test_network(server_binary, client_binary):
    with server(server_binary) as (port, _), contextlib.ExitStack() as stack:
        peers = []
        for index in range(3):
            peer = stack.enter_context(connect(port))
            peers.append(peer)
            exchange(peers, peer, f"join-{index}")

        for index, peer in enumerate(peers):
            exchange(peers, peer, f"send-{index}", f"hello from {index}")

        fragmented = frame("fragmented", "Привет!")
        for byte in fragmented[:-1]:
            peers[0].sendall(bytes([byte]))
        exchange(peers, peers[0], "fragmented", "Привет!", fragmented[-1:])

        peers[0].sendall(frame("batch-1") + frame("batch-2"))
        for request_id in ("batch-1", "batch-2"):
            result, event = receive(peers[0]), receive(peers[0])
            assert result["request_id"] == request_id
            assert event["payload"]["client_message_id"] == request_id
            for peer in peers[1:]:
                assert receive(peer)["payload"]["client_message_id"] == request_id

        peers[2].setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii" if os.name != "nt" else "hh", 1, 0))
        peers.pop().close()
        exchange(peers, peers[0], "after-reset")

        for bad_frame in (b"\x00\x01\x00\x01", b"\x00\x00\x00\x01{",
                          b"\x00\x00", frame("truncated")[:-3]):
            with connect(port) as broken:
                broken.sendall(bad_frame)
                broken.shutdown(socket.SHUT_WR)
                closed(broken)
        exchange(peers, peers[1], "after-bad-frames")

        peers[0].sendall(frame("missing-room", conversation="missing"))
        error = receive(peers[0])
        assert error["type"] == "error"
        assert error["request_id"] == "missing-room"
        assert error["payload"]["code"] == "not_found"

        with connect(port, slow=True) as slow:
            exchange(peers + [slow], slow, "slow-join")
            for index in range(100):
                exchange(peers, peers[0], f"load-{index}", "x" * 60000)
            # drain whatever reached the kernel before the server dropped this reader
            try:
                while slow.recv(65536):
                    pass
            except ConnectionResetError:
                pass
        exchange(peers, peers[1], "after-slow-reader")

        completed = subprocess.run([client_binary, str(port)], input="one\nПривет!\nlast",
                                   encoding="utf-8", capture_output=True, timeout=10)
        assert completed.returncode == 0, completed.stderr
        for text in ("one", "Привет!", "last"):
            assert text in completed.stdout, completed.stdout
            for peer in peers:
                assert receive(peer)["payload"]["text"] == text

        completed = subprocess.run([client_binary, str(port)], input="before quit\n/quit\nafter quit\n",
                                   encoding="utf-8", capture_output=True, timeout=10)
        assert completed.returncode == 0, completed.stderr
        assert "before quit" in completed.stdout
        assert "after quit" not in completed.stdout
        for peer in peers:
            assert receive(peer)["payload"]["text"] == "before quit"
    print("multi-client delivery, framing, disconnects, backpressure, CLI: passed")


def test_shutdown(server_binary, client_binary):
    with server(server_binary, run_for=2) as (port, process), contextlib.ExitStack() as stack:
        peers = [stack.enter_context(connect(port)) for _ in range(3)]
        peers[0].sendall(frame("unfinished")[:7])
        client = subprocess.Popen([client_binary, str(port)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            assert process.wait(timeout=5) == 0
            for peer in peers:
                closed(peer)
            # stdin stays open; disconnect alone must let the CLI exit
            assert client.wait(timeout=5) in (0, 1)
            assert b"Connected to" in client.stdout.read()
        finally:
            if client.poll() is None:
                client.kill()
                client.wait()
            client.stdin.close()
            client.stdout.close()
            client.stderr.close()

    if os.name != "nt":
        with server(server_binary) as (port, process), connect(port) as peer:
            process.send_signal(signal.SIGINT)
            assert process.wait(timeout=5) == 0
            closed(peer)
    print("shutdown with active connections and open stdin: passed")


def schema(path):
    with contextlib.closing(sqlite3.connect(path)) as db:
        return db.execute("SELECT version, name, applied_at FROM schema_migrations").fetchall()


def test_storage(server_binary):
    with tempfile.TemporaryDirectory() as directory:
        database = os.path.join(directory, "nested", "pulse.db")

        # first start creates the file and applies the initial migration
        with server(server_binary, run_for=1, extra_args=["--db", database]) as (_, process):
            assert process.wait(timeout=10) == 0
        first = schema(database)
        assert [(v, n) for v, n, _ in first] == [(1, "001_initial")], first
        with contextlib.closing(sqlite3.connect(database)) as db:
            tables = {row[0] for row in db.execute(
                "SELECT name FROM sqlite_master WHERE type = 'table'")}
            assert {"users", "sessions", "conversations", "conversation_members",
                    "messages", "schema_migrations"} <= tables, tables

        # restart keeps the data and doesn't re-run the migration
        with contextlib.closing(sqlite3.connect(database)) as db:
            db.execute("INSERT INTO users (username, password_hash, created_at) "
                       "VALUES ('alice', 'test-hash', 0)")
            db.commit()
        with server(server_binary, run_for=1, extra_args=["--db", database]) as (_, process):
            assert process.wait(timeout=10) == 0
        assert schema(database) == first
        with contextlib.closing(sqlite3.connect(database)) as db:
            assert db.execute("SELECT username FROM users").fetchall() == [("alice",)]

        # a database from a newer build must stop the server before it listens
        with contextlib.closing(sqlite3.connect(database)) as db:
            db.execute("INSERT INTO schema_migrations VALUES (99, '099_future', 0)")
            db.commit()
        completed = subprocess.run([server_binary, "--db", database, "--run-for", "1"],
                                   capture_output=True, encoding="utf-8", timeout=10)
        assert completed.returncode == 1, completed.stdout
        assert "newer" in completed.stderr, completed.stderr

        # a path that can't be a database is a startup error, not a crash
        blocker = os.path.join(directory, "blocker")
        with open(blocker, "w") as handle:
            handle.write("not a directory")
        completed = subprocess.run([server_binary, "--db", os.path.join(blocker, "pulse.db")],
                                   capture_output=True, encoding="utf-8", timeout=10)
        assert completed.returncode == 1, completed.stdout
        assert completed.stderr.startswith("Server:"), completed.stderr

        # option parsing
        for bad in (["--db"], ["--bogus", "1"], ["5555", "--run-for"]):
            completed = subprocess.run([server_binary] + bad, capture_output=True,
                                       encoding="utf-8", timeout=10)
            assert completed.returncode == 1 and "Usage" in completed.stderr, bad
    print("database startup, migrations, restart, bad database, options: passed")


if __name__ == "__main__":
    server_binary, client_binary = sys.argv[1:]
    test_network(server_binary, client_binary)
    test_shutdown(server_binary, client_binary)
    test_storage(server_binary)

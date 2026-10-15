import contextlib
import itertools
import json
import os
import signal
import socket
import sqlite3
import ssl
import struct
import subprocess
import sys
import tempfile
import time

PASSWORD = "correct horse"
_ids = itertools.count()


def encode(message_type, payload, request_id):
    data = json.dumps({"version": 1, "type": message_type, "request_id": request_id,
                       "payload": payload}, ensure_ascii=False).encode("utf-8")
    return struct.pack("!I", len(data)) + data


def frame(request_id, text="hello", conversation="lobby"):
    return encode("message.send", {"conversation_id": conversation,
                                   "client_message_id": request_id, "text": text}, request_id)


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


def call(peer, message_type, payload):
    request_id = f"req-{next(_ids)}"
    peer.sendall(encode(message_type, payload, request_id))
    reply = receive(peer)
    assert reply["request_id"] == request_id, reply
    return reply


def expect_error(reply, code):
    assert reply["type"] == "error" and reply["payload"]["code"] == code, reply


def register(peer, username, password=PASSWORD):
    reply = call(peer, "auth.register", {"username": username, "password": password})
    assert reply["type"] == "auth.register.result", reply
    return reply


def login(peer, username, password=PASSWORD):
    reply = call(peer, "auth.login", {"username": username, "password": password})
    assert reply["type"] == "auth.login.result", reply
    return reply["payload"]["session_token"]


def closed(peer):
    try:
        assert peer.recv(1) == b"", "connection should be closed"
    except (ConnectionResetError, ConnectionAbortedError, ssl.SSLError):
        pass


class Certs:
    def __init__(self, tool, directory):
        subprocess.run([tool, directory, "--test-set"], check=True, capture_output=True,
                       timeout=30)
        self.directory = directory
        self.ca = os.path.join(directory, "ca.crt")

    def pair(self, name):
        return [os.path.join(self.directory, name + ".crt"),
                os.path.join(self.directory, name + ".key")]


def connect(port, certs, slow=False):
    raw = socket.socket()
    raw.settimeout(5)
    raw.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    if slow:
        raw.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    try:
        raw.connect(("127.0.0.1", port))
        context = ssl.create_default_context(cafile=certs.ca)
        return context.wrap_socket(raw, server_hostname="localhost")
    except BaseException:
        raw.close()
        raise


def free_port():
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        return reservation.getsockname()[1]


def server_args(binary, certs, port, cert="server", extra_args=()):
    certificate, key = certs.pair(cert)
    return [binary, str(port), "--cert", certificate, "--key", key,
            "--password-cost", "minimal", *extra_args]


@contextlib.contextmanager
def server(binary, certs, run_for=None, extra_args=(), cert="server"):
    port = free_port()
    args = server_args(binary, certs, port, cert, extra_args)
    if run_for is not None:
        args += ["--run-for", str(run_for)]
    with tempfile.TemporaryFile() as log:
        def read_log():
            log.flush()
            log.seek(0)
            return log.read().decode("utf-8", errors="replace")

        process = subprocess.Popen(args, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 10
            while True:
                if process.poll() is not None:
                    raise AssertionError("server exited during startup")
                try:
                    socket.create_connection(("127.0.0.1", port), timeout=1).close()
                    break
                except ConnectionRefusedError:
                    if time.monotonic() >= deadline:
                        raise AssertionError("server did not start")
                    time.sleep(0.02)
            yield port, process, read_log
        except BaseException:
            print(read_log(), file=sys.stderr)
            raise
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


@contextlib.contextmanager
def user(port, certs, name, slow=False):
    """A connection that's registered and logged in as `name`."""
    with connect(port, certs, slow) as peer:
        register(peer, name)
        login(peer, name)
        yield peer


def exchange(peers, sender, request_id, text="hello", data=None):
    sender.sendall(data if data is not None else frame(request_id, text))
    result = receive(sender)
    assert result["type"] == "message.send.result", result
    assert result["request_id"] == request_id
    for peer in peers:
        event = receive(peer)
        assert event["type"] == "message.received"
        assert event["request_id"] is None
        assert event["payload"]["message_id"] == result["payload"]["message_id"]
        assert event["payload"]["client_message_id"] == request_id
        assert event["payload"]["text"] == text


def run_client(client_binary, certs, port, text, *extra):
    return subprocess.run([client_binary, str(port), "--ca", certs.ca, *extra], input=text,
                          encoding="utf-8", capture_output=True, timeout=15)


def test_network(server_binary, client_binary, certs):
    with server(server_binary, certs) as (port, _, _), contextlib.ExitStack() as stack:
        peers = []
        for index in range(3):
            peer = stack.enter_context(user(port, certs, f"user{index}"))
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
            assert event["payload"]["sender"] == "user0"
            for peer in peers[1:]:
                assert receive(peer)["payload"]["client_message_id"] == request_id

        peers[2].setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                            struct.pack("ii" if os.name != "nt" else "hh", 1, 0))
        peers.pop().close()
        exchange(peers, peers[0], "after-reset")

        # bad headers and bad JSON get the connection dropped on the spot
        for bad_frame in (b"\x00\x01\x00\x01", b"\x00\x00\x00\x01{"):
            with connect(port, certs) as broken:
                broken.sendall(bad_frame)
                closed(broken)
        # cut-off frames: just hang up mid-frame and make sure nobody else notices
        for partial in (b"\x00\x00", frame("truncated")[:-3]):
            with connect(port, certs) as broken:
                broken.sendall(partial)
        exchange(peers, peers[1], "after-bad-frames")

        expect_error(call(peers[0], "message.send", {"conversation_id": "missing",
                                                     "client_message_id": "x", "text": "x"}),
                     "not_found")

        with user(port, certs, "slow", slow=True) as slow:
            exchange(peers + [slow], slow, "slow-join")
            for index in range(100):
                exchange(peers, peers[0], f"load-{index}", "x" * 60000)
            # drain whatever reached the kernel before the server dropped this reader
            try:
                while slow.recv(65536):
                    pass
            except (ConnectionResetError, ConnectionAbortedError, ssl.SSLError, TimeoutError):
                pass
        exchange(peers, peers[1], "after-slow-reader")

        completed = run_client(client_binary, certs, port,
                               f"/login user0 {PASSWORD}\none\nПривет!\nlast")
        assert completed.returncode == 0, completed.stderr
        assert "Logged in as user0" in completed.stdout, completed.stdout
        # the CLI exits once every request is answered; its own echoes may not make it
        for text in ("one", "Привет!", "last"):
            for peer in peers:
                assert receive(peer)["payload"]["text"] == text

        completed = run_client(client_binary, certs, port,
                               f"/login user1 {PASSWORD}\nbefore quit\n/quit\nafter quit\n")
        assert completed.returncode == 0, completed.stderr
        assert "after quit" not in completed.stdout
        for peer in peers:
            assert receive(peer)["payload"]["text"] == "before quit"
    print("multi-client delivery, framing, disconnects, backpressure, CLI: passed")


def test_auth(server_binary, client_binary, certs):
    with server(server_binary, certs) as (port, _, read_log):
        with connect(port, certs) as alice:
            register(alice, "alice")
            expect_error(call(alice, "auth.register",
                              {"username": "ALICE", "password": PASSWORD}), "conflict")
            expect_error(call(alice, "auth.register",
                              {"username": "a!", "password": PASSWORD}), "invalid_request")
            expect_error(call(alice, "auth.register",
                              {"username": "carol", "password": "short"}), "invalid_request")

            # nothing works before login
            expect_error(call(alice, "message.send", {"conversation_id": "lobby",
                                                      "client_message_id": "x", "text": "x"}),
                         "unauthorized")
            expect_error(call(alice, "auth.logout", {}), "unauthorized")

            token = login(alice, "Alice")  # usernames are case-insensitive
            reply = call(alice, "auth.logout", {})
            assert reply["type"] == "auth.logout.result", reply

        # wrong password and unknown user look exactly the same from outside
        with connect(port, certs) as mallory:
            wrong = call(mallory, "auth.login", {"username": "alice", "password": "nope-nope"})
            missing = call(mallory, "auth.login", {"username": "nobody", "password": "nope-nope"})
            expect_error(wrong, "unauthorized")
            expect_error(missing, "unauthorized")
            assert wrong["payload"] == missing["payload"]
            for _ in range(4):
                expect_error(call(mallory, "auth.login",
                                  {"username": "alice", "password": "nope-nope"}), "unauthorized")
            # five strikes: even the right password waits now, from any connection
            expect_error(call(mallory, "auth.login", {"username": "alice", "password": PASSWORD}),
                         "rate_limited")
        with connect(port, certs) as other:
            expect_error(call(other, "auth.login", {"username": "ALICE", "password": PASSWORD}),
                         "rate_limited")

        # several users online, one logs out, the rest carry on
        with user(port, certs, "first") as u1, user(port, certs, "second") as u2, \
                user(port, certs, "third") as u3:
            exchange([u1, u2, u3], u1, "all-three")
            assert call(u2, "auth.logout", {})["type"] == "auth.logout.result"
            exchange([u1, u3], u3, "two-left")
            # u2 got no lobby traffic after logging out: its next frame is its own reply
            expect_error(call(u2, "message.send", {"conversation_id": "lobby",
                                                   "client_message_id": "y", "text": "y"}),
                         "unauthorized")

        # one session on two connections; logging out on one revokes both
        with connect(port, certs) as first, connect(port, certs) as second:
            register(first, "dave")
            token = login(first, "dave")
            reply = call(second, "auth.resume", {"session_token": token})
            assert reply["type"] == "auth.resume.result", reply
            assert reply["payload"]["username"] == "dave"
            exchange([first, second], second, "shared-session")
            assert call(first, "auth.logout", {})["type"] == "auth.logout.result"
            expect_error(call(second, "message.send", {"conversation_id": "lobby",
                                                       "client_message_id": "z", "text": "z"}),
                         "unauthorized")
            expect_error(call(second, "auth.resume", {"session_token": token}), "unauthorized")
            expect_error(call(second, "auth.resume", {"session_token": "00" * 32}),
                         "unauthorized")
            expect_error(call(second, "auth.resume", {"session_token": "not-a-token"}),
                         "unauthorized")

        # requests on one connection run in order, even when login is still hashing
        with connect(port, certs) as eager:
            register(eager, "eager")
            eager.sendall(encode("auth.login", {"username": "eager", "password": PASSWORD},
                                 "pipelined-login") + frame("pipelined-send"))
            assert receive(eager)["type"] == "auth.login.result"
            assert receive(eager)["type"] == "message.send.result"

        log = read_log()
        assert PASSWORD not in log and "nope-nope" not in log and token not in log, log
    print("register, login, throttling, logout, resume, revocation, ordering: passed")


def test_session_expiry(server_binary, certs):
    with server(server_binary, certs, extra_args=["--session-ttl", "1"]) as (port, _, _):
        with connect(port, certs) as peer:
            register(peer, "brief")
            token = login(peer, "brief")
            exchange([peer], peer, "still-fresh")
            time.sleep(1.5)
            reply = call(peer, "message.send", {"conversation_id": "lobby",
                                                "client_message_id": "late", "text": "late"})
            expect_error(reply, "unauthorized")
            assert "expired" in reply["payload"]["message"]
            expect_error(call(peer, "auth.resume", {"session_token": token}), "unauthorized")
    print("session expiry: passed")


def test_tls(server_binary, client_binary, certs):
    # the client refuses servers it can't verify, and says why
    for cert, reason in (("expired", "certificate has expired"),
                         ("wrong-name", "hostname mismatch"),
                         ("untrusted", "unable to get local issuer certificate")):
        with server(server_binary, certs, cert=cert) as (port, _, _):
            completed = run_client(client_binary, certs, port, f"/login x {PASSWORD}\n")
            assert completed.returncode == 1, completed.stdout
            assert reason in completed.stderr, completed.stderr
            assert "Connected" not in completed.stdout

    with server(server_binary, certs) as (port, _, _):
        # the certificate lists 127.0.0.1 too, and IP literals are checked as IPs
        completed = run_client(client_binary, certs, port, "/quit\n", "--host", "127.0.0.1")
        assert completed.returncode == 0, completed.stderr
        # without our CA, the system store doesn't know this server
        completed = subprocess.run([client_binary, str(port)], input="/quit\n",
                                   encoding="utf-8", capture_output=True, timeout=15)
        assert completed.returncode == 1 and "TLS handshake failed" in completed.stderr

        # plain TCP never gets a protocol reply
        with socket.create_connection(("127.0.0.1", port), timeout=5) as plain:
            plain.sendall(frame("plaintext"))
            try:
                data = plain.recv(65536)
                assert b"message.send.result" not in data
            except (ConnectionResetError, ConnectionAbortedError):
                pass

    # the server needs a matching certificate and key
    port = free_port()
    for args, message in (([server_binary, str(port)], "TLS is required"),
                          ([server_binary, str(port), "--cert", certs.pair("server")[0],
                            "--key", certs.pair("expired")[1]], "mismatch")):
        completed = subprocess.run(args, capture_output=True, encoding="utf-8", timeout=10)
        assert completed.returncode == 1 and message in completed.stderr, completed.stderr
    print("certificate verification, host names, plaintext, server keys: passed")


def test_shutdown(server_binary, client_binary, certs):
    with server(server_binary, certs, run_for=2) as (port, process, _), \
            contextlib.ExitStack() as stack:
        peers = [stack.enter_context(connect(port, certs)) for _ in range(3)]
        peers[0].sendall(frame("unfinished")[:7])
        client = subprocess.Popen([client_binary, str(port), "--ca", certs.ca],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE)
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
        with server(server_binary, certs) as (port, process, _), connect(port, certs) as peer:
            process.send_signal(signal.SIGINT)
            assert process.wait(timeout=5) == 0
            closed(peer)
    print("shutdown with active connections and open stdin: passed")


def schema(path):
    with contextlib.closing(sqlite3.connect(path)) as db:
        return db.execute("SELECT version, name, applied_at FROM schema_migrations").fetchall()


def test_storage(server_binary, certs):
    with tempfile.TemporaryDirectory() as directory:
        database = os.path.join(directory, "nested", "pulse.db")
        with_db = ["--db", database]

        # first start creates the file and applies the initial migration
        with server(server_binary, certs, extra_args=with_db) as (port, process, _):
            with connect(port, certs) as peer:
                register(peer, "keeper")
                login(peer, "keeper")
        first = schema(database)
        assert [(v, n) for v, n, _ in first] == [(1, "001_initial")], first

        with contextlib.closing(sqlite3.connect(database)) as db:
            stored, = db.execute("SELECT password_hash FROM users WHERE username = 'keeper'")
            assert stored[0].startswith("$argon2id$") and PASSWORD not in stored[0]
            hashes = [row[0] for row in db.execute("SELECT token_hash FROM sessions")]
            assert len(hashes) == 1 and len(hashes[0]) == 32

        # the account survives a restart and the migration doesn't run twice
        with server(server_binary, certs, extra_args=with_db) as (port, process, _):
            with connect(port, certs) as peer:
                login(peer, "keeper")
        assert schema(database) == first

        # a database from a newer build must stop the server before it listens
        with contextlib.closing(sqlite3.connect(database)) as db:
            db.execute("INSERT INTO schema_migrations VALUES (99, '099_future', 0)")
            db.commit()
        completed = subprocess.run(
            server_args(server_binary, certs, free_port(), extra_args=with_db),
            capture_output=True, encoding="utf-8", timeout=10)
        assert completed.returncode == 1, completed.stdout
        assert "newer" in completed.stderr, completed.stderr

        # a path that can't be a database is a startup error, not a crash
        blocker = os.path.join(directory, "blocker")
        with open(blocker, "w") as handle:
            handle.write("not a directory")
        completed = subprocess.run(
            server_args(server_binary, certs, free_port(),
                        extra_args=["--db", os.path.join(blocker, "pulse.db")]),
            capture_output=True, encoding="utf-8", timeout=10)
        assert completed.returncode == 1, completed.stdout
        assert "Server: " in completed.stderr, completed.stderr

        # option parsing
        for bad in (["--db"], ["--bogus", "1"], ["5555", "--run-for"],
                    ["--password-cost", "free"]):
            completed = subprocess.run([server_binary] + bad, capture_output=True,
                                       encoding="utf-8", timeout=10)
            assert completed.returncode == 1 and "Usage" in completed.stderr, bad
    print("persistent accounts, migrations, restart, bad database, options: passed")


if __name__ == "__main__":
    server_binary, client_binary, certs_binary = sys.argv[1:]
    with tempfile.TemporaryDirectory() as certs_directory:
        certs = Certs(certs_binary, os.path.join(certs_directory, "certs"))
        test_network(server_binary, client_binary, certs)
        test_auth(server_binary, client_binary, certs)
        test_session_expiry(server_binary, certs)
        test_tls(server_binary, client_binary, certs)
        test_shutdown(server_binary, client_binary, certs)
        test_storage(server_binary, certs)

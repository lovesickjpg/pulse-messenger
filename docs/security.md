# Security

What the server and CLI protect, how, and what they don't do yet.

## Model

- Every connection is TLS (1.2 or newer). There's no plaintext mode.
- The client always verifies the server: certificate chain, validity dates, and
  that the certificate matches the name (or IP) you connected to. A failed check
  ends the connection before a single request goes out, so a password never
  reaches a server the client hasn't verified.
- The server does **not** check client certificates. Users prove who they are
  with a username and password, then a session.
- Who sent a request is decided by the server from the connection's session.
  Nothing in the JSON (like a `sender` field) is trusted as identity.
- The server reads message contents. There's no end-to-end encryption in the
  first release.
- The server still listens on 127.0.0.1 only.

## Setting up trust for local development

`pulse-dev-certs` makes a throwaway CA and a server certificate for `localhost`,
`127.0.0.1`, and `::1`:

~~~sh
./build/dev-debug/bin/pulse-dev-certs certs/local
./build/dev-debug/bin/pulse-server --cert certs/local/server.crt --key certs/local/server.key
./build/dev-debug/bin/pulse-client --ca certs/local/ca.crt
~~~

`certs/local/` and `*.key` are git-ignored. Add more names with `--host`
(`pulse-dev-certs certs/local --host pulse.test`). The tool refuses to replace an
existing CA unless you pass `--force`, because everyone trusting the old one
would have to switch.

The client trusts only the `--ca` file when you pass one, and the system store
otherwise. Don't add the dev CA to your system store: its private key sits in
plain text right next to it, so anyone who can read that folder could mint
certificates your machine would trust.

The same files with the OpenSSL CLI, if you'd rather:

~~~sh
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 365 \
  -keyout ca.key -out ca.crt -subj "/CN=Pulse development CA" \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign"
openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
  -keyout server.key -out server.csr -subj "/CN=localhost"
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -days 365 -out server.crt \
  -extfile <(printf "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1\nextendedKeyUsage=serverAuth")
~~~

The name you connect to must be in the certificate's subjectAltName; the CN is
ignored. `pulse-client --host 127.0.0.1` checks the IP entry, `--host localhost`
the DNS one.

## Passwords

- Hashed with Argon2id (`crypto_pwhash_str`) on a small background pool, never
  on the network thread. The salt and cost live inside the stored string.
- Cost: `--password-cost interactive` (default, 64 MiB per hash), `moderate`
  (256 MiB, slower), or `minimal` (tests only; the server warns about it).
- At most 32 hashes wait in line; past that, logins get `rate_limited` instead of
  piling up memory.
- Passwords are 8 to 256 bytes. The length check happens before hashing, so a
  giant "password" can't burn CPU.
- A login for a user that doesn't exist still runs a full hash check, and the
  error is word-for-word the same as for a wrong password.
- Five failed logins for a username within a minute block that username (from
  any connection) until the window slides. A successful login clears it.

## Sessions

- Logging in returns a random 256-bit token (`session_token`, 64 hex chars). The
  database stores only its BLAKE2b hash, plus the expiry and revocation time.
- Sessions last 12 hours by default (`--session-ttl seconds`).
- The CLI keeps the token in memory only. Closing it means logging in again.
- `auth.resume` binds another connection to an existing session (reconnects will
  use this).
- `auth.logout` revokes the session in the database and signs out every
  connection using it right away, before the database even answers.
- Each request checks the session's expiry. An expired session gets
  `unauthorized` and has to log in again.

## Logs

The server logs connects, disconnects, registrations, and logins by username.
It never logs passwords, tokens, or the contents of auth requests. The
integration test greps the server log to keep it that way.

## Not done yet

- Listening on anything but loopback, and a production certificate setup.
- Persistent client-side token storage (OS keychain).
- Per-IP limits and connection caps; those come with the recovery stage.
- No password echo suppression in the CLI: `/login name password` shows the
  password on screen.
- No end-to-end encryption.

-- Initial schema. Times are unix epoch milliseconds, IDs are SQLite rowids.
-- The migrator wraps this file in a transaction, so no BEGIN/COMMIT or PRAGMAs here.

CREATE TABLE users (
    id            INTEGER PRIMARY KEY,
    -- NOCASE only folds ASCII, which is fine until we add real name rules
    username      TEXT    NOT NULL UNIQUE COLLATE NOCASE CHECK (length(username) > 0),
    password_hash TEXT    NOT NULL CHECK (length(password_hash) > 0),
    created_at    INTEGER NOT NULL
) STRICT;

CREATE TABLE sessions (
    id         INTEGER PRIMARY KEY,
    user_id    INTEGER NOT NULL REFERENCES users (id) ON DELETE CASCADE,
    -- we only ever store a hash of the token, never the token itself
    token_hash BLOB    NOT NULL UNIQUE CHECK (length(token_hash) > 0),
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    revoked_at INTEGER
) STRICT;

CREATE INDEX sessions_by_user ON sessions (user_id);

CREATE TABLE conversations (
    id            INTEGER PRIMARY KEY,
    kind          TEXT    NOT NULL CHECK (kind IN ('direct', 'room')),
    name          TEXT    UNIQUE COLLATE NOCASE,
    -- "lowId:highId" so a pair of users can only ever have one direct dialog
    direct_key    TEXT    UNIQUE,
    -- highest sequence handed out so far, bumped in the same txn as the insert
    last_sequence INTEGER NOT NULL DEFAULT 0,
    created_at    INTEGER NOT NULL,
    CHECK ((kind = 'room' AND length(name) > 0 AND direct_key IS NULL)
        OR (kind = 'direct' AND name IS NULL AND direct_key IS NOT NULL))
) STRICT;

CREATE TABLE conversation_members (
    conversation_id INTEGER NOT NULL REFERENCES conversations (id) ON DELETE CASCADE,
    user_id         INTEGER NOT NULL REFERENCES users (id) ON DELETE CASCADE,
    role            TEXT    NOT NULL CHECK (role IN ('owner', 'member')),
    joined_at       INTEGER NOT NULL,
    PRIMARY KEY (conversation_id, user_id)
) STRICT, WITHOUT ROWID;

CREATE INDEX members_by_user ON conversation_members (user_id);

CREATE TABLE messages (
    id                INTEGER PRIMARY KEY,
    conversation_id   INTEGER NOT NULL REFERENCES conversations (id) ON DELETE CASCADE,
    sender_id         INTEGER NOT NULL REFERENCES users (id),
    client_message_id TEXT    NOT NULL CHECK (length(client_message_id) > 0),
    sequence          INTEGER NOT NULL CHECK (sequence > 0),
    body              TEXT    NOT NULL CHECK (length(body) > 0),
    created_at        INTEGER NOT NULL,
    -- the first one doubles as the history index
    UNIQUE (conversation_id, sequence),
    -- the dedupe key for retried sends
    UNIQUE (conversation_id, sender_id, client_message_id)
) STRICT;

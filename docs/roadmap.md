# Roadmap

Milestones describe intended work. An item is complete only after implementation,
relevant verification, review, and documentation updates.

| Milestone | Deliverable | Completion criterion |
| --- | --- | --- |
| M0 Foundation | Build, style, repository workflow | Both maintainers can build; the first PR passes CI |
| M1 Protocol | Versioned JSON contract and frame codec | Malformed, partial, combined, and oversized frames are tested |
| M2 Transport | Asynchronous server and CLI connection | Several local clients exchange framed messages; disconnects are handled |
| M3 Storage | Migrations and repositories | Data survives restart and failed transactions do not partially persist |
| M4 Identity | TLS, registration, login, session handling | Credentials travel over verified TLS; invalid access is rejected |
| M5 Messaging | Private chats, rooms, history | Only members access conversations; retries do not duplicate messages |
| M6 Recovery | Reconnect, replay, limits, shutdown | Interrupted delivery recovers without missing acknowledged history |
| M7 Release | Packaging, Docker, documentation | A fresh checkout and the published quick start reproduce the demo |

The first public version is planned as v0.1.0. Desktop UI, file transfer, calls,
and end-to-end encryption are later decisions.

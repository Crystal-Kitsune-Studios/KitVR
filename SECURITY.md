# Security Policy

## Supported Versions

KitVR is pre-release. Only the latest release (or the `main` branch before the first
release) receives security fixes.

| Version | Supported          |
| ------- | ------------------ |
| latest 0.x release | :white_check_mark: |
| older 0.x releases | :x:                |

This table will be updated once stable versions exist.

## Known Limitations

Previously released protocol-v1 builds do not authenticate or encrypt control or media
traffic. Anyone on the same network can connect to those servers. Do not expose their ports
(TCP 9943, UDP 9944) to the internet.

The current development protocol (v2) requires a 32-byte pairing key and uses HMAC-SHA-256
for authentication, HKDF-SHA-256 for session keys, and AES-256-GCM for control and media
traffic. Protocol-v1 clients are rejected without an insecure fallback. The implementation
is not yet released. Encrypted control, tracking, and video were verified end to end on a
Quest 2; audio and body-tracking media have not been verified. Discovery metadata and packet
timing and sizes remain visible. See `docs/PROTOCOL.md` for key storage and protocol details.

## Reporting a Vulnerability

Please do not open a public issue for security problems.

Report privately using either:

- GitHub's "Report a vulnerability" button on the repository's Security tab (preferred), or
- email: abuse@crystal-kitsune-studios.com

Please include what you found, the affected version or commit, steps to reproduce, and
the impact you expect. A proof of concept is helpful but not required.

What to expect:

- We aim to acknowledge your report within 7 days.
- We aim to send a status update at least every 14 days until it is resolved.
- If the report is accepted, we will work on a fix, agree a disclosure date with you, and
  credit you in the release notes unless you prefer to stay anonymous.
- If the report is declined, we will explain why.

This is a volunteer project, so these are targets, not guarantees.

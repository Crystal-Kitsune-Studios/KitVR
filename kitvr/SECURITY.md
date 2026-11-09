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

Protocol version 1 has no authentication or encryption on the control or media
channels. Anyone on the same network can connect to a running server. Until PIN pairing
and encrypted media land (see `docs/PROTOCOL.md`), run KitVR only on a trusted LAN and do
not expose its ports (TCP 9943, UDP 9944) to the internet. Reports about this specific
limitation are already tracked, so there is no need to submit them as new vulnerabilities.

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

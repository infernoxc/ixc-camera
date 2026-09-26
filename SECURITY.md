# Security policy

## Reporting a vulnerability

Please report security issues **privately** through GitHub's "Report a vulnerability" (Security Advisories) on this repository, not in a public issue. Include the IXC Camera version, your Windows build, and reproduction steps. Don't include camera recordings of people.

## Security commitments

IXC Camera:

- runs without administrator rights. Only the installer's camera registration step may require elevation;
- installs no kernel driver, disables no security feature, and changes no unrelated system setting;
- makes no network connections for camera processing, collects no telemetry, and needs no account;
- never executes code from effect packages or profiles. They are validated data (see `src/common/json.h` for parser limits);
- respects Windows camera privacy settings and never tries to bypass them;
- keeps logs free of secrets and personal content, with size-capped rotation.

## Release verification

Every release lists SHA-256 hashes for its artifacts, the exact commit, which scans were run, and whether the binaries are code-signed. We don't claim a release is "virus-free". We state exactly which checks were performed.

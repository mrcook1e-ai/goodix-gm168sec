# Documentation Hub (`goodix-gm168sec`)

This directory documents the **Goodix GM168SEC** (`27c6:589a`) experimental
[libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) overlay driver,
its `FPI_PRINT_SIGFM` matching integration, its read-only in-memory PSK
recovery subsystem, and the project's clean-room engineering boundaries.

> **Scope & Status Notice**
>
> - **Experimental community preview**: not production-ready and not yet
>   upstreamed into `libfprint`.
> - **Confirmed hardware / firmware**: USB `27c6:589a` running OEM firmware
>   `GFUSB_GM168SEC_APP_10036` on OpenSSL 3 (`>= 3.0, < 4.0`).
> - **Authentication safety**: always keep password fallback configured in PAM;
>   population-scale FAR/FRR, suspend/resume, and alternative firmware variants
>   remain unvalidated.

## Quick Links by Audience

| Audience | Recommended Reading Order | Key Focus |
|---|---|---|
| **End Users & Evaluators** | [`README.md`](../README.md) / [`README.ru.md`](../README.ru.md) → [`INSTALL.md`](../INSTALL.md) → [`FAQ.md`](FAQ.md) → [`SECURITY.md`](SECURITY.md) | English/Russian quick start, OpenSSL 3 build, Fedora 45 TLS note, private state permissions, limitations |
| **Driver & `libfprint` Developers** | [`ARCHITECTURE.md`](ARCHITECTURE.md) → [`STATE-MACHINES.md`](STATE-MACHINES.md) → [`IMAGE-PIPELINE.md`](IMAGE-PIPELINE.md) → [`SIGFM.md`](SIGFM.md) → [`TESTING.md`](TESTING.md) → [`DEVELOPMENT.md`](DEVELOPMENT.md) | `FpiSsm` async design, GLib/OpenSSL threading, `160x128` image flow, SIGFM core patch, test suites |
| **Security & Protocol Researchers** | [`PROTOCOL.md`](PROTOCOL.md) → [`PSK-RECOVERY.md`](PSK-RECOVERY.md) → [`REVERSE-ENGINEERING.md`](REVERSE-ENGINEERING.md) → [`SECURITY.md`](SECURITY.md) → [`../NOTICE`](../NOTICE) | `A0`/`B0`/`B2` wire framing, `0xE4` DPAPI/white-box verification gates, evidence hierarchy, exclusions |
| **AI Coding Agents & Maintainers** | [`../AGENTS.md`](../AGENTS.md) → [`DEVELOPMENT.md`](DEVELOPMENT.md) → [`TESTING.md`](TESTING.md) → [`../CONTRIBUTING.md`](../CONTRIBUTING.md) | Canonical overlay layout, patch regeneration, mandatory test/memory checks, safety prohibitions |

## Complete Document Index

### Repository Root Guides & Policies

- [`README.md`](../README.md) — Short English user guide: ready-made Fedora RPM installation, removal, current limits, and complete contributor credits.
- [`README.ru.md`](../README.ru.md) — Русская версия краткого руководства: установка готового RPM, удаление, ограничения и полный список участников.
- [`INSTALL.md`](../INSTALL.md) — Dependency matrix (OpenSSL 3, OpenCV 4, GLib, GUsb, doctest), isolated Meson build, local RPM build, optional udev rule, and private state setup.
- [`CHANGELOG.md`](../CHANGELOG.md) — Unreleased overlay changes, SIGFM migration, PSK recovery additions, and known limitations.
- [`CONTRIBUTING.md`](../CONTRIBUTING.md) — Safe bug-report checklist, code contribution rules, and upstream readiness expectations.
- [`SECURITY.md`](../SECURITY.md) — Sensitive artifact exclusions, runtime storage permissions (`0700`/`0600`), vulnerability reporting, and scope warning.
- [`NOTICE`](../NOTICE) — Provenance, licensing (`LGPL-2.1-or-later`), and attribution for `goodix-gm168`, `55b4-experimental`, and SIGFM contributors.
- [`AGENTS.md`](../AGENTS.md) — Operational rules, build/test contracts, review policy, and strict safety/secret boundaries for AI agents and maintainers.

### Technical Architecture & Subsystem Deep Dives (`docs/`)

| Document | Summary | Primary Source References |
|---|---|---|
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | Component boundaries, activation-to-verify data flow, GLib owner context vs TLS thread model, async USB, and state directory isolation | `driver/goodix_gm168.c`, `driver/goodix_proto.{c,h}`, `driver/goodix_tls.{c,h}`, `driver/gm168_psk_recovery.{c,h}` |
| [`STATE-MACHINES.md`](STATE-MACHINES.md) | `INIT`, `CAP`, `REARM`, `RECOVER`, and `DEINIT` `FpiSsm` lifecycles, `0x00`/`0x01`/`0x03` ACKs, coalesced packets, generation/deadline guards, fast path, and pre-TLS recovery placement | `driver/goodix_gm168.c`, `driver/gm168_timeouts.h`, `driver/gm168_usb_errors.h`, `driver/gm168_trace.h` |
| [`PROTOCOL.md`](PROTOCOL.md) | Concise wire-level summary of USB endpoints, packet types (`A0`/`B0`/`B2`), TLS 1.2 options, frame format, and SIGFM constants | `driver/goodix_proto.{c,h}`, `driver/goodix_tls.c` |
| [`PSK-RECOVERY.md`](PSK-RECOVERY.md) | Exact `psk.bin` vs `ENOENT` vs malformed/mismatch flow, `0xE4` TLV tags, dynamic DPAPI v1 length parser, white-box decryption/gates, in-memory boundary, and failure matrix | `driver/gm168_psk_recovery.{c,h}`, `driver/goodix_gm168.c`, `driver/goodix_tls.c` |
| [`IMAGE-PIPELINE.md`](IMAGE-PIPELINE.md) | `10564`-byte wire frame unpacking (`80x132` → `80x64` 12-bit), 5-frame dark background validation, dynamic fallback vs optional calibration, `2x` (`160x128`) scaling, quality gate, and dual capture | `driver/goodix_gm168.c`, `driver/gm168_log.{c,h}` |
| [`SIGFM.md`](SIGFM.md) | Rationale for replacing NBIS, SIFT + Lowe ratio + RANSAC affine pipeline, `SGFM` v1 / `FP3` integration, async extraction, 20-stage anti-dup, verify auto-retry, and FAR/FRR limits | `patches/libfprint-v1.94.100-integration.patch`, `driver/goodix_gm168.c` |
| [`REVERSE-ENGINEERING.md`](REVERSE-ENGINEERING.md) | Research history, static + dynamic + synthetic differential methodology, proven vs unproven boundaries, `GFUSB_GM168SEC_APP_10036` findings, and Windows re-provisioning behavior | `driver/goodix_proto.c`, `driver/gm168_psk_recovery.c`, [`../NOTICE`](../NOTICE) |
| [`TESTING.md`](TESTING.md) | Isolated build/apply steps, synthetic KAT/unit/negative suites, Valgrind/memory hygiene, experimental RPM `%check`, offline vs live proof boundary, and user hardware validation | `tools/apply-overlay.sh`, `tests/test_timeouts.c`, `packaging/fedora/libfprint-goodix-gm168.spec` |
| [`DEVELOPMENT.md`](DEVELOPMENT.md) | Overlay maintenance, integration patch regeneration excluding `driver/`, isolated RPM/release packaging, diagnostic environment variables, and upstream blockers | `tools/apply-overlay.sh`, `packaging/fedora/build-rpm.sh`, `patches/libfprint-v1.94.100-integration.patch` |
| [`SECURITY.md`](SECURITY.md) | Runtime `/etc/goodix-gm168` ownership checks, PSK lifecycle rules, sensitive diagnostic artifact bans, and advisory process | `driver/goodix_gm168.c`, `driver/gm168_log.c` |
| [`FAQ.md`](FAQ.md) | Troubleshooting missing/stale PSKs, Windows dual-boot rotation, optional calibration, Fedora 45 OpenSSL 3/TLS errors, template incompatibility, and safe bug reporting | [`SECURITY.md`](SECURITY.md), [`TESTING.md`](TESTING.md), [`PSK-RECOVERY.md`](PSK-RECOVERY.md) |

## Cross-Cutting Safety Principles

1. **No secrets or biometric data in git or issues**: never publish `psk.bin`, PSK hashes, DPAPI blobs, calibration tables, raw `.pgm` dumps, serialized `FpPrint` templates, packet captures, firmware images, or proprietary Windows driver binaries.
2. **Offline vs. live proof**: synthetic unit tests and KATs validate parser, cryptographic, and `libfprint` core logic without hardware; live `27c6:589a` verification is user-driven and constrained by limited hardware coverage.
3. **Read-only driver recovery**: missing-`psk.bin` recovery reads only bounded `0xE4` records, operates strictly in memory, and never writes `/etc` or sends destructive flash/erase/provisioning opcodes (`0xA4`, `0xE0`, `0xF0`, `0xF4`).

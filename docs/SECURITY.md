# Security Notes

See also the root [`SECURITY.md`](../SECURITY.md) policy.

## Private state directory and permissions

The driver reads runtime secrets and per-device state from `GOODIX_GM168_DIR` if
set, or `/etc/goodix-gm168` by default:

```text
/etc/goodix-gm168/            (root:root, mode 0700)
├── psk.bin                   (optional 32-byte backup, root:root, mode 0600)
├── fdt_thresholds.bin        (optional 6-byte driver-saved FDT state, mode 0600)
├── calib_windows.dat         (optional 140464-byte calibration block, mode 0600)
└── cal_scale.bin             (optional 10240-byte scale plane, mode 0600)
```

Before creating `fdt_thresholds.bin`, `gm168_ensure_private_dir` checks via
`lstat` that the directory is not a symlink, is owned by the effective UID, and
has no group or world permission bits (`0077 == 0`). PSK recovery performs no
filesystem writes and needs no `ReadWritePaths` exception. Only the optional FDT
state write may need such an exception under a restrictive systemd sandbox;
prefer a standard drop-in over weakening directory permissions.

## PSK lifecycle and Windows re-provisioning

- The 32-byte PSK is device-specific and is never compiled into driver code.
- If `psk.bin` is absent, bounded read-only recovery uses the existing key in
  memory after a fresh SHA-256 match. It does not persist the key, write a DPAPI
  blob, provision a replacement, or retry after failure.
- An existing unreadable, malformed, or mismatching `psk.bin` fails activation
  rather than being silently bypassed or overwritten.
- Re-enrolling or re-provisioning the fingerprint reader under Windows may
  overwrite the PSK in sensor flash. Keep a secure backup and deliberately move
  a known-stale `psk.bin` aside before requesting recovery of the current key.

## Sensitive diagnostics and artifacts

Never commit or upload to issues/PRs:

- `psk.bin`, PSK SHA-256 hashes, `sealed.bin`, or Windows DPAPI blobs;
- `calib_windows.dat`, `cal_scale.bin`, or `fdt_thresholds.bin`;
- `.pgm` / `.png` / `.raw` fingerprint images (including `GM168_DUMP_FRAMES`
  output) or serialized `FpPrint` templates;
- `GM168_LOG_DIR` logs, USB packet captures (`.pcapng`), or Windows binaries.

## Authentication policy

This driver is an experimental community preview with limited FAR/FRR testing
and no independent security audit. Always keep password authentication enabled
for login, `sudo`, and `polkit`.

Report vulnerabilities privately via GitHub security advisories on
`mrcook1e-ai/goodix-gm168sec` rather than public issues.

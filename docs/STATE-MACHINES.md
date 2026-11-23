# State Machines and Transport Guarantees

All USB operations in `driver/goodix_gm168.c` execute asynchronously via `libfprint` `FpiSsm` sequential state machines on the GLib owner context. This document details every state machine, packet ACK rule, generation/cancellation guard, watchdog deadline, and recovery boundary.

## Overview of State Machines

| State Machine | Trigger | Deadline (`driver/gm168_timeouts.h`) | Completion Handler | Purpose |
|---|---|---|---|---|
| `INIT` (`init_run_state`) | `dev_activate()` slow path | `10000 ms` (`GM168_INIT_DEADLINE_MS`) | `init_completed()` | Wakeup, reset, version, pre-TLS PSK check/recovery, TLS handshake, register setup, 5-frame dark BG capture, initial FDT arm |
| `CAP` (`capture_run_state`) | `poll_cb()` touch + settle delay (or retry/dual #2) | `2000 ms` (`GM168_CAPTURE_DEADLINE_MS`) | `capture_completed()` | Scan trigger (`0x20`), budget-bounded `EP_IN` receive, `B0`/`B2` TLS decryption, quality gate & dual-capture orchestration |
| `REARM` (`rearm_run_state`) | `capture_completed()` (normal submit, retry, or capture error) | `6000 ms` (`GM168_REARM_DEADLINE_MS`) | `rearm_completed()` | Dual `0x34` discharge, settle delay, `0xAE` IRQ arm, `0x32` FDT setup, bounded `wait_lift` finger-release loop |
| `RECOVER` (`recover_run_state`) | `rearm_completed()` when `REARM` fails | `5000 ms` (`GM168_RECOVER_DEADLINE_MS`) | `recover_completed()` | Mid-session level-2 sensor recovery (`0x60` → `0xAE` → `0x32` → REARM chain) preserving active TLS & cached BG |
| `DEINIT` (`deinit_run_state`) | `dev_deactivate()` | `3000 ms` (`GM168_DEINIT_DEADLINE_MS`) | `deinit_completed()` | Completes deactivation cleanly without issuing `0xD6` power-off so TLS state survives across enroll stages |

## `INIT` State Machine

```mermaid
stateDiagram-v2
    [*] --> INIT_WAKEUP : Slow path in dev_activate()
    INIT_WAKEUP --> INIT_WAKEUP_DELAY : 0x11 TX
    INIT_WAKEUP_DELAY --> INIT_RESET : 50 ms
    INIT_RESET --> INIT_RESET_ACK : 0x60 (01 00)
    INIT_RESET_ACK --> INIT_VERSION : ACK 00/01/03
    INIT_VERSION --> INIT_VERSION_ACK : 0x20 (empty)
    INIT_VERSION_ACK --> INIT_PSK_GATE : ACK 00/01/03

    INIT_PSK_GATE --> INIT_PSK_HASH_SEND : Both psk.bin & ENOENT paths

    INIT_PSK_HASH_SEND --> INIT_PSK_HASH_RECV : 0xE4 tag 0xbb020001 (32 B)
    INIT_PSK_HASH_RECV --> INIT_TLS_START : psk_loaded && hash match
    INIT_PSK_HASH_RECV --> INIT_PSK_READ_SEND : !psk_loaded && non-zero hash OK
    INIT_PSK_HASH_RECV --> Failed : mismatch / invalid / zero sentinel

    INIT_PSK_READ_SEND --> INIT_PSK_READ_RECV : 0xE4 tag 0xbb010002 (<=256 B)
    INIT_PSK_READ_RECV --> INIT_PSK_READ_SEND : NEED_MORE & reads < 16
    INIT_PSK_READ_RECV --> INIT_TLS_START : DPAPI+TLV valid & SHA256 match
    INIT_PSK_READ_RECV --> Failed : DPAPI/TLV/HMAC/IV/hash/budget error

    INIT_TLS_START --> INIT_TLS_START_ACK : 0xD0 (00 00)
    INIT_TLS_START_ACK --> INIT_TLS_RX
    INIT_TLS_RX --> INIT_TLS_DELAY : RX B0 fed to OpenSSL
    INIT_TLS_RX --> INIT_TLS_RX : RX timeout (!tls_done)
    INIT_TLS_DELAY --> INIT_TLS_TX_PULL : 10 ms
    INIT_TLS_TX_PULL --> INIT_TLS_TX_PULL : TX B0 flight (out_n > 0)
    INIT_TLS_TX_PULL --> INIT_TLS_RX : out_n == 0 && !tls_done
    INIT_TLS_TX_PULL --> INIT_SET_PARAM : tls_done == TRUE
    INIT_TLS_RX --> INIT_SET_PARAM : tls_done == TRUE

    INIT_SET_PARAM --> INIT_SET_PARAM_ACK : 0x90 (256 B)
    INIT_SET_PARAM_ACK --> INIT_DEL_TMPL_1
    INIT_DEL_TMPL_1 --> INIT_DEL_TMPL_1_ACK : 0xC4 (01 00)
    INIT_DEL_TMPL_1_ACK --> INIT_DEL_TMPL_2
    INIT_DEL_TMPL_2 --> INIT_DEL_TMPL_2_ACK : 0xC4 (01 00)
    INIT_DEL_TMPL_2_ACK --> INIT_SESSION
    INIT_SESSION --> INIT_SESSION_ACK : 0x60 (01 00)
    INIT_SESSION_ACK --> INIT_D6_POST
    INIT_D6_POST --> INIT_D6_POST_ACK : 0xD6 (00 00)
    INIT_D6_POST --> INIT_ARM : Skip 0xD6 if bg_retry_count > 0
    INIT_D6_POST_ACK --> INIT_ARM
    INIT_ARM --> INIT_ARM_ACK : 0xAE (00 01 00)
    INIT_ARM_ACK --> INIT_FDT
    INIT_FDT --> INIT_FDT_ACK : 0x32 (35 B)
    INIT_FDT_ACK --> INIT_BG_TRIG

    INIT_BG_TRIG --> INIT_BG_RX : 0x20 capture trigger
    INIT_BG_RX --> INIT_BG_PROCESS : stitch_buf >= 10564 B
    INIT_BG_PROCESS --> INIT_BG_RX : img_len < 10564 B
    INIT_BG_PROCESS --> INIT_BG_LOOP_CHECK : decoded frame accumulated
    INIT_BG_LOOP_CHECK --> INIT_BG_TRIG : bg_frames_captured < 5
    INIT_BG_LOOP_CHECK --> INIT_BG_REARM_34_1 : 5 frames averaged & mean >= min_mean
    INIT_BG_LOOP_CHECK --> INIT_BG_REARM_34_1 : bg_mean < min_mean (sets bg_waiting_lift)

    INIT_BG_REARM_34_1 --> INIT_BG_REARM_34_2 : 0x34 + ACK (x2)
    INIT_BG_REARM_34_2 --> INIT_BG_REARM_DELAY : 150 ms
    INIT_BG_REARM_DELAY --> INIT_BG_REARM_AE : 0xAE + ACK
    INIT_BG_REARM_AE --> INIT_BG_REARM_32 : 0x32 + ACK
    INIT_BG_REARM_32 --> INIT_BG_WAIT_LIFT
    INIT_BG_WAIT_LIFT --> [*] : !bg_waiting_lift -> init_completed
    INIT_BG_WAIT_LIFT --> INIT_BG_REARM_32 : Quick-tap consumed (<= 8 rearms)
    INIT_BG_WAIT_LIFT --> INIT_SESSION : Finger lifted / cap -> retry BG
```

### Why PSK Recovery Lives Before TLS (and Why TLS Failure Never Triggers Recovery)

1. **Pre-TLS dependency**: `goodix_gm168_tls_init()` registers `psk_server_cb()` (`driver/goodix_tls.c`), which reads the 32-byte `goodix_gm168_psk` buffer as soon as the sensor sends its TLS `ClientHello` after `INIT_TLS_START` (`0xD0`). Both loading `psk.bin` and recovering from `0xE4` (`0xbb020001` + `0xbb010002`) therefore run in `INIT_PSK_GATE` **before** `0xD0` is ever sent.
2. **Pre-TLS cryptographic proof**: before `INIT_TLS_START` runs, the active key (whether loaded from `psk.bin` or recovered from `0xbb010002` + `0xbb010003`) has **already** matched the sensor's fresh `0xbb020001` SHA-256 reply in constant time (`CRYPTO_memcmp`).
3. **Why TLS failure never invokes recovery**: if `SSL_accept()` subsequently fails, the PSK is already known to match the MCU's `0xbb020001` digest. A failure at that stage indicates an OpenSSL provider/cipher-policy mismatch (e.g., missing `OPENSSL_CONF` for legacy `PSK-AES128-CBC-SHA256` on Fedora 45), a stale MCU TLS state from an unreset prior session, or USB framing loss—none of which can be fixed by re-reading `0xE4`. Falling back to recovery after TLS failure would also violate the fail-closed policy when an existing `psk.bin` mismatches.

## `CAP`, `REARM`, `RECOVER`, and `DEINIT` State Machines

```mermaid
stateDiagram-v2
    state "Touch Polling (EP_IN 0x83)" as POLL
    state "Capture Settle Timer (150 ms default)" as SETTLE

    POLL --> SETTLE : A0 touch (echo=0x32, status=0x02)
    SETTLE --> CAP_TRIG : report_finger_status(TRUE)

    state CAP {
        [*] --> CAP_TRIG
        CAP_TRIG --> CAP_RX : 0x20 scan trigger
        CAP_RX --> CAP_PROCESS : USB RX received
        CAP_RX --> CAP_RX : Timeout #1
        CAP_RX --> CAP_TRIG : Consecutive timeout == 2 (re-trigger)
        CAP_PROCESS --> CAP_RX : img_len < 10564 B
        CAP_PROCESS --> [*] : img_len >= 10564 B -> capture_completed()
    }

    CAP --> CAP_TRIG : Dual #1 complete -> start Dual #2 (no REARM)
    CAP --> REARM_34_1 : Quality-gate retry (rd->retry = TRUE)
    CAP --> REARM_34_1 : Dual #2 complete / error fallback -> submit best

    state REARM {
        [*] --> REARM_34_1
        REARM_34_1 --> REARM_34_1_ACK : 0x34
        REARM_34_1_ACK --> REARM_34_2
        REARM_34_2 --> REARM_34_2_ACK : 0x34
        REARM_34_2_ACK --> REARM_DELAY
        REARM_DELAY --> REARM_AE : 150 ms (submit) / 200 ms (retry)
        REARM_AE --> REARM_AE_ACK : 0xAE
        REARM_AE_ACK --> REARM_32
        REARM_32 --> REARM_32_ACK : 0x32
        REARM_32_ACK --> REARM_WAIT_LIFT
        REARM_WAIT_LIFT --> [*] : rd->retry || rd->fdt_rearm_only || lift detected
        REARM_WAIT_LIFT --> REARM_32 : FDT touch consumed (rearm32_count < 8)
    }

    REARM --> CAP_TRIG : rearm_completed() with rd->retry == TRUE
    REARM --> POLL : rearm_completed() -> submit image, finger_status(FALSE)
    REARM --> RECOVER : rearm_completed(error) -> escalate

    state RECOVER {
        [*] --> RECOVER_SESSION : 0x60 -> 0xAE -> 0x32 -> 0x34x2 -> 150ms -> 0xAE -> 0x32
        RECOVER_SESSION --> [*] : Preserves TLS & cached background
    }

    RECOVER --> POLL : RECOVER OK && !capture_error
    RECOVER --> Failed : capture_error present or RECOVER failed

    state DEINIT {
        [*] --> DEINIT_POWER : Completes immediately (no 0xD6 power-off)
    }
```

### Key `CAP` / `REARM` / `RECOVER` Mechanics

- **Budget-bounded `CAP_RX`**: each `CAP_RX` USB read uses `min(GM168_USB_RX_SHORT_TIMEOUT_MS, max(50, GM168_CAPTURE_BUDGET_MS - elapsed_ms))` (`1000 ms` max, `600 ms` total touch budget, `50 ms` floor) so a quiet sensor cannot stall past the budget.
- **Re-trigger on consecutive timeouts**: after `GM168_CAP_RX_RETRIG_AFTER` (`2`) consecutive `CAP_RX` timeouts, `capture_rx_cb()` jumps back to `CAP_TRIG` once in case the sensor dropped the scan trigger.
- **1-byte framing resync**: in `CAP_PROCESS`, encountering an unknown packet type byte drops **only 1 byte** from `self->stitch_buf` rather than wiping the whole buffer, preserving trailing valid packets.
- **Bounded `wait_lift_cb()`**: polls `EP_IN` with `gm168_wait_lift_poll_ms()` (`150 ms` default, configurable `50..1000 ms` via `$GM168_WAIT_LIFT_POLL_MS`).
  - If the finger stays on or quick-taps during `wait_lift`, FDT fires (`echo=0x32, status=0x02`) and sets `rd->consumed_touch = TRUE`. On the subsequent timeout, `wait_lift_cb()` jumps back to `REARM_32` to re-arm FDT (otherwise `start_polling()` would listen on a disarmed FDT).
  - Re-arms are capped at `GM168_WAIT_LIFT_MAX_REARMS` (`8`), after which `rd->fdt_rearm_only = TRUE` triggers one final `REARM_32` and exits `REARM_WAIT_LIFT` immediately.
  - `wait_lift_cb()` checks `gm168_ssm_deadline_expired()` **on every callback** (not only on state transitions) so continuous finger holding cannot loop indefinitely without hitting `GM168_REARM_DEADLINE_MS` (`6000 ms`).

## ACK Semantics (`0x00`, `0x01`, `0x03`) and Coalesced Packets

### Two `A0` Response Shapes

`ack_cb()` in `driver/goodix_gm168.c` handles both `A0` response layouts emitted by Goodix firmware:

1. **Inner-`0xB0`-marked ACK** (`packet_len >= 10 && packet[4] == GOODIX_GM168_PKT_TLS` (`0xB0`)):
   - `echo_cmd = packet[7]`, `status = packet[8]`.
   - Seen on `GFUSB` firmware (and as the immediate ACK preceding `0xE4` data responses).
2. **Direct-echo `A0` packet** decoded via `goodix_gm168_decode_ack()` (`driver/goodix_proto.c`):
   - `echo_cmd = packet[4]`, `status = packet[7]`, optional `extra = packet + 8` of length `inner_len - 5`.
   - Also carries `0x34` dark baseline readings (`gm168_calibrate_fdt()` extracts 6 channels from bytes `11..22` when `status == 0x00`) and `0x32` touch notifications (`status == 0x02`).

### Accepted ACK Status Codes

| Constant | Byte | Meaning | Behavior in `ack_cb()` |
|---|---:|---|---|
| `GOODIX_GM168_STATUS_OK` | `0x00` | Command succeeded | Advances SSM if `echo == expected_ack_cmd` |
| `GOODIX_GM168_STATUS_BUSY` | `0x01` | Immediate ACK / busy flag | Accepted by `ack_cb()`; in `0xE4` probes, `find_b0_ack()` triggers at most 1 continuation read |
| `GOODIX_GM168_STATUS_MORE` | `0x03` | `GFUSB` continuation flag (more response packets follow) | Accepted by `ack_cb()` and `find_b0_ack()` (`GFUSB_GM168SEC_APP_10036` emits `0x03` during init) |
| `GOODIX_GM168_STATUS_TOUCH` | `0x02` | Finger detected (on `echo=0x32`) | Ignored as command ACK in `ack_cb()`; triggers capture in `poll_cb()` |

Any other status byte fails the SSM immediately (`FP_DEVICE_ERROR_PROTO`).

### Coalesced Bulk Transfers & `0xE4` Single-Continuation Rule

- A single `16 KiB` `EP_IN` bulk transfer can contain multiple concatenated packets (`while (offset + 4 <= transfer->actual_length)`).
- `ack_cb()` walks every packet in the transfer:
  - Evaluates `0xA0` packets for `got_immediate` (ignoring `0x32`/`0x02` touch events).
  - Feeds any `0xB0`/`0xB2` packets in the same bulk transfer into `process_rx_buffer_for_tls()` so coalesced TLS handshake bytes are never dropped.
  - Advances only after inspecting the full transfer; if no matching ACK was seen, `ack_resubmit_or_fail()` re-submits up to `GM168_USB_RX_RETRY_LIMIT` (`60`).
- **Strict `0xE4` reply handling (`psk_hash_rx_cb`, `psk_read_rx_cb`)**:
  - First searches the entire coalesced transfer via `find_a0_ack()` for `echo == 0xE4`.
  - If absent, ONLY if `!deactivating && ack_retry == 0 && find_b0_ack(..., 0xE4)` detects an immediate `0x01`/`0x03` ACK, submits **one** continuation bulk read (`ack_retry = 1`).
  - Never re-sends `0xE4` and never loops past that single continuation read (`ack_resubmit_or_fail` is deliberately bypassed).

## Cancellation, Generation Guards, and Deadlines

### Generation & Cancellation Guards

- **Monotonic generation counter (`self->generation`)**:
  - Incremented at the start of both `dev_activate()` and `dev_deactivate()`.
  - Every `FpiSsm` stores `generation` in its `SsmData` / `RearmData` / `RecoverData` struct; `poll_cb()` and `capture_after_settle_cb()` receive `self->generation` via `user_data`.
  - `gm168_ssm_is_current()` and `gm168_ssm_callback_guard()` check `data->generation == self->generation` at the top of every `*_run_state` and USB callback; stale callbacks free any transfer error and fail with `G_IO_ERROR_CANCELLED`.
  - `gm168_activate_complete()` checks `self->activation_pending && generation == self->activation_generation` so deactivation mid-init cannot double-complete activation.
- **Shared `io_cancellable`**:
  - Created fresh in `dev_activate()`, wired to `fpi_device_get_cancellable()` via `action_cancelled_cb()`, and cancelled + unrefed in `dev_deactivate()`, aborting any pending `EP_IN`/`EP_OUT` transfer immediately.
- **USB Error Classification (`driver/gm168_usb_errors.h`)**:
  - `gm168_classify_usb_error()` categorizes `GError` into `OTHER`, `CANCELLED`, `TIMEOUT`, `NO_DEVICE`, and `IO`.
  - `gm168_handle_fatal_usb_error()` consumes `NO_DEVICE` across all SSM USB callbacks, sets `self->device_dead = TRUE`, and emits `FP_DEVICE_ERROR_REMOVED`.

### Timing and Deadline Contract (`driver/gm168_timeouts.h`)

| Constant | Value | Role |
|---|---:|---|
| `GM168_USB_RX_TIMEOUT_MS` | `2000 ms` | Standard `EP_IN` ACK and `INIT_BG_RX` read timeout |
| `GM168_USB_RX_SHORT_TIMEOUT_MS` | `1000 ms` | `INIT_TLS_RX`, `CAP_RX` ceiling, polling, and `REARM` `0x34`/`0xAE` ACKs |
| `GM168_USB_RX_LONG_TIMEOUT_MS` | `3000 ms` | `INIT_FDT_ACK` (`0x32`) |
| `GM168_USB_TX_TIMEOUT_MS` | `2000 ms` | `EP_OUT` bulk transfer timeout |
| `GM168_WAKEUP_DELAY_MS` | `50 ms` | Post-`0x11` wakeup delay (no MCU ACK) |
| `GM168_TLS_PUMP_DELAY_MS` | `10 ms` | Pacing between `INIT_TLS_RX` and `INIT_TLS_TX_PULL` |
| `GM168_REARM_DELAY_MS` | `150 ms` | Capacitive front-end discharge delay between `0x34` and `0xAE` on submit |
| `GM168_REARM_RETRY_DELAY_MS` | `200 ms` | Delay during quality-gate capture retry (`driver/goodix_gm168.c`) |
| `GM168_CAPTURE_SETTLE_MS` | `150 ms` | Pre-capture finger settle delay (`$GM168_CAPTURE_SETTLE_MS`, `100..2000`) |
| `GM168_WAIT_LIFT_POLL_DEFAULT_MS` | `150 ms` | Default `wait_lift_cb` poll (`$GM168_WAIT_LIFT_POLL_MS`, `50..1000`; `gm168_timeouts.h` static assert uses `300 ms` worst-case bound) |
| `GM168_USB_RX_RETRY_LIMIT` | `60` | Per-state consecutive `ack_cb` / `bg_rx_cb` re-listen cap |
| `GM168_TLS_RETRY_LIMIT` | `60` | Max `INIT_TLS_RX` iterations |
| `GM168_WAIT_LIFT_MAX_REARMS` | `8` | Max `wait_lift_cb` → `REARM_32` loops before forced exit |
| `GM168_INIT_DEADLINE_MS` | `10000 ms` | Wall-clock deadline for `INIT` SSM |
| `GM168_CAPTURE_DEADLINE_MS` | `2000 ms` | Wall-clock deadline for `CAP` SSM (`> GM168_CAPTURE_BUDGET_MS` `600 ms`) |
| `GM168_REARM_DEADLINE_MS` | `6000 ms` | Wall-clock deadline for `REARM` SSM |
| `GM168_RECOVER_DEADLINE_MS` | `5000 ms` | Wall-clock deadline for `RECOVER` SSM |
| `GM168_DEINIT_DEADLINE_MS` | `3000 ms` | Wall-clock deadline for `DEINIT` SSM |

Compile-time `_Static_assert` checks in both `driver/gm168_timeouts.h` and `tests/test_timeouts.c` prevent silent drift across these invariants.

## Fast Path Across Enroll Stages

`fprintd` cycles `dev_deactivate()` → `dev_activate()` between every enrollment stage (`20` stages for GM168) and across open/close bursts while the daemon stays resident:

- **Sensor behavior**: the MCU keeps its TLS session alive across `deactivate`/`activate`; sending a second `0xD0` (`TLS_START`) without a USB device reset leaves the MCU silent on `INIT_TLS_RX` until the watchdog trips.
- **Fast path condition** (`dev_activate()` in `driver/goodix_gm168.c`):
  ```c
  if (self->tls.ssl != NULL && self->tls_done && self->background != NULL)
  ```
  When true, `dev_activate()` skips USB reset, PSK loading/recovery, TLS handshake, `SET_PARAM`, and the 5-frame dark background capture, completing activation immediately (`gm168_activate_complete(self, self->generation, NULL)`).
- **Persistence across deactivate/close**: `dev_deactivate()` intentionally does **not** call `goodix_gm168_tls_cancel()`, and `dev_close()` keeps both the TLS session and USB interface claim alive until process exit or `fpi_device_goodix_gm168_finalize()`.
- **Slow path reset**: on cold start or error teardown, `dev_activate()` executes a USB `release_interface` → `reset` → `claim_interface` sequence so any stale TLS session from an earlier process is cleared before `INIT` runs.

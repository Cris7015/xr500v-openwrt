# GPON bring-up — phase 15 reset-then-LOS observer (compile-only)

Date: 2026-07-13

## Purpose

Phase 14 completed the reset-only question, so this observer answers a distinct
one: does an OEM software reset immediately before the isolated LOS receiver
sequence change the non-transactional phase-10 result?

The proven reset-audit package gained a mutually-exclusive
`arm_en7570_reset_then_los` mode.  It has not yet been deployed in this phase.

## Fixed sequence

1. require the exact phase-12 28-group baseline and all phase-14 gates;
2. self-pin and submit the OEM four-byte SW_RESET RMW once;
3. capture the complete immediate post-reset state and repeat TX postflight;
4. cancel LOS if reset, snapshot or postflight reports any error;
5. wait 10–12 ms, exceeding the 1–2 ms bandgap delay in merbanan's model;
6. apply only the five OEM LOS writes to `SVADC_PD`, `LOS_CTRL1` and
   `LOS_CTRL2`, using the per-unit `0x1c/0x10` thresholds from DT;
7. wait 20–25 ms, capture every group again and repeat TX postflight;
8. remain pinned with GPIO16 asserted; make no rollback claim.

The observer contains no ADC/RSSI sampling, DDMI, MPD, ERC, TGEN, APD, current,
MAC, QDMA, interrupt or MMIO write path.  Reset-only and reset-then-LOS modes
cannot be selected together.

## Audit

```text
package release:   2
module SHA-256:    3eb48c5a75011b969c30bd771d2d7fd95fccdf2db4278793e1ae0938cd52085d
checkpatch:        0 errors, 0 warnings (533 lines)
kernel:            6.12.80
autoload:          none
shipping DT match: none
router deployment: none
```

The next action is the same full temporary-image audit used in phase 14,
followed by one fibre-disconnected execution and mandatory physical power-cycle
recovery.


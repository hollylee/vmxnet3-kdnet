# One-shot recovery test

The separate `fault-once` variant is **0.3.3.0**, with description
**`vmxnet3-kdnet fault-once (manual reset, TEST ONLY)`**. It boots normally and
does nothing unusual until explicitly armed in WinDbg. It is a diagnostic
variant for the VM. Both previous descriptor triggers completed on VMware
without a captured queue error. The current test explicitly calls the existing
recovery routine; it tests reset/reinitialization and continued debugging.
It cannot establish device queue-error detection. The first explicit reset
succeeded on the ARM64 VM, with WinDbg continuing automatically. All five
post-reset break/resume cycles and checksum-verified transfers through kdnic
in both directions passed. This explicit-reset checkpoint is complete;
device queue-error detection remains unverified.

## Preserved baseline and build

The signed v0.3.0.0 DLLs, matching PDBs, manifest and build sources were copied
to `artifacts/baseline-v0.3.0.0/`. Their hashes were checked against the existing
transport manifest. The files in `out/<arch>/transport/` were also preserved.
Use these commands to build and sign only the new variant:

```bat
build.cmd all test fault-only
sign.cmd windows fault-once
```

The normal build does not emit `fault-once`. The fault-only build leaves the
baseline DLL/PDB outputs alone; the filtered signing command signs only
`fault-once`. Unfiltered build/sign commands retain their previous behavior
and can replace/re-sign other outputs. The baseline archive is outside `out/`.

Both architectures pass the initialization/checkpoint, normal packet and
fault-variant tests, with DLL metadata checks (no imports, one export and
relocations). The fault simulation covers explicit arming, asynchronous-only
triggering, recovery while RX/TX buffers are retained, continued traffic and
ring wraps, rejection of repeat injection, full controller reinitialization,
accepted chains (including payload reconstruction across ring wraps), buffer
reservation pressure, pending TX, watchdog timeouts, failed activation and
failed cleanup. The emulator's rejection of the 17-descriptor packet is a
test assumption; only the VMware run establishes the real device response.
The explicit-reset tests also cover retained RX/TX buffers, outstanding TX,
the transient send failure returned to KDNET, continued traffic across ring
wraps, synchronous/invalid sends leaving the arm pending, repeat prevention,
full controller reinitialization, and failed activation/cleanup.
Build output is saved
in `out/fault-build.log`, and signed artifact hashes in `out/fault-manifest.json`.

## Current test: explicitly requested reset

Arm with **`0x52535431`**. On the next valid asynchronous send, after normal
device-event/completion processing, the module consumes the arm value and
calls `VmxRecover` with test stage `0x15`, detail 1. This performs the existing
quiesce, DMA stop, ring reset, configuration and activation sequence. The
current unsent packet is released and the send returns `STATUS_IO_TIMEOUT`,
as the normal event-recovery path does. Other caller-held RX/TX buffers survive;
previously posted TX packets are discarded by the reset.

The diagnostic record uses **`FaultOutcome=6`** (explicit reset),
`FaultDescriptors=0`, and zero captured device-error fields. It never writes
fake ECR or queue errors. A successful attempt records `FaultRecoveryDone=1`,
`FaultRecoveryResult=0`, `Running=1`, and `Restarts=FaultRestartsBefore+1`.
The main diagnostic result remains **`0xE2150001`**, with `LastStage=0x15`
and `LastDetail=1`, to identify why recovery was invoked.

Synchronous debugger traffic alone cannot trigger it. The one-shot guard is
shared with the older descriptor test: a consumed test cannot be followed by
another test in the same controller lifetime. A full controller initialization
clears the record and disarms it. All test logic is compiled out of the normal
transport. Do not edit context fields to bypass the guard.

## Retained descriptor test (not the next test)

The older arm value `0x5146554c` still selects the 17-descriptor test below.
It completed without a captured queue error on the tested Fusion VM; use
`0x52535431` for the next explicitly requested reset instead.

After arming, an otherwise valid asynchronous TX submission is split across
17 descriptors, exceeding the non-TSO limit of 16 in the BSD DPDK v24.11
reference `third_party/dpdk/vmxnet3_defs.h`; revision 1 is negotiated. The
Ethernet frame contents and total length stay unchanged. Each descriptor
references a distinct valid, page-contained DMA buffer. The first fragment
contains all but the final 16 bytes, followed by sixteen one-byte fragments;
only the final descriptor sets EOP and requests completion. There are no
fabricated device events or queue-status writes.

Injection waits for an empty TX ring and room to reserve 16 extra buffers
within the usual ownership limit. Otherwise that submission proceeds normally
and the arm value remains pending. Caller-held buffers are preserved. The
normal completion path frees all 17 buffers if VMware accepts the chain.

The expected device response is a stopped TX queue and an error event, often
`0x80000002` (`VMXNET3_ERR_BIG_PKT`: too many descriptors for a packet).
The exact VMware response is not
assumed. A completion without an error or a timeout-only reset is recorded
as inconclusive for this test, rather than counted as queue-error recovery.

The trigger consumes its arm value and fires at most once per controller
initialization. Queue recovery preserves the injection record, so it cannot
fire again even if the arm value is rewritten. A full controller initialization
clears the record and disarms the trigger. Synchronous debugger traffic never
triggers it. While the injected packet is pending, device event status is
polled on every packet service call so a queue event can be captured before
the existing TX watchdog fires. The normal transport compiles out all test
fields, the arm variable and injection logic.

## Install and arm on the ARM64 VM

1. Keep the known working baseline and a recoverable VM snapshot/console.
   Copy `out/arm64/fault-once/kd_02_15ad.dll` into the target's
   `C:\Windows\System32`. Put its matching PDB in a symbol directory on the
   WinDbg host. Preserve the existing debugger IP, key, port and PCI settings.
2. Save/clear the old KDNET registry diagnostics and reboot using Startup
   Settings option 7 as in [transport.md](transport.md). Connect WinDbg and
   confirm normal break/resume before arming.
3. Run `lmDvm kd_02_15ad`. Verify **v0.3.3.0 and matching private PDB symbols**.
   If WinDbg omits the version-resource strings, check the installed file's
   version/description and compare its hash with `out/fault-manifest.json`;
   the context type must include `FaultDescriptors` and `FaultLastSlot`.
   If needed, add the folder containing the new PDB
   using `.sympath+ <folder>` and reload with `.reload /f kd_02_15ad.dll`.
   Do not use the baseline PDB for this variant.
4. While broken in, save an initial context reading:

   ```text
   r @$t0 = poi(poi(kd_02_15ad!VmxSharedData))
   dt kd_02_15ad!_VMX_CONTEXT @$t0 Running Result LastStage LastDetail Restarts
   dt kd_02_15ad!_VMX_CONTEXT @$t0 Fault*
   ```

   Expect `Running=1`, `Result=0`, and all `Fault*` fields zero. If an unrelated
   fault is already recorded, capture it before attempting this test.
5. Arm the test, then continue Windows:

   ```text
   ed kd_02_15ad!VmxFaultArm 0x52535431
   g
   ```

   This intentional target-memory write selects the explicit-reset test.
   Background OS traffic may trigger immediately
   once Windows runs. Otherwise generate traffic from the VM console through
   kdnic, such as a small copy to macOS over the already validated path. No
   large transfer is needed. Synchronous WinDbg commands alone will not trigger.
6. Leave the existing WinDbg session open. Allow up to a minute, attempt a
   break, and capture:

   ```text
   r @$t0 = poi(poi(kd_02_15ad!VmxSharedData))
   dt kd_02_15ad!_VMX_CONTEXT @$t0 Fault*
   dt kd_02_15ad!_VMX_CONTEXT @$t0 Running Result LastStage LastDetail
   dt kd_02_15ad!_VMX_CONTEXT @$t0 Restarts TxPackets RxPackets RxDropped TxTimeouts LastTxError LastRxError
   ```

   Save `check-target.ps1` and relevant `vmware.log` output too. The registry
   initialization status is not the live runtime-fault record.
7. If debugging recovered, verify five break/resume cycles and a checksum-verified
   copy in each direction, then read the counters again. Record whether the
   debugger recovered automatically, required reattaching, or failed entirely.
   Do not re-arm; `FaultCount` must remain 1 during this controller lifetime.

If the debugger does not recover, use the VM console to save available logs
and restore the baseline DLL, then reboot with option 7; revert the snapshot
if console recovery is unavailable. After the test, restore the baseline even
on success. A diagnostic fault variant is not the normal deployment artifact.

## Interpret the explicit-reset record

Require `FaultCount=1`, `FaultOutcome=6`, `FaultRecoveryStage=0x15`,
`FaultRecoveryDone=1`, `FaultRecoveryResult=0`, `Running=1`, and
`Restarts=FaultRestartsBefore+1`. Device-error fields and `FaultDescriptors`
remain zero. Verify five break/resume cycles and checksum-verified copies in
both directions after the reset. Record whether reconnection was automatic
or required host intervention. This validates an explicitly requested reset
on this VM; device queue-error detection remains a separate unverified path.

If `FaultCount=0`, read `dd kd_02_15ad!VmxFaultArm L1` and check asynchronous
traffic. If `FaultRecoveryDone=0`, no completed recovery result is available;
zero `FaultRecoveryResult` alone is not success. If the result is nonzero or
`Running=0`, preserve the record and follow the baseline restoration steps.

## Interpret the retained descriptor-test record

| Field | Meaning / passing evidence |
|---|---|
| `FaultCount` | Exactly 1 injection was posted |
| `FaultSlot`, `FaultBuffer` | First injected TX descriptor slot and backing buffer |
| `FaultDescriptors`, `FaultLastSlot` | 17 (`0x11`) descriptors; final slot is `(FaultSlot + 16) & 31` |
| `FaultWord2`, `FaultWord3` | First descriptor words: fragment length plus generation bit; word 3 is 0 |
| `FaultOutcome` | 2 for a device queue-error event observed while injection was pending |
| `FaultEvents` | Captured device ECR; TX-error bit `0x1` should be set |
| `FaultTxError`, `FaultTxStopped` | Device queue status before reset: nonzero error and stopped state |
| `FaultRxError`, `FaultRxStopped` | RX queue status captured at the same time |
| `FaultRecoveryStage` | `0x0E` for device queue-error recovery |
| `FaultRecoveryDone` | 1: the first associated recovery attempt returned |
| `FaultRecoveryResult` | 0: recovery succeeded; inspect only when Done is 1 |
| `FaultRestartsBefore` | Successful-reset counter immediately before injection |
| `Restarts` | Expected to be `FaultRestartsBefore + 1` |
| `Running` | 1 after recovery |

`Result` deliberately retains the original fault even when recovery succeeds:
normally `0xE20E0001` for a TX-only event, with `LastStage=0x0E`. Do not require
it to return to zero. `LastTxError` similarly retains the captured error.
Later unrelated faults can change these general fields; the `Fault*` record
preserves this injection's first recovery attempt.

`FaultOutcome` values:

| Value | Meaning |
|---|---|
| 0 | Not injected: check arming, asynchronous traffic and available TX resources |
| 1 | Posted, no outcome yet |
| 2 | Device queue-error event observed; inspect TX status and recovery result |
| 3 | Packet completed without a captured queue error; trigger inconclusive |
| 4 | TX watchdog timeout; reset may work, but this is not queue-error evidence |
| 5 | A different recovery cause intervened; inspect stage/result |
| 6 | Explicit test reset; evaluate using the separate criteria above |

A pass needs the captured TX queue-error evidence, successful reset, and
working debugger/packet traffic afterwards. `Restarts=1` alone is insufficient.
An RX-only fault or recovery failure is not a pass for this TX injection.

## First VM attempt: v0.3.1.0 trigger accepted without a queue error

The user reported traffic on a VM with only one NIC, but no observed fault.
The supplied `lmDvm kd_02_15ad` output matches the ARM64 fault-once artifact:

| Item | Target and local artifact |
|---|---|
| PE timestamp | `0x6ADB7C61` |
| PE checksum | `0x00007D5F` |
| Image size | `0xD000` |
| PDB GUID / age | `BFC4798C5D3D4E25B2F657644B1755AE` / 2 |

WinDbg reported private symbols but did not display version-resource strings.
The local file has the fault-once description and version 0.3.1.0; the matching
PE and PDB metadata supports the intended build being loaded. Missing strings
in this listing alone do not establish a deployment error.

The subsequent runtime reading established that the old trigger fired:

| Field | Observed value |
|---|---|
| `VmxFaultArm` | 0 (consumed) |
| `FaultCount`, `FaultOutcome` | 1, 3 (completed without a captured queue error) |
| `FaultSlot`, `FaultBuffer` | `0x17`, 0 |
| `FaultWord2`, `FaultWord3` | `0x14048`, `0x3000` |
| `FaultRestartsBefore`, `FaultEvents` | 0, 0 |
| `FaultTxError`, `FaultRxError`, `FaultTxStopped`, `FaultRxStopped` | All 0 |
| `FaultRecoveryStage`, `FaultRecoveryDone`, `FaultRecoveryResult` | All 0 |
| `Running`, `Result`, `LastStage`, `LastDetail` | 1, 0, 0, 0 |
| `Restarts`, `TxTimeouts`, `LastTxError`, `LastRxError` | All 0 |

Version 0.3.1.0 set descriptor word 2's type bit (`0x10000`). The captured
`0x14048` confirms that bit was set on a 72-byte asynchronous submission.
VMware completed it without a captured queue error; recovery never ran.
This is an inconclusive recovery test, not a failure to arm. The old DLLs,
PDBs, manifest and source/test snapshot are preserved in
`artifacts/fault-once-v0.3.1.0/`. Version 0.3.2.0 replaces that trigger with
the 17-descriptor packet described above; its real-VM result follows.

## Second VM attempt: v0.3.2.0 chain completed without a queue error

Reported on 2026-10-24, after arming the revised trigger:

| Field | Observed value |
|---|---|
| `FaultCount`, `FaultOutcome` | 1, 3 |
| `FaultSlot`, `FaultBuffer` | 2, 0 |
| `FaultWord2`, `FaultWord3` | `0x403E`, 0 |
| `FaultDescriptors`, `FaultLastSlot` | `0x11` (17), `0x12` (18) |
| `FaultRestartsBefore`, `FaultEvents` | 0, 0 |
| `FaultTxError`, `FaultRxError`, `FaultTxStopped`, `FaultRxStopped` | All 0 |
| `FaultRecoveryStage`, `FaultRecoveryDone`, `FaultRecoveryResult` | All 0 |
| `Running`, `Result`, `LastStage`, `LastDetail` | 1, 0, 0, 0 |
| `Restarts`, `TxTimeouts`, `LastTxError`, `LastRxError` | All 0 |

The chain occupied slots 2 through 18. The first fragment length was 62 bytes
(`0x3E`), followed by sixteen one-byte fragments: 78 bytes total. The final
descriptor completed without a captured queue error; no recovery ran. This
confirms the trigger executed, but does not validate reset/reinitialization
or prove that all malformed chains are accepted. The v0.3.2.0 DLLs, PDBs,
manifest and build sources are preserved in `artifacts/fault-once-v0.3.2.0/`.
The v0.3.3.0 explicit-reset result follows.

## Third VM attempt: v0.3.3.0 explicit reset succeeded

Reported on 2026-10-24:

| Field | Observed value |
|---|---|
| `FaultCount`, `FaultOutcome` | 1, 6 (explicit reset) |
| `FaultRestartsBefore`, `Restarts` | 0, 1 |
| `FaultRecoveryStage`, `FaultRecoveryDone` | `0x15`, 1 |
| `FaultRecoveryResult` | 0 (success) |
| `Running` | 1 |
| `LastStage`, `LastDetail` | `0x15`, 1 |
| `Result` | `-501940223` = `0xE2150001` (intentional test-reset diagnostic) |
| `FaultEvents`, `FaultTxError`, `FaultRxError`, `FaultTxStopped`, `FaultRxStopped` | All 0 |
| `FaultSlot`, `FaultBuffer`, `FaultWord2`, `FaultWord3`, `FaultDescriptors`, `FaultLastSlot` | All 0 (no descriptor injection) |
| `TxTimeouts`, `LastTxError`, `LastRxError` | All 0 |

The real recovery routine completed successfully and the transport was running
after one reset. The supplied WinDbg reads establish debugger communication
afterward. The user subsequently confirmed that WinDbg continued automatically
without reconnecting. This does not validate retained buffers under every workload.

The follow-up runtime sample showed:

| Field | Follow-up value |
|---|---|
| `TxPackets` | `0x15699` = 87,705 |
| `RxPackets` | `0x2142C` = 136,236 |
| `Restarts`, `Running` | 1, 1 |
| `RxDropped`, `TxTimeouts`, `LastTxError`, `LastRxError` | All 0 |
| `Fault*`, `Result`, `LastStage`, `LastDetail` | Unchanged from the first reset reading |

These are cumulative controller-session counts, not counts isolated to traffic
after the reset; the first reset reading did not include packet counters.
The follow-up confirms a running transport, no additional recorded resets or
errors, and an unchanged successful explicit-reset record. The user explicitly
confirmed that all five post-reset break/resume cycles passed, file copies
through kdnic in both directions had matching checksums, and WinDbg continued
automatically without reconnecting. File size and checksum algorithm were not
specified for these post-reset copies; the earlier 5.5G/MD5 baseline tests are
separate evidence.

**Explicit-reset checkpoint: passed on this ARM64 VM.** No further arming is
needed for this checkpoint. Restore the preserved v0.3.0.0 baseline DLL and
matching PDB, reboot using Startup Settings option 7, and verify normal
connection/break-resume before continuing baseline testing. Restoration has
not yet been reported. Actual device queue-error detection remains open.

## Reading an unclassified attempt

For future attempts, read the existing record before re-arming or changing the DLL:

```text
dd kd_02_15ad!VmxFaultArm L1
r @$t0 = poi(poi(kd_02_15ad!VmxSharedData))
dt kd_02_15ad!_VMX_CONTEXT @$t0 Fault*
dt kd_02_15ad!_VMX_CONTEXT @$t0 Running Result LastStage LastDetail Restarts TxPackets RxPackets TxTimeouts LastTxError LastRxError
```

With `FaultCount=0`, an arm value of `52535431` (explicit reset) or
`5146554c` (descriptor test) means the corresponding request remains
pending; zero means it is currently disarmed (not why). With `FaultCount=1`,
use `FaultOutcome` and the captured device/recovery fields above. A consumed
one-shot cannot be retried by writing the arm value again in that controller
lifetime. A different test requires the intended DLL, matching PDB and a fresh
controller initialization. Continue post-reset validation before changing builds.

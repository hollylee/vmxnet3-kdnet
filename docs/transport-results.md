# Packet-transport results

## ARM64 Windows 11 25H2, build 26200.9711

Reported by the user on 2026-10-23 from the VMware Fusion VM on Apple Silicon
used for the probe and init-only tests. Evidence is the pasted
`check-target.ps1` output and the user's statement: "WinDbg connected and
worked in break/resume."

| Item | Observed value |
|---|---|
| Architecture / PCI location | ARM64 / 1.0.0 |
| Variant | `vmxnet3-kdnet transport (experimental)` |
| DLL file size / image size | 22,496 bytes / `0xD000` |
| Loaded modules | `kdnet.dll` (`0x47000`) and `kd_02_15ad.dll` (`0xD000`) |
| Boot settings | DEBUG, TESTSIGNING, DISABLE_INTEGRITY_CHECKS; Secure Boot off |
| Signer / verification status | Windows component test certificate / `UnknownError` |
| Debugger host / port | `192.168.74.132` / 50000 |
| DHCP configured | Yes |
| `KdInitStatus` | **`0x00000000`** |
| `KdInitResultString` | No text displayed |
| `KdNetIPAddress` | **`192.168.74.153`** |
| `KdNetMacAddress` | **`00-0C-29-79-75-CD`**, previously confirmed to match the NIC |
| `KdNetInitializeCount` | 1 |
| `KdNetRxSuccess` / `KdNetTxSuccess` | 1 / 1 |
| Device status | vmxnet3: Error; Microsoft Kernel Debug Network Adapter: OK |
| Code Integrity events mentioning kd modules | None in the last seven days |
| Interactive debugger result | Connection and break/resume confirmed by the user |

### Module and symbol inspection

The user subsequently supplied `lmDvm kd_02_15ad` output:

| Item | Observed value |
|---|---|
| Loaded address range | `fffff800688f0000`–`fffff800688fd000` |
| Symbol status | Private PDB symbols loaded |
| PDB symbol-cache identity | `9A07D65A1EA44FE5A6DC80F7C60FB7EA2` |
| PE timestamp | `0x6ADACC29` (displayed as Oct 23 2026 10:53:29) |
| PE checksum | `0x0000DEAE` |
| Image size | `0xD000` |
| File / product version | `0.3.0.0` / `0.3.0.0` |
| File description | `vmxnet3-kdnet transport (experimental)` |

The local ARM64 transport DLL's PE timestamp, checksum and image size match.
Its CodeView PDB GUID is `9A07D65A1EA44FE5A6DC80F7C60FB7EA`, age 2, matching
the reported symbol-cache identity. This establishes agreement of build and
symbol metadata; the installed file's SHA-256 remains unreported. Private
symbols now allow runtime-context inspection using the commands in
[transport.md](transport.md).

### Runtime-context inspection

The user supplied the following private-symbol `dt` readings from the working
session, using the context reached through `VmxSharedData->Hardware`:

| Field | Observed value |
|---|---|
| `Running` | 1 |
| `Result`, `LastStage`, `LastDetail` | 0 / 0 / 0 |
| `TxPackets` | `0x5A53` = **23,123** |
| `RxPackets` | `0x92B0` = **37,552** |
| `RxDropped` | 0 |
| `TxTimeouts` | 0 |
| `Restarts` | 0 |
| `LastTxError`, `LastRxError` | 0 / 0 |

In this implementation, TX counts reclaimed descriptors and RX counts packets
delivered to KDNET. These readings demonstrate substantial traffic and many
ring-generation wraps on the real ARM64 VM, with no recorded receive drops,
TX timeouts, recovery attempts or saved queue errors. `Running=1` and the
zero diagnostic result show the transport remains active without a recorded
fault. This is a snapshot since context initialization, not a measurement of
elapsed time, throughput, or packet loss outside the module. The debugger's
own traffic contributes to the counters.

### Repeated break/resume, idle and file transfer

The user subsequently reported:

| Test | Reported outcome |
|---|---|
| 20 break/resume cycles | All passed |
| One hour idle, then break/resume | Passed |
| macOS host to target VM file transfer | 5.5G transferred through kdnic; source and destination MD5 checksums matched |
| Target VM to macOS host file transfer | Same 5.5G file transferred back; MD5 matched |
| Five break/resume cycles after the reverse transfer | All passed |

This passes the requested 20-cycle and one-hour idle checks on the ARM64
target. The checksum comparison supports integrity of the transferred file.
The user explicitly confirmed that the transfer used the Microsoft Kernel
Debug Network Adapter, then completed the requested reverse transfer and
post-copy debugger checks. This establishes successful large file transfers
in both directions with end-to-end checksum agreement and a responsive
debugger afterwards. The exact byte count and transfer durations were not
supplied, so this is an integrity and functional result, not a throughput
measurement.

The post-transfer counter reading was:

| Field | Observed value |
|---|---|
| `TxPackets` | `0x424AC0` = **4,344,512** |
| `RxPackets` | `0x7E9898` = **8,296,600** |
| `RxDropped` | 0 |
| `TxTimeouts` | 0 |
| `Restarts` | 0 |
| `LastTxError`, `LastRxError` | 0 / 0 |

These are cumulative driver counters, not counts isolated to the file copy.
They show millions of TX completions and delivered RX packets, with no
recorded receive drops, TX timeouts, successful recovery resets or saved queue
errors. `Running`, `Result`, `LastStage` and `LastDetail` were not included in
this later reading; the earlier values above should not be treated as a new
sample. Successful post-transfer break/resume provides direct liveness evidence.

### Virtual-link interruption

Following the requested disconnect/reconnect test, the user reported that
break/resume still worked after disconnecting the virtual NIC for 30 seconds.
The subsequent context reading was:

| Field | Observed value |
|---|---|
| `TxPackets` | `0x428D93` = **4,361,619** |
| `RxPackets` | `0x7EE394` = **8,315,796** |
| `RxDropped`, `TxTimeouts`, `Restarts` | 0 / 0 / 0 |
| `LastTxError`, `LastRxError` | 0 / 0 |

Compared with the previous sample, TX increased by 17,107 and RX by 19,196.
These differences include intervening traffic and are not packet counts
isolated to the interruption. The session remained usable after the link test,
without a recorded TX timeout, receive drop, successful recovery reset or
saved queue error. This passes the functional link-interruption check on this
VM; it does not exercise or validate queue-reset recovery. The report did not
include link-state readings during the outage, exact reconnect latency, the
number of post-test break/resume cycles, or the diagnostic status fields.

### Reboot series: 5 of 5 passed

The user reported reboot test 1 on the same ARM64 25H2 build: WinDbg connected
and break/resume worked. The checker again showed the experimental transport
(22,496-byte DLL, image size `0xD000`) loaded alongside `kdnet.dll`,
`KdInitStatus=0`, IP `192.168.74.153`, MAC `00-0C-29-79-75-CD`, initialize
count 1, and RX/TX success values both 1. DEBUG, TESTSIGNING and
DISABLE_INTEGRITY_CHECKS were present, Secure Boot was off, and no matching
Code Integrity events were reported. The module signer and device statuses
were unchanged from the earlier report.

The user then completed reboots 2–5, reported unchanged checker results and
working break/resume on all four, and supplied the following context readings
in reboot order. All five boots passed the initial checkpoint.

| Reboot | Connection / break-resume | TX completions | RX delivered | RX drops | TX timeouts | Restarts | Last TX / RX error |
|---|---|---|---|---|---|---|---|
| 1 | Passed / passed | `0x1225` = 4,645 | `0x188A` = 6,282 | 0 | 0 | 0 | 0 / 0 |
| 2 | Passed / passed | `0x5D38` = 23,864 | `0x5E4F` = 24,143 | 0 | 0 | 0 | 0 / 0 |
| 3 | Passed / passed | `0x5CCF` = 23,759 | `0x5DFA` = 24,058 | 0 | 0 | 0 | 0 / 0 |
| 4 | Passed / passed | `0x30EE` = 12,526 | `0x3115` = 12,565 | 0 | 0 | 0 | 0 / 0 |
| 5 | Passed / passed | `0x4EF5` = 20,213 | `0x4ECB` = 20,171 | 0 | 0 | 0 | 0 / 0 |

The context pointer was explicitly re-read in the reboot-1 transcript; the
later excerpts show only the field dumps. These are per-boot readings, not a
continuation of the preceding multi-million-packet
session. Diagnostic status fields (`Running`, `Result`, `LastStage`,
`LastDetail`) were not included in these runtime readings; initialization
success and interactive debugger operation were separately confirmed.
This completes the five-boot checkpoint; the broader 50-boot stress target
in the plan is not yet complete.

## Complete-memory-dump checkpoint (2026-10-24)

After resolving dump-output path/permission errors on the WinDbg computer,
the user reported "All done" for the requested `.dump /f` procedure and
supplied the following final runtime sample. The procedure included process
and stack inspection, completing the dump, five subsequent break/resume
cycles, and opening the dump in a separate WinDbg session. Completion is
recorded as user-reported; individual command outputs, dump size, and elapsed
time were not supplied.

| Field | Value |
|---|---|
| `Result`, `Running` | 0, 1 |
| `Restarts` | 0 |
| `TxPackets` | `0x4A1921` = 4,856,097 |
| `RxPackets` | `0xB4BEF` = 740,335 |
| `RxDropped`, `TxTimeouts`, `LastTxError`, `LastRxError` | All 0 |

This records completion of the dump checkpoint with a running transport and
no recorded resets, drops, timeouts or queue errors. These are cumulative
session counts; no pre-dump sample was provided, so they are not isolated
dump-traffic counts or a throughput measurement. The procedure requested the
v0.3.0.0 baseline; this excerpt does not independently identify the loaded
DLL or its hash. The earlier one-reset diagnostic session is separate.

## Thirty-minute traffic run: before/after counters

The user reported completion of the requested 30-minute transfer run with
periodic break/resume and supplied these snapshots:

| Field | Before | After | Change |
|---|---|---|---|
| `TxPackets` | `0x4A1921` = 4,856,097 | `0x4ABD2C` = 4,898,092 | +41,995 |
| `RxPackets` | `0xB4BEF` = 740,335 | `0xC7B65` = 818,021 | +77,686 |
| `Running` | 1 | 1 | Unchanged |
| `Result`, `Restarts` | 0, 0 | 0, 0 | Unchanged |
| `RxDropped`, `TxTimeouts`, `LastTxError`, `LastRxError` | All 0 | All 0 | No recorded errors |

The initial counts match the final dump-test sample. The supplied samples
show packet activity and a running transport without additional recorded
errors or resets. The user subsequently confirmed continuous copying,
matching checksums, and successful break/resume every five minutes. The file
was estimated at about 500 MB, explicitly uncertain, and had already been
deleted. No exact byte count, copy count, transfer log, or precise timing was
supplied.

**Thirty-minute continuous-transfer functional checkpoint: passed as reported
on this ARM64 VM.** The packet deltas do not independently corroborate the
estimated transfer volume; no throughput or link-saturation claim is made.
Retain the approximate file size as an unverified estimate. A future throughput
benchmark should record exact bytes transferred and elapsed time alongside
counter snapshots; repeating this functional run solely to recover the deleted
file's size is unnecessary.

## Boot without a WinDbg listener and attach later

The user confirmed that Windows booted with WinDbg not listening and that
attaching afterward worked on the ARM64 VM. The supplied runtime sample was:

| Field | Value |
|---|---|
| `Result`, `Running` | 0, 1 |
| `Restarts` | 0 |
| `TxPackets` | `0x747A` = 29,818 |
| `RxPackets` | `0x76E4` = 30,436 |
| `RxDropped`, `TxTimeouts`, `LastTxError`, `LastRxError` | All 0 |

This passes the boot-without-listener and later-attachment checkpoint on this
VM, with no recorded runtime errors or resets. The WinDbg field dump also
establishes debugger communication after attachment. The report did not
separately state the requested five-cycle break/resume result or attachment
latency. These counts belong to the new boot and are not a continuation of
the earlier dump/continuous-copy session.

## What this establishes

The transport loaded and returned initialization success on this target.
KDNET published a usable IPv4 address and the expected MAC. The successful
WinDbg session is direct evidence of bidirectional packet transport and
working callbacks during debugger break/resume, beyond device initialization.

The on-disk size and variant agree with the supplied ARM64 v0.3.0.0 artifact.
The report contains no file hash, so it does not prove a byte-for-byte match
to `out/transport-manifest.json`. Preserve the current DLL/PDB baseline and
record the installed hash on the next run.

`KdNetRxSuccess` and `KdNetTxSuccess` were both 1. Their exact update semantics
have not been established; these values do not imply that only one packet
was sent/received or provide a throughput measurement. Use the reported
interactive session as the connection evidence.

The VMware virtual vmxnet3 adapter's Error status did not prevent this working
session. It is consistent with KDNET owning the NIC, but no PnP problem code
was supplied to establish its exact cause. The kdnic device's OK status alone
does not establish that ordinary Windows networking through it works.

## Remaining validation

- Module and runtime-counter inspection are recorded. The user reported
  completion of the dump procedure, which included `!process 0 0` and `k`.
  The 20-cycle, one-hour idle, and complete-memory-dump checkpoints passed;
  broader stress coverage remains open. Dump size and duration are unreported.
- The phase 5.2 large-copy/debugger-responsiveness criterion is passed on this
  ARM64 VM. The reported 30-minute continuous-copy run also passed checksum
  and periodic break/resume checks, with clean before/after counters.
  Quantified throughput, saturation testing and coverage on other targets
  remain open.
- Broader coverage toward the plan's 50-boot target; the initial five passed.
- VMware-generated queue-error recovery and sustained heavy-load stress runs.
  The subsequent v0.3.3.0 explicit-reset test succeeded on ARM64 with
  `FaultOutcome=6`, recovery done/result 1/0, `Restarts=1`, `Running=1`, and
  automatic continuation of WinDbg without reconnecting. All five post-reset
  break/resume cycles and checksum-verified transfers through kdnic in both
  directions passed. Follow-up counters showed no additional resets, drops,
  timeouts or queue errors. See [fault-test.md](fault-test.md)
  for this separate diagnostic build's full record and limits.
- VMware-log inspection and installed DLL hash. The report also does not
  explicitly confirm that the registry key was cleared before this boot.
- Identification of which accepted packet-callback pointer form KDNET uses,
  and direct validation of link-field units. The session establishes that
  the current implementation works, not those exact ABI details.
- x64 target builds and other VMware hosts/revisions.
- Sleep/hibernate registration and resume support; production signing.

The first ARM64 connection milestone is passed. The project remains
experimental; the next procedure is in [transport.md](transport.md).

# Device bring-up results

## ARM64 Windows 11 25H2, build 26200.9711

Reported by the user on 2026-10-23 from the VMware Fusion VM on Apple
Silicon used for probe v2. Evidence: pasted output of `check-target.ps1`
after booting the normal init-only module.

| Item | Observed value |
|---|---|
| Architecture / PCI location | ARM64 / 1.0.0 |
| Variant | `vmxnet3-kdnet init-only (stop after 0)` |
| DLL file size / image size | 13,792 bytes / `0xB000` |
| Loaded modules | `kdnet.dll` and `kd_02_15ad.dll` |
| Boot settings | DEBUG, TESTSIGNING, DISABLE_INTEGRITY_CHECKS; Secure Boot off |
| Signer | `CN=vmxnet3-kdnet test (Windows component)` |
| `KdInitStatus` | **`0xE2802710`** (3800049424) |
| `KdInitResultString` | `NIC hardware initialization failed.` |
| `KdNetMacAddress` | **`00-0C-29-79-75-CD`** |
| `KdNetIPAddress` | `0.0.0.0` |
| `KdNetInitializeCount` | 1 |
| `KdNetRxSuccess` / `KdNetTxSuccess` | 0 / 0 |
| Device status | vmxnet3: Error; Microsoft Kernel Debug Network Adapter: OK |

### What this establishes

The returned status has completion stage `0x80` and detail `0x2710`, or
10,000 Mbps. In the current code, reaching it means:

- The exact 33/15 import/export tables and required import pointers passed
  validation, as did shared-data identity and mapped BAR checks.
- PCI command writes/readback worked, including enabling bus mastering.
- Revision 1 and UPT v1 were selected; quiesce/reset and interrupt-mask
  writes returned.
- All DMA regions and packet buffers passed bounds, physical alignment and
  continuity checks. This does not require the whole allocation to be
  physically contiguous.
- The stall returned and the cycle counter advanced with a usable frequency.
  Precise timing accuracy has not been measured.
- **`ACTIVATE_DEV` returned zero**, RX descriptors were posted, and the
  device reported **link up at 10 Gbps**.
- Neither queue reported stopped/error, and no queue-error event was set at
  the time of the final check.
- Cleanup issued quiesce/reset, cleared the shared address registers, and
  successfully wrote/read back PCI bus mastering as disabled. Cleanup failure
  would have replaced the completion status with stage `0x0F`.

KDNET recorded `00-0C-29-79-75-CD` rather than the earlier placeholder
`F2-F5-D5-01-7C-6C`. Since init-only reads MACL/MACH and copies those bytes
through `TargetMacAddress`, this supports the MAC publication path. On
2026-10-23 the user independently confirmed that this address matches the NIC.

### Expected failure presentation

The init-only module intentionally returns an NTSTATUS failure even when
bring-up passes. The generic initialization-failed text is consistent with
that behavior and does not contradict the encoded completion status. This
module does not call the error-string import, so this text supplies no new
evidence about that slot.

No packet callbacks are operational. Zero RX/TX counters and `0.0.0.0` are
therefore expected. The physical adapter's Error status is consistent with
KDNET taking the device and this build then stopping it; the output does not
include a PnP problem code that would establish the exact cause. The kdnic
device's OK status alone does not establish working networking.

### Remaining checks and next work

Phase 4 now has an experimental transport and simulated-device coverage for
RX/TX and recovery. The subsequent ARM64 WinDbg connection and break/resume
passed; see [transport-results.md](transport-results.md) and
[transport.md](transport.md). The init-only variant remains available for
regression diagnosis.

The KDNET interpretation of `LinkSpeed`, `LinkDuplex` and `LinkState` remains
unverified: init-only completion only proves the writes were reached. The
subsequent transport run demonstrates basic packet DMA, but does not establish
stress coverage for ring wraparound, asynchronous sends, recovery or memory
ordering under sustained traffic. The x64 probe and bring-up runs remain open.

The pasted report does not include a DLL hash, confirmation that the registry
key was cleared, or `vmware.log`. Record those on
the next target run; do not treat this result as satisfying all-host coverage
or the plan's VMware-log check.

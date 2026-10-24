# Initialization-only vmxnet3 bring-up

The `init-only` module implements device initialization without a packet path.
It negotiates vmxnet3 revision 1 and UPT version 1, resets the NIC, prepares one
TX and one RX queue, activates the device, posts RX buffers, checks link and
queue status, then quiesces/resets the device and disables bus mastering.
It always returns a failure status, including when every check passes, so
Windows can continue booting without waiting for a debugger connection.

## Validation status

As of 2026-10-23:

- ARM64 and x64 builds pass MSVC `/W4 /WX`, structure-layout assertions, and
  the existing no-import, single-export and relocation checks.
- The simulated-device tests run on both architectures on the ARM64
  development machine (x64 under Windows emulation). They cover successful
  initialization, repeated initialization, discontiguous physical pages above
  4 GB, allocation bounds, RX descriptor types, activation failure, bad
  mappings, unsupported revisions, invalid PCI/device data, link timeouts,
  failed clocks, queue errors, cleanup failures, and all five checkpoints.
- The status decoder is tested with signed and unsigned DWORD values, the
  new init-only codes, and old probe codes.
- The init-only and reset-checkpoint DLLs for both architectures were
  test-signed with the project's Windows certificate profile. This does not
  remove the ARM64 Startup Settings option 7 requirement. Standard signature
  verification reports no chain to a trusted root for this test profile;
  this is not a production-trusted signature. The current artifact hashes
  and version labels are recorded in `out/bring-up-manifest.json`.
- **ARM64 real-VM bring-up passed** on Windows 11 25H2 build 26200.9711:
  `0xE2802710` reports successful activation, 10-Gbps link, queue checks and
  cleanup. KDNET recorded MAC `00-0C-29-79-75-CD`. See
  [bring-up-results.md](bring-up-results.md) for the evidence and its limits.
  x64 target runs remain outstanding.

The ARM64 run exercised register-write, PCI-write, stall and cycle-counter
imports and supports publication through `TargetMacAddress`. The build
provisionally uses Mbps for `LinkSpeed`, 2 for full duplex, and 1 for connected
`LinkState`. These link-field conventions remain unverified: completion
proves the writes were reached, but not how KDNET interpreted them. Compare
the link fields during the transport run. The user confirmed the registry MAC
matches the NIC on 2026-10-23. Link-field
interpretation may need a debugger or a working packet transport to verify.

## Build and run

From a command prompt in the project directory:

```bat
build.cmd all test
powershell -NoProfile -ExecutionPolicy Bypass -File tests\status_test.ps1
sign.cmd windows
```

The build creates:

- `out\arm64\init-only\kd_02_15ad.dll` and `.pdb`
- `out\x64\init-only\kd_02_15ad.dll` and `.pdb`

The `test` argument runs simulated-device tests after building each target.
On an x64 development machine, ARM64 test binaries are compiled but cannot
be executed. Test executables are user-mode programs; they do not access
the real NIC. Building replaces signatures, so always sign after the final
build. Build and tests alone do not sign or install anything.

On the ARM64 VM already used for probe v2:

1. Take a VM snapshot. This build writes device registers and enables DMA.
2. Copy the **ARM64 init-only** DLL into `C:\Windows\System32`, using an
   elevated prompt. Keep the existing vmxnet3 `busparams`, test-signing and
   Secure Boot settings from the successful probe run.
3. Run the updated `tools\check-target.ps1`; confirm `Variant` says
   `vmxnet3-kdnet init-only (stop after 0)`.
4. Clear the old KDNET registry key and reboot through Startup Settings
   option 7, exactly as in the README's probe instructions.
5. After boot, run the updated `tools\check-target.ps1` again. Save its
   output, the exact Windows build, the DLL hash, and the relevant
   `vmware.log` entries. The checker omits the debug key.

Do not install on x64 Windows 11 24H2 or later, where Windows owns the inbox
file. Run the original probe first on older x64 targets; matching table
counts alone do not establish that their interface semantics match ARM64.

The expected completion is **`0xE280ssss`**, where `ssss` is the observed
link speed in Mbps. A 10-Gbps virtual link gives **`0xE2802710`**. Check that
`KdNetMacAddress` equals the VM's actual adapter MAC rather than KDNET's
placeholder. The NIC has been stopped by the time this status is returned;
this build does not provide network debugging or normal networking over kdnic.

The decoder can also read a saved value without elevation or querying the
local machine:

```powershell
.\tools\check-target.ps1 -DecodeStatus 0xE2802710
```

If initialization fails, the status identifies the stage. No error-string or
DbgPrintf import is used by this module. If it crashes or hangs inside an
unverified import, it cannot return a status; use a checkpoint build or a
serial boot debugger on x64. Revert the snapshot after a boot failure.

## Checkpoint builds

An optional environment variable builds an additional DLL which stops after
one stage, performs cleanup, and returns a checkpoint status. The normal
`init-only` output is still built separately.

```bat
set VMX_INIT_STOP_AFTER=8
build.cmd arm64
set VMX_INIT_STOP_AFTER=
sign.cmd windows
```

Install `out\arm64\init-stop-8\kd_02_15ad.dll` for this example. Its version
resource identifies the selected checkpoint. A checkpoint reports that the
calls returned and checks passed; the reset command itself has no checked
completion code.

| Stop after | Checkpoint | Expected status |
|---|---|---|
| 6 | PCI memory decode enabled, bus mastering disabled | `0xE2460000` |
| 8 | Revision selected, reset issued, interrupt masks written | `0xE2480000` |
| 9 | Physical layout verified and shared structures prepared | `0xE2490000` |
| 10 | Stall returned and cycle counter advanced | `0xE24A0000` |
| 12 | `ACTIVATE_DEV` returned zero | `0xE24C0000` |

Cleanup itself uses MMIO writes after checkpoint 6, so that checkpoint
cannot isolate PCI writes from all register-write calls. These builds are
intended to narrow the failing interval, not to catch CPU faults.

## Status format

Init-only codes use `0xE2SSDDDD`: `SS` is the stage and `DDDD` is the low
16 bits of its detail. Stage `0x80` means all bring-up checks passed;
checkpoint stages are `0x40 | stage`. Every code is an intentional NTSTATUS
failure. These encodings are distinct from probe v1/v2 codes.

| Stage (hex) | Meaning | Detail |
|---|---|---|
| `01` | Table rejection | Import count in high byte, export count in low byte; unavailable counts are zero |
| `02` | Missing required import | Zero-based import slot, with `Exports` at slot 0 |
| `03` | Adapter/device | 1: shared-data pointers mismatch; 2: nonzero PCI segment or wrong device IDs |
| `04` | Memory | 1: block too small, misaligned context, or address overflow |
| `05` | BAR mapping | 0/1: invalid BAR index; `00FF`: BARs-mapped flag absent |
| `06` | PCI | 1: ID read; 2: command read; 3: initial command write/readback; 4: bus-master enable/readback |
| `07` | Revision | 1: revision 1 unavailable/invalid; 2: UPT v1 unavailable/invalid |
| `09` | DMA layout | 1–8: region below; `0100`–`015F`: buffer index + `0100`; `FFFF`: bounds |
| `0A` | Clock | 1: invalid frequency; 2: no advance or implausible delta after stall |
| `0B` | MAC | 1: zero or multicast MAC |
| `0C` | Activation | Low 16 bits of the nonzero command result |
| `0D` | Link | 1: timeout; 2: all-ones register response; 3: zero speed while up |
| `0E` | Queues | 1: TX stopped/error; 2: RX stopped/error; 3: queue-error event |
| `0F` | Cleanup failed | Original result stage; bus mastering could not be disabled/verified |
| `10` | Output pointers | 1: null MAC or link-state pointer |

`VMX_CONTEXT` keeps the original stage and full 32-bit detail for inspection
with this module's PDB. A cleanup failure overrides the externally returned
status so it cannot appear to be a successful diagnostic run. If that
happens, inspect/revert the VM before proceeding.

## Memory and device constraints

The module requests **232 KiB**, including its context and alignment slack.
Eight separately aligned 4-KiB slots hold the following DMA regions:

| Region / detail | Object | Bytes used |
|---|---|---|
| 1 | Revision-1 DriverShared | 720 |
| 2 | TX + RX queue descriptors | 512 |
| 3 | TX ring, 32 entries | 512 |
| 4 | TX data ring, 32 × 128 bytes | 4096 |
| 5 | TX completion ring | 512 |
| 6 | RX head ring | 512 |
| 7 | RX body ring | 512 |
| 8 | RX completion ring, 64 entries | 1024 |

There are 96 individually checked 2-KiB buffers: 32 TX, 32 RX head and
32 RX body. Each region and buffer stays within a 4-KiB page. Physical
addresses are obtained individually; different regions need not be
contiguous. The final byte and every crossed page are checked, along with
the required physical alignment. RX posts 31 entries per ring initially,
leaving one unused so the producer index is unambiguous. TX buffers/rings
are reserved but no transmit descriptors are posted.

Only PCI segment 0, pre-mapped memory BARs, revision 1 and UPT v1 are
accepted. There is no BAR-mapping fallback yet. DMA coherence is assumed
for the VMware guest platform; ARM64 uses full-system memory barriers and
x64 uses a compiler barrier plus a memory fence before ownership and MMIO
publication. Cache maintenance for noncoherent hardware is not implemented.
Link polling has both a three-second cycle deadline and a 3000-iteration
limit, with 1-ms stalls. The iteration limit prevents an infinite loop if
the cycle counter stops after its initial check; it cannot recover from
an import call that itself hangs.

The wire definitions are reduced from BSD-licensed DPDK v24.11, with notices
and untouched reference headers in `third_party/dpdk/`. No offloads,
interrupt delivery, multiple queues, packet transport or power management
are implemented in this diagnostic build.

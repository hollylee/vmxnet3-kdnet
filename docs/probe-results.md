# Probe results

Findings from running the probe and its variants on target VMs (plan step
1.7). Probe runs are listed chronologically within each target. Subsequent
device-initialization results are in [bring-up-results.md](bring-up-results.md).

## Windows 11 ARM64 25H2 (26200.9711), VMware Fusion on Apple Silicon

VM: vmxnet3 at PCI 1.0.0. No e1000e adapter is offered, and the firmware has
no ACPI DBG2 table (tables: MCFG FACP APIC PPTT GTDT TCPA SSDT TPM2), so
neither a second debug NIC nor serial debugging is available.

| Date | Module on target | Signature | Result |
|---|---|---|---|
| 2026-10-20 | probe (normal), first build: no relocation table, no load config | Basic (self-signed leaf, Code Signing only) | Only `kd.dll` loaded (local-only fallback); no `kdnet` key |
| 2026-10-20 | probe (normal), rebuilt to match the inbox PE layout | Basic | Same |
| 2026-10-20 | probe-hang-entry | Basic | Booted normally: `KdInitializeLibrary` was never called |
| 2026-10-20 | Windows' `kd_02_1af4.dll` (virtio) copied as `kd_02_15ad.dll` | Microsoft Windows | `kdnet.dll` and `kd_02_15ad.dll` loaded; KDNET took the vmxnet3 device; `kdnet` key created with `KdInitStatus` 0xC0000001 and `KdInitResultString` "Not a VirtualIO network controller." |
| 2026-10-21 | probe-hang-entry | Windows profile (test root issuing a leaf with Windows System Component Verification + Code Signing) | Booted normally: still refused |
| 2026-10-22 | probe-hang-entry, booted with "Disable driver signature enforcement" (Startup Settings, option 7) | Windows profile | **Froze**: winload loaded the module and called `KdInitializeLibrary` |
| 2026-10-22 | probe v1 (normal), option 7 | Windows profile | `kdnet.dll` + `kd_02_15ad.dll` loaded; `KdInitStatus` = 0xE15A0001 (the probe's own code), so `KdInitializeLibrary`'s table checks passed and KDNET called `KdInitializeController`. `KdInitResultString` was stale (left from the virtio test): our text pointer never reached KDNET |
| 2026-10-22 | probe v2 (normal), option 7, `kdnet` key cleared first | Windows profile | `KdInitStatus` = **0xE6FF0F21**, decoded below. No `KdInitResultString` written |

### Probe v2 summary, 0xE6FF0F21

| Field | Value | Meaning |
|---|---|---|
| Set via | return value (bit 28 = 0) | KDNET recorded `KdInitializeController`'s return value, not our write through `KdNetErrorStatus` |
| Import / export counts | 33 / 15 | Exactly what the ReactOS header defines |
| devmatch / hwmatch | 1 / 1 | The `KdInitializeController` argument is `KDNET_SHARED_DATA`; its `Device` is the descriptor from `KdInitializeLibrary` and its `Hardware` is our memory block |
| Memory block contiguous | 1 | The first 64 KB is physically contiguous |
| PCI config read = vmxnet3 | 1 | `GetPciDataByOffset` (slot 2) with the descriptor's `Bus`/`Slot` works |
| VRRS read OK | 1 | BAR1's `TranslatedAddress` is mapped and `ReadRegisterULong` (slot 7) works |
| String / status / DbgPrintf slots | present, non-null | Within the count and non-null; whether they are the right slots is unverified |
| `KdInitializeLibrary` calls | 2 | Both calls (winload and kernel) reached the same image instance, so module globals survive from winload into the kernel |

### Layout status after v2

Confirmed on this build: the import and export counts, the export table
order (KDNET calls our functions correctly), `KDNET_SHARED_DATA.Hardware`
and `.Device`, `DEBUG_DEVICE_DESCRIPTOR` `Bus`, `Slot`, `BaseAddress[]` and
`Memory`, and the `GetPciDataByOffset`, `GetPhysicalAddress` and
`ReadRegisterULong` imports.

With the key cleared before boot, KDNET still wrote `KdNetMacAddress` =
F2-F5-D5-01-7C-6C, `KdNetIPAddress` = 0.0.0.0 and `KdNetInitializeCount` =
1, although the probe never wrote `TargetMacAddress`. **Answered:** this is
not the vmxnet3 adapter's MAC. It's KDNET's own placeholder: the same value
appeared in the virtio test, where that module also failed before reporting
a MAC, and its first byte (F2) marks it as locally administered. So KDNET
does not read the MAC itself; the driver must copy the adapter's MAC (from
MACL/MACH) to `*TargetMacAddress` during initialization, as planned.

Not yet confirmed by probe v2: the write-register, stall and cycle-counter imports
(exercised by device bring-up), `TargetMacAddress`, `LinkSpeed` and
`LinkState` (exercised once initialization succeeds), and the
`KdNetErrorString`/`KdNetErrorStatus` slots. KDNET did not pick up our text
pointer, so either those slots sit elsewhere, or the string must be supplied
differently (for example copied into a KDNET-owned buffer). Until that is
understood, failures are reported through the returned status.

### Conclusions so far

- winload builds the module name from the device and loads it from
  `System32`, as documented. With a Microsoft signature, a module under the
  name `kd_02_15ad.dll` loads on this VM.
- winload refuses a module signed with the Basic test certificate, even with
  test signing on and Secure Boot off. Those boots never reached our code;
  the subsequent option-7 probe runs validated the interface as described above.
- The registry channel works: KDNET writes `KdInitStatus`,
  `KdInitResultString` (the module's `KdNetErrorString` text),
  `KdNetMacAddress`, `KdNetInitializeCount`, `KdNetRxSuccess`,
  `KdNetTxSuccess` and `KdNetIPAddress` under
  `HKLM\SYSTEM\CurrentControlSet\Services\kdnet`.
- The module appears in the kernel's module list under its own name.
- Every inbox KDNET module is signed by a "Microsoft Windows" leaf with the
  Windows System Component Verification usage (1.3.6.1.4.1.311.10.3.6) plus
  Code Signing, issued by Microsoft Windows Production PCA 2011.

- Adding the Windows System Component Verification usage to a test
  certificate chain does not make winload accept the module.

- With driver signature enforcement disabled for one boot, winload loads
  and calls our module. **Signature policy is the only blocker**: winload's
  rule for KDNET modules rejects every test certificate tried, even with
  test signing on. Development is possible by booting with option 7 each
  time; distribution needs a signature winload accepts (ask Microsoft).

### Next

The plan's step 1.7 gate is passed for ARM64 25H2: the header is usable as
is for the tested paths. The subsequent ARM64 init-only run passed activation,
link and queue checks; see [bring-up-results.md](bring-up-results.md). Packet
handling (phase 4) is next. The same probe still needs to run on the x64 targets
(Windows 10 and Windows 11 before 24H2), where the counts may differ.

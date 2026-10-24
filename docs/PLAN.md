# vmxnet3-kdnet: development plan

This plan covers everything from checking the KDNET interface to shipping a
working `kd_02_15ad.dll`. Each step says **what** to do, **why** it is needed,
and **done when** (how to know the step is finished). Steps marked *gate* are
decision points: later steps depend on their results and may change because of
them.

**Implementation update (2026-10-23):** the ARM64 probe gate has passed on
build 26200.9711. The phase 2 definitions/wrappers and phase 3 initialization-only
module now build and pass simulated-device tests on ARM64 and x64. ARM64
device bring-up returned `0xE2802710`: activation, 10-Gbps link, queue checks
and cleanup passed; the user confirmed KDNET's MAC matches the NIC. Phase 4
now has an experimental packet transport with RX/TX ownership, completion
polling, bounded sends, queue recovery and tests passing on both architectures.
The first real ARM64 WinDbg connection and break/resume are now confirmed:
`KdInitStatus=0`, target IP `192.168.74.153`, RX/TX success values both 1.
The pre-reboot session's last context inspection shows 4,361,619 TX completions
and 8,315,796 RX packets, with zero recorded drops, timeouts, restarts or queue
errors. Earlier inspection confirmed `Running=1` and zero diagnostic status.
Ring wraparound now has real-VM evidence.
The user also passed 20 break/resume cycles and one hour idle followed by
break/resume. A 5.5G file copy through kdnic in both directions between macOS
and the VM had matching MD5 checksums, followed by five successful break/resume
cycles. The phase 5.2 copy/responsiveness criterion is passed on this ARM64 VM.
A 30-second virtual-NIC interruption was followed by working break/resume
with no recorded restart or TX timeout. Broader timed load and queue-reset
recovery tests remain outstanding.
See [transport-results.md](transport-results.md), [transport.md](transport.md)
and [bring-up-results.md](bring-up-results.md). This passes the basic ARM64
connection and bulk-transfer milestones, not all phase 4/5 completion criteria.
All five reboots in the initial checkpoint passed, with unchanged successful
checker results, working break/resume and zero drops, timeouts, restarts or
saved queue errors in every supplied context reading. The per-boot counts
are recorded in transport-results.md. A separate v0.3.3.0 `fault-once` build
now implements explicit one-shot arming, device-error capture and recovery
diagnostics, with tests on both architectures. The signed v0.3.0.0 baseline
is preserved. Both the v0.3.1.0 descriptor-type trigger and v0.3.2.0
17-descriptor trigger fired but completed without a captured queue error
(`FaultOutcome=3`); no recovery ran. The 2026-10-24 update adds an explicitly
requested reset with a distinct outcome and test stage. This exercises the
existing recovery routine without device-error evidence. Its first ARM64 run
succeeded: outcome 6, recovery done/result 1/0, Restarts 1 and Running 1, with
WinDbg continuing automatically without reconnecting. All five post-reset
break/resume cycles and checksum-verified transfers through kdnic in both
directions passed; the follow-up recorded no additional resets, drops, timeouts
or queue errors. This passes the explicit-reset checkpoint in
[fault-test.md](fault-test.md). Actual device queue-error
detection/recovery (4.3) is still unverified. The 50-boot stress target, x64, power management
and VMware-log checks remain open.
The x64 probe runs remain outstanding. The implementation accepts exact
33/15 table counts, revision 1, pre-mapped BARs and PCI segment 0. It uses
returned status codes instead of the unverified text slots, individually
checks page-contained DMA regions, posts head buffers on RX ring 0 and body
buffers on ring 1. The init-only variant stops DMA before its diagnostic
failure; the transport returns success with the queues running.
The MAC output path now has target evidence; link-field interpretation
still needs validation. The detailed
steps below describe the eventual transport, including work beyond this
initial implementation. Packet handles now include a generation token as well
as the buffer index, keeping the top three bits for KDNET flags. Recovery
preserves buffers held by KDNET and their tokens. `RestartKdnet` remains
untouched because its meaning is unverified. Hibernate support remains phase 5 work.

---

## 0. Goal and ground rules

**Goal.** A KDNET extensibility module that lets WinDbg debug a Windows kernel
over VMware's vmxnet3 virtual NIC (PCI `15AD:07B0`) on systems where Windows
has no inbox module:

| Target | Why it needs this module |
|---|---|
| Windows 11 ARM64 (current builds) | No inbox `kd_02_15ad.dll` at all |
| Windows 10 x64, Windows 11 x64 before 24H2 | Inbox module arrived only in 24H2 |
| Windows 11 x64 24H2 and later | Not targeted; Windows already ships the module |

**Ground rules.**

- **No GPL code.** The Linux driver may be read to understand device
  behavior, but nothing is copied from it.
- **No code derived from Microsoft or VMware binaries.** No disassembly of
  `vmxnet3.sys` or Microsoft's `kd_02_15ad.dll`. Watching data that crosses
  the interface at runtime (tables, pointers) is fine. Reading their code is
  not.
- **Allowed sources:**
  - Microsoft's KDNET extensibility documentation
  - the WDK headers (`DEBUG_DEVICE_DESCRIPTOR` in `ntddk.h`)
  - the MIT-licensed ReactOS KDNET headers
  - BSD-licensed vmxnet3 definitions (DPDK `drivers/net/vmxnet3/base/`,
    FreeBSD `if_vmx.c`)

**Why these rules:** they support the project's BSD-3-Clause licensing while preserving
the required third-party terms and copyright notices.

---

## Phase 1: Verify the KDNET interface (probe module)

The ReactOS headers aren't Microsoft's, and nothing that ships uses them. A
wrong field offset in the import or export table would crash the target at
boot, before any debugger can help. So the interface is checked first, with
a module that only observes.

### 1.1 Prepare the test VMs

**What.** Set up these VMware VMs, each with a vmxnet3 adapter, plus a second
NIC (e1000e) or console access:

| VM | Host product | Priority |
|---|---|---|
| Windows 11 ARM64 (25H2 or 24H2) | VMware Fusion on Apple Silicon | Required |
| Windows 11 x64 23H2 | VMware Workstation or ESXi | Required |
| Windows 10 x64 22H2 | VMware Workstation or ESXi | Required |
| Windows 11 x64 24H2+ | Any | Optional, for step 1.6 |

For each VM:
- Turn off Secure Boot in the firmware settings.
- Add a serial port backed by a named pipe (Workstation/ESXi) or a file or
  socket (Fusion).
- Take a snapshot before any change.

**Why.**
- These are the builds the module has to support, and the import/export
  counts may differ between them.
- The second NIC keeps the VM reachable while the vmxnet3 adapter belongs to
  the debugger.
- The serial port is the fallback debug channel for step 1.5.
- Snapshots let you undo a boot failure in seconds.

**Done when:** every VM boots, shows a vmxnet3 adapter in Device Manager, and
has a snapshot.

### 1.2 Build and test-sign the probe

**What.** Run `build.cmd`, then `sign.cmd`. This produces
`out\arm64\probe\kd_02_15ad.dll` and `out\x64\probe\kd_02_15ad.dll`.

**Why.**
- `build.cmd` checks the two structural rules KDNET enforces: no imports,
  and exactly one export (`KdInitializeLibrary`).
- Test signing is the only way an unofficially signed module can load.

**Done when:** both DLLs are signed. `signtool verify /pa` reports a signature
(an untrusted root is expected).

### 1.3 Deploy the probe

**What.** Follow the README's "Run the probe" steps on each VM:
1. Turn on test signing.
2. Copy the DLL for that architecture to `System32`.
3. Find the adapter's bus.device.function.
4. `bcdedit /debug on`, then `/dbgsettings net ... busparams:b.d.f`.
5. Reboot.

**Why.**
- winload builds the module name from the selected device's PCI class and
  vendor ID, `kd_02_15ad.dll`. `busparams` makes sure the vmxnet3 adapter is
  the selected device.
- The probe fails initialization on purpose, so the VM should boot normally.

**Done when:** each VM boots to the desktop with debugging configured.

### 1.4 Collect results

**What.** On each VM, run
`reg query HKLM\SYSTEM\CurrentControlSet\Services\kdnet /s` and save the
output, along with the exact Windows build (`winver`).

**Why.** The probe reports through `KdNetErrorString` and `KdNetErrorStatus`,
which KDNET is expected to save under that key. This is the cheapest way to
get the data, with no debugger needed.

**Done when:** each VM has a saved output with a `kd_02_15ad probe v1 ...`
line, or step 1.5 has been done for it.

### 1.5 Fallback: serial boot debugger (only if 1.4 shows nothing)

**What.** Follow Microsoft's "Debugging KDNET Extensibility Modules" steps
over the serial pipe:
1. Set `bootdebug`, and turn off boot-failure recovery
   (`BootStatusPolicy IgnoreAllFailures`, `RecoveryEnabled No`).
2. Break in at `winload!BlBdStop`.
3. Set breakpoints on `kdstub!KdInitializeLibrary` (or
   `kd_02_15ad!KdInitializeLibrary`).
4. Read the `KdNetDbgPrintf` output, or inspect the tables directly with `dt`
   using our header types.

**Why.** No registry output means one of three things, and this tells which:
- the module wasn't loaded (signing, Secure Boot, file name or location)
- it was loaded but rejected
- KDNET doesn't save the error string where expected

**Done when:** the probe's report has been seen, or the reason it never ran
is known.

### 1.5a Freeze tests (VMs with no boot debugger)

**What.** Run the `probe-hang-entry` and `probe-hang-checked` variants (see
the README) and note whether each one freezes the boot.

**Why.** VMware Fusion on Apple Silicon offers no e1000e NIC, and its
firmware has no ACPI DBG2 table, so serial debugging isn't available either.
These variants tell, without any debugger, whether the boot loader refused
the module, called it and failed our checks, or got past them.

**Done when:** the README's result table gives a single meaning for the VM.

### 1.6 Optional: compare with the inbox module on x64 24H2

**What.** On an x64 24H2 VM, with Microsoft's own module in place, use the
boot debugger to break at `kdstub!KdInitializeLibrary` and dump the import
table, the export table and the device descriptor that Windows passes.
Compare them with the ReactOS layout.

**Why.** This is independent evidence of the layout from a module known to
work. It looks only at data Windows passes in, not at Microsoft's code.

**Done when:** the dumped tables either match the ReactOS layout or the
differences are written down.

### 1.7 *Gate:* interpret the results

Record each finding in `docs/probe-results.md` (one section per build). Then
decide:

| Finding | If as expected | If not |
|---|---|---|
| `imp`/`exp` = 33/15 on every build | Use the header as is; check counts exactly | Record counts per build. Accept a range, and use only the slots present (step 2.1) |
| `devmatch=1`, `hwmatch=1` | `Adapter` is `KDNET_SHARED_DATA`, and `Hardware` is our memory block | Find out what `Adapter` is (1.5/1.6) before going further |
| `contig=1` | Rings can sit anywhere in the block | Each ring must fit inside one page, or the block must be requested differently (step 3.2) |
| `cached` | Note the value; decides whether explicit cache handling is needed | Same |
| `bar0`/`bar1` valid, `ddflags` has the BARs-mapped flag | Use `TranslatedAddress` directly | Map the BARs with `KdMapPhysicalMemory64` |
| `cfgid=07b015ad`, `cmd` has memory decode on | PCI access works; KDNET enabled the device | Set memory decode and bus mastering ourselves during init |
| `vrrs`, `uvrs` non-zero, `mac` matches the VMX file | Register reads work | Re-check the BAR index and offsets against the BSD definitions |
| `libcalls` | Shows whether winload and kernel phases share globals | If 1, globals don't survive from winload to the kernel. Keep all state in the hardware context |

**Done when:** every row has an answer for every target build, and any
change to the plan is written into this file.

---

## Phase 2: Project foundations

### 2.1 Finalize the KDNET interface layer

**What.** In `src/common/kdnet_rt.*`:
- Turn the probe's count handling into the production rule chosen in 1.7.
  Either the counts match exactly, or there's a per-build list of accepted
  counts, and only slots inside the OS-provided count are used.
- Add wrapper functions for the imports the driver uses (register reads and
  writes, physical address lookup, PCI config, stall, cycle counter), so the
  driver code never calls through the table directly.

**Why.**
- Microsoft's sample rejects any count mismatch. A module that writes
  export slots the OS didn't provide corrupts KDNET's memory.
- The wrappers do the job of the call remapping that Microsoft's header
  provides and ReactOS's lacks.

**Done when:** the probe still builds with the new layer and gives the same
results.

### 2.2 vmxnet3 interface definitions

**What.** Write `src/vmxnet3/vmxnet3_hw.h` from the BSD-licensed DPDK
`base/vmxnet3_defs.h` (and `upt1_defs.h`), keeping only what the module
uses:

- **Register offsets.**
  - BAR0: IMR, TXPROD, RXPROD, RXPROD2.
  - BAR1: VRRS, UVRS, DSAL, DSAH, CMD, MACL, MACH, ICR, ECR.
- **Command codes:** `ACTIVATE_DEV`, `QUIESCE_DEV`, `RESET_DEV`,
  `UPDATE_RX_MODE`, `UPDATE_MAC_FILTERS`, `UPDATE_VLAN_FILTERS`, `GET_LINK`,
  `GET_QUEUE_STATUS`.
- **Shared structures:** `Vmxnet3_DriverShared`, the misc/queue/interrupt/
  filter configuration sub-structures, `Vmxnet3_TxQueueDesc`,
  `Vmxnet3_RxQueueDesc`.
- **Descriptors:** `Vmxnet3_TxDesc`, `Vmxnet3_TxCompDesc`, `Vmxnet3_RxDesc`,
  `Vmxnet3_RxCompDesc`.
- **Constants:** the DriverShared magic, ring alignment and size rules, the
  initial generation value, the RX mode bits, the interrupt-disable flag.

Add a `C_ASSERT` on the size of every structure, and on key field offsets,
against the sizes the DPDK headers state. Keep the BSD copyright notice if
any text is copied.

**Why.**
- The device reads these structures directly from guest memory, so a
  one-byte layout error means the device rejects activation or quietly
  corrupts memory.
- The compile-time checks catch layout mistakes on both compilers and both
  architectures.
- BSD licensing keeps the project free of GPL code.

**Done when:** the header compiles for ARM64 and x64 and every `C_ASSERT`
passes.

---

## Phase 3: Device bring-up (initialize only)

The aim of this phase is to show that the device accepts our configuration,
before any packet code exists. Like the probe, this module stops after
initialization and reports. It's step 1's tool with real work added.

### 3.1 Hardware context and memory layout

**What.** Define one context structure at the start of the memory block,
followed by every structure the device reads by DMA:

| Region | Size (for 32-entry rings) | Alignment |
|---|---|---|
| Module context (state, indexes, pointers) | ~1 KB | 64 B |
| `Vmxnet3_DriverShared` | ~1 KB | 8 B (use 64 B) |
| TX queue descriptor + RX queue descriptor | 2 × 256 B | 128 B |
| TX ring (32 × 16 B) | 512 B | 512 B |
| TX data ring (32 × 128 B; early revisions expect it) | 4 KB | 512 B |
| TX completion ring (32 × 16 B) | 512 B | 512 B |
| RX ring 0 (32 × 16 B) | 512 B | 512 B |
| RX ring 1 (minimum size; device expects it) | 512 B | 512 B |
| RX completion ring (64 × 16 B) | 1 KB | 512 B |
| TX buffers (32 × 2 KB) | 64 KB | 64 B |
| RX buffers (32 × 2 KB, plus ring 1 buffers) | 64–128 KB | 64 B |

Have `KdGetHardwareContextSize` return the total size plus alignment slack.
Compute every physical address with `KdGetPhysicalAddress`.

**Why.**
- KDNET gives the module one block of memory and nothing else. There's no
  allocator, so everything must be laid out up front.
- vmxnet3 needs 512-byte-aligned rings and 128-byte-aligned queue
  descriptors.
- A 2 KB buffer holds a full 1514-byte frame, so every packet fits in one
  descriptor.
- Ring sizes must be multiples of 32. 32 entries is plenty for debugger
  traffic and keeps the block small.

**Done when:** the layout is computed in a single function, checked against
the block's length, and a debug report prints each region's virtual and
physical address.

### 3.2 Physical memory rules

**What.** Using the probe results:
- If the block isn't physically contiguous, place each ring and each buffer
  so it never crosses a page boundary, and look up the physical address per
  page.
- If the block is cached, use explicit barriers (step 4.4). Decide whether
  to ask for uncached memory through the `Memory.Cached` field before
  returning from `KdInitializeLibrary`.

**Why.** The device takes one physical base address per ring. A ring that
crosses a page boundary into a non-adjacent page would make the device read
the wrong memory.

**Done when:** every region's physical range is verified at initialization,
and initialization fails cleanly with a clear error string if it isn't.

### 3.3 `KdInitializeController`: bring-up sequence

**What.** Implement in this order (the order is the device's documented
bring-up, as the BSD and Linux drivers do it):

1. **PCI.** Make sure memory decode and bus mastering are on in the command
   register.
   *Why:* without bus mastering the device can't read the rings.
2. **Revision.**
   1. Read VRRS, a bitmask of the revisions the device supports.
   2. Pick the lowest one that gives what we need (revision 1 or 2; nothing
      newer is needed) and write that bit back.
   3. Do the same with UVRS, choosing UPT version 1.

   *Why:* a low revision uses the oldest, simplest descriptor formats, which
   every vmxnet3 device supports.
3. **Reset.** Issue `RESET_DEV`.
   *Why:* clears any state left by firmware, an earlier boot, or the NDIS
   driver before a warm reboot.
4. **MAC.** Read MACL/MACH and copy the 6 bytes to `*TargetMacAddress`.
   *Why:* KDNET needs the MAC to build Ethernet frames and answer ARP.
5. **Fill `DriverShared`:**
   - the magic value
   - driver info: guest OS type Windows, 64-bit, the revision and UPT
     version chosen above
   - no UPT features (no checksum offload, no LRO, no VLAN stripping)
   - MTU 1500, one TX queue, one RX queue, the queue descriptor area's
     address and length
   - interrupt configuration: one interrupt, everything disabled
     (`intrCtrl` = disable all), and IMR masked
   - RX filter: unicast + broadcast + all-multicast, with VLAN 0 allowed in
     the VLAN filter table so untagged frames pass

   *Why:*
   - Debug devices must run without interrupts.
   - Broadcast is needed for ARP and DHCP, and multicast for IPv6.
   - Without the VLAN 0 entry the device can drop untagged traffic. The
     Linux driver sets that bit for exactly this reason.
6. **Fill the queue descriptors.** Ring physical addresses and sizes for the
   TX, TX data, TX completion, RX ring 0, RX ring 1 and RX completion rings.
7. **Activate.**
   1. Write the `DriverShared` physical address to DSAL/DSAH.
   2. Issue `ACTIVATE_DEV`.
   3. Read CMD back: 0 means success.

   *Why:* this is the step where the device checks our whole configuration.
   A non-zero result means a layout or configuration error.
8. **Post-activate.**
   1. Issue `UPDATE_RX_MODE`, `UPDATE_MAC_FILTERS` and
      `UPDATE_VLAN_FILTERS`.
   2. Fill RX ring 0 (and ring 1) with buffers, with the generation bit set.
   3. Write the RX producer index to RXPROD/RXPROD2.
9. **Link.**
   1. Poll `GET_LINK` until bit 0 (link up) is set, with a timeout of a few
      seconds measured by `KdReadCycleCounter`.
   2. Store the speed (upper 16 bits, in Mbps) in `LinkSpeed`, full duplex in
      `LinkDuplex`, and the link state in `*LinkState`, in the units step 1.7
      confirmed.

   *Why:* Microsoft requires the link to be up before returning, but the
   routine must never stall forever (for example with the adapter
   disconnected in VMware).
10. **Errors.** On any failure:
    1. Write a short reason and the failing value to `KdNetErrorString`,
       e.g. `vmxnet3: ACTIVATE_DEV returned 0x00000001`.
    2. Reset the device.
    3. Return a failure status.

    *Why:* this string is the only diagnostic available in the field.

**Done when:**
- The init-only build reports `ACTIVATE_DEV=0`, link up and the right MAC on
  every target VM.
- VMware's `vmware.log` shows no vmxnet3 errors.

---

## Phase 4: Packet path

### 4.1 Receive: `KdGetRxPacket` / `KdReleaseRxPacket`

**What.**
- **`KdGetRxPacket`:**
  1. Look at the next RX completion entry. If its generation bit doesn't
     match the expected value, return `STATUS_IO_TIMEOUT` at once.
  2. Otherwise, read barrier, then read the ring index, descriptor index,
     length, start/end-of-packet flags and error flag.
  3. Drop and recycle any frame with an error or more than one fragment.
  4. Return the buffer address and length, with a handle equal to the
     buffer index (top 3 bits clear).
  5. Advance the completion index, flipping the expected generation bit on
     wrap.
- **`KdReleaseRxPacket`:**
  1. Put the released buffer into the next free RX descriptor slot (address
     and length, buffer type "head").
  2. Barrier, then flip the generation bit.
  3. Advance the fill index and write RXPROD (or only when the device asks,
     through `updateRxProd`).

**Why.**
- KDNET polls with interrupts off, so receive is a pure "is anything there?"
  check that never waits.
- KDNET may call `KdGetRxPacket` several times before releasing, and may
  release out of order. Keeping buffers separate from descriptor slots
  handles that without copying.
- The device writes the completion fields before the generation bit, so the
  driver must read the generation bit first and the other fields after a
  barrier.

**Done when:** a test build that logs received frames shows ARP and DHCP
traffic from the VMware network.

### 4.2 Transmit: `KdGetTxPacket` / `KdSendTxPacket`

**What.**
- **`KdGetTxPacket`:**
  1. Collect finished TX completions (generation check, as on receive).
  2. Hand out a free TX buffer with handle `TRANSMIT_HANDLE | index`.
  3. If none is free, return `STATUS_IO_TIMEOUT`.
- **`KdGetPacketAddress` / `KdGetPacketLength`:** for TX handles, return the
  buffer address and its 2 KB capacity. For RX handles, return the buffer
  and the received length.
- **`KdSendTxPacket`:**
  1. Fill the next TX descriptor: address, length, end-of-packet, completion
     requested, no offload, no VLAN.
  2. Barrier, then flip the generation bit as the final write.
  3. Write TXPROD for every packet (no batching).
  4. If `TRANSMIT_ASYNC` is clear, poll the completion ring until this
     descriptor completes, for up to 100 ms, then return `STATUS_IO_TIMEOUT`.
  5. If it's set, return at once.

**Why.**
- KDNET writes frames directly into our buffers, so no copy is needed.
- Debugger packets must be sent synchronously. Dump transfers and OS
  traffic are sent asynchronously for speed.
- The ring completes in order, so waiting for a synchronous packet also
  covers any earlier asynchronous ones, as Microsoft requires.
- Writing the doorbell for every packet avoids the device's batching
  threshold and keeps latency predictable.

**Done when:** WinDbg on the host connects to the target, as described in
5.1.

### 4.3 Error recovery

**What.**
- On each receive and transmit call, check the event register (ECR) for
  TX/RX queue-error bits, at low cost: read it only every N calls, or after
  a timeout.
- On a queue error, run `GET_QUEUE_STATUS` for diagnostics, then do a full
  reset and re-initialization (3.3), and set `RestartKdnet` in the shared
  data if step 1.7 showed it means "restart the connection".
- Acknowledge link-change events and update `*LinkState`.

**Why.**
- vmxnet3 stops a queue after an error, and only a reset recovers it.
- Without recovery the debugger connection would hang for good after a
  single bad descriptor.

**Done when:** a deliberately corrupted descriptor (test build only) leads
to recovery and the debugger reconnecting.

### 4.4 Memory ordering on ARM64 and x64

**What.** Use compiler intrinsics at four points:
1. before the TX generation-bit write
2. after reading an RX or TX completion generation bit
3. before the RX generation-bit write
4. before every doorbell write

The intrinsics:
- ARM64: `__dmb(_ARM64_BARRIER_ISHST)` for stores and
  `__dmb(_ARM64_BARRIER_ISHLD)` for loads. Use `OSH`/`SY` variants if
  testing shows the device can see stale data.
- x64: `_ReadWriteBarrier()` (x86 ordering covers the rest).

**Why.**
- These are the places where the Linux driver uses `dma_wmb()`/`dma_rmb()`.
- ARM64 reorders memory accesses, so without barriers the device could see
  the generation bit before the descriptor contents. The result is
  intermittent, hard-to-reproduce corruption, and only on ARM64.
- Intrinsics compile to instructions, not imports.

**Done when:** each barrier has a comment naming the ordering it protects,
and the ARM64 stress test (5.3) passes.

---

## Phase 5: Functional testing

### 5.1 First debugger connection

**Observed (2026-10-23):** the user confirmed WinDbg connected and break/resume
worked on ARM64 25H2 build 26200.9711, followed by 20 successful break/resume
cycles and another successful break/resume after one hour idle. The full
inspection-command checklist and x64 sessions below remain outstanding.
The subsequent `lmDvm kd_02_15ad` report confirms v0.3.0.0 and private PDB
symbols; its timestamp, checksum, image size and PDB identity match the local
ARM64 build. Runtime-counter inspection is also recorded, with no faults
reported at the sampled point. Process and stack inspection remain pending.

**What.** On each target VM:
1. Set real `dbgsettings` (host IP, port, a generated key).
2. Install the full module and reboot.
3. Run `windbg -k net:port=...,key=...` on the host.

Then check the session:
- the initial break-in
- `lm`, `!process 0 0`, `k`
- breakpoints, `g`, `Ctrl+Break`

**Why.** This is the main use case, end to end: DHCP, KDNET's protocol, and
both directions of the packet path.

**Done when:** a full session (break, inspect, continue, break again) works
on every target build.

### 5.2 OS networking through the debug NIC

**Passed on ARM64 25H2:** a 5.5G file copied in both directions between macOS
and the target through kdnic with matching MD5 checksums. Five post-copy
break/resume cycles passed. Post-copy counters show 4,344,512 TX completions,
8,296,600 RX packets and zero recorded drops, timeouts, restarts or queue
errors. Other target builds remain untested.

**What.** With the debugger connected, check that Windows' "Microsoft Kernel
Debug Network Adapter" (kdnic) gets an IP address and can browse and ping,
and that large file copies over it work.

**Why.**
- While KDNET owns the vmxnet3 device, all normal Windows traffic on that
  adapter goes through our module as asynchronous sends and receives.
- This exercises the module much harder than debugger traffic does.

**Done when:** a 1 GB file copy over kdnic finishes with the debugger still
responsive.

### 5.3 Stress and edge cases

**Observed on ARM64:** 20 break/resume cycles and a one-hour idle session
followed by successful break/resume. Break/resume also worked after a
30-second virtual-NIC interruption, with zero recorded restarts/timeouts.
The link test did not exercise queue-reset recovery; the later explicit-reset
checkpoint passed separately. On 2026-10-24 the user reported completion of
the complete-memory-dump procedure, including the requested follow-up checks.
The final sample recorded Running 1, Result 0, 4,856,097 TX completions and
740,335 RX packets, with zero resets, drops, timeouts and queue errors. Dump
size and elapsed time were not supplied. A subsequent reported 30-minute
traffic run added 41,995 TX completions and 77,686 RX packets, with Running 1,
Result 0 and no resets, drops, timeouts or queue errors in either snapshot.
The user confirmed continuous copying, matching checksums and successful
break/resume every five minutes, passing this functional checkpoint. File size
was estimated at about 500 MB but was uncertain and the file was deleted;
exact transfer volume, throughput and saturation are not established. See
transport-results.md. Broader coverage below remains open.

**What.**
- Hours-long idle sessions.
- Repeated break/continue loops.
- `.dump /f` complete memory dump over the debugger transport while the target
  is stopped (reported complete on ARM64; other targets remain open).
- 50 reboots in a row with the debugger attached.
- Disconnecting the virtual NIC in VMware: initialization must time out
  cleanly, and the session must recover after reconnecting.
- A host that isn't running WinDbg (boot and later attachment passed on the
  ARM64 VM; Result 0, Running 1, 29,818 TX completions and 30,436 RX packets,
  with zero resets, drops, timeouts or queue errors in the supplied sample).

**Why.** Ring wrap-around, generation-bit handling, timeouts and recovery
are where polled drivers fail, and they only show up over long runs.

**Done when:** every case passes on ARM64 and on at least one x64 build.

### 5.4 Power management

**What.** Implement and test:
- **`KdShutdownController`:** wait (with a limit) for every pending TX to
  complete, then `QUIESCE_DEV`, `RESET_DEV`, and turn bus mastering off.
- **`KdSetHibernateRange`:** register the module's image (and the hardware
  context, if needed) with `PoSetHiberRange`. Confirm the right flags first,
  from the WDK documentation and by testing.

Then test sleep and resume, hibernate and resume, and shutdown with the
debugger attached.

**Why.**
- Microsoft requires that shutdown not drop frames already queued, and that
  the module be usable very early during resume from hibernation.
- After resume, KDNET calls `KdInitializeController` again, so
  re-initialization must work from any device state.

**Done when:** the debugger reconnects after each power transition on every
target.

### 5.5 Device revision and host coverage

**What.** Repeat 5.1 and 5.3 on:
- different VMware products and virtual hardware versions: Workstation 17,
  ESXi 8, Fusion 13 on Apple Silicon
- if it's easy to set up, QEMU's `-device vmxnet3` on x64

**Why.** Each host can offer a different set of vmxnet3 revisions in VRRS.
The module must work with the lowest revision it picks, whatever the newest
one offered is.

**Done when:** a coverage table (host, hardware version, VRRS value, result)
is filled in `docs/test-results.md`.

---

## Phase 6: Documentation and license

Deployment follows the manual instructions in README.md and transport.md.
Automated installation/removal and production-signing work are outside the
current plan. The existing test-signing tools and recorded loading limitations
remain applicable to lab use.

**License recorded (2026-10-24):** the project's own code uses BSD-3-Clause;
the root [LICENSE](../LICENSE) names Holly Lee <holly.lee@gmail.com> as the
copyright holder. Third-party licenses and notices remain in place.

**What.**
- Keep the BSD-3-Clause license and the project's copyright notice with distributions.
- Add the BSD notices for any copied vmxnet3 definitions.
- Expand the README with supported builds and hosts, setup,
  troubleshooting (the error strings from 3.3), and known limits.

**Why.** Users need to know where the module works and how to read its
errors. The third-party notices are a license requirement.

**Done when:** the README covers install, use, troubleshooting and limits,
and every third-party file has its notice.

---

## Scope boundaries

These are left out on purpose, to keep the module small (about 1–2k lines)
and debuggable:

- interrupts, MSI-X, multiple queues, RSS
- checksum offload, TSO, LRO, VLAN tagging, jumbo frames
- `KdDeviceControl` and the serial-only exports
- Windows on x86 or 32-bit ARM
- x64 24H2+ (the inbox module covers it)

---

## Risks and open questions

| Risk | Effect | Mitigation | Resolved in |
|---|---|---|---|
| ReactOS header layout wrong on some build | Crash at boot | Probe first; tolerant count handling | 1.7 |
| Test-signed module not loaded by winload | No test path at all | **Seen on ARM64 25H2:** winload rejects every test certificate tried for KDNET modules, but loads ours when booted with "Disable driver signature enforcement" (Startup Settings, option 7). Use that procedure for lab boots | 1.5a |
| KDNET doesn't keep the error string in the registry | Probe output unseen | Serial boot debugger | 1.5 |
| Memory block not physically contiguous | Ring corruption | Page-aware layout | 3.2 |
| ARM64 ordering bugs | Intermittent corruption | Barriers + ARM64 stress | 4.4, 5.3 |
| No debug channel on VMware Fusion ARM64: no e1000e NIC, and no ACPI DBG2 table, so no serial debugging (seen 2026-10) | ARM64 module can only be tested as a black box | Develop and debug on x64 VMware (Workstation/ESXi, which have e1000e and serial); on ARM64 use the freeze variants and the error string | 1.5, 5.5 |
| Hypervisor/VBS shares or hides the debug device | KDNET picks no device | Try the `FORCEHVTONOTSHAREDEBUGDEVICE` load option (used for AWS ENA) | 1.5, 5.1 |
| Production signing unavailable to third parties | Lab-only use | Document test-signing requirements and loading limitations | Outside current scope |
| Microsoft ships an ARM64 module | Project redundant on ARM64 | Check target builds for an inbox module before manual deployment | Manual deployment |

## Rough effort

| Phase | Estimate |
|---|---|
| 1. Probe runs and analysis | 2–4 days (mostly VM setup) |
| 2. Foundations | 1–2 days |
| 3. Bring-up | 3–5 days |
| 4. Packet path | 4–6 days |
| 5. Testing and power management | 5–8 days |
| 6. Documentation and license | 2–3 days |

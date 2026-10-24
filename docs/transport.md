# Packet-transport validation

As of 2026-10-23, `out/<arch>/transport/kd_02_15ad.dll` is the experimental
phase 4 module, version **0.3.0.0**, with description
**`vmxnet3-kdnet transport (experimental)`**. It returns `STATUS_SUCCESS`
after successful initialization and leaves the queues running. The separate
`init-only` variant still stops DMA and deliberately returns `0xE2802710`
when its checks pass at 10 Gbps.

**The first ARM64 WinDbg connection and break/resume passed.** On the tested
25H2 VM, KDNET reported initialization success, IP `192.168.74.153`, and
RX/TX success values of 1. The user confirmed the interactive session worked.
See [transport-results.md](transport-results.md) for the evidence and limits.
Subsequent runtime inspection showed 23,123 TX completions and 37,552 RX
packets, with no recorded drops, timeouts, restarts or queue errors. This
establishes packet transport and repeated ring wraparound on that VM; timed sustained traffic,
fault recovery, power transitions and x64 target validation remain open.

## Implemented behavior

- One TX queue and one RX queue, revision 1/UPT v1, MTU 1500, polled operation.
  No interrupt delivery, offloads or VLAN-tagged packet support.
- RX checks completion ownership before reading fields or payload. It returns
  immediately when empty, drops erroneous/fragmented/oversized packets, and
  recycles skipped descriptors. Retained packets have separate buffer ownership
  so callers can release them in any order.
- TX reserves a buffer per handle, rings the doorbell for every packet,
  pads Ethernet frames shorter than 60 bytes, and requests completions.
  Asynchronous sends return after posting. Synchronous sends wait for their
  completion and preceding sends, with a 100-ms deadline and a finite poll
  limit if the counter freezes. Full queues return immediately.
- Tokens distinguish a current buffer reservation from stale handles until
  the 23-bit token sequence wraps. The top three handle bits remain available
  for KDNET's TX flags.
- Queue faults, invalid completions and TX timeouts stop DMA and reset queues.
  Held RX/TX buffers, payloads and tokens survive recovery; device-owned TX
  packets are discarded. RX polling also reclaims TX completions and checks
  the asynchronous-send deadline. Failed recovery leaves the device stopped.
- Link changes update shared fields. Memory barriers order payloads,
  descriptors, ownership and doorbells. VMware DMA coherence is assumed.

The code accepts packet callback arguments equal to either the saved shared
data pointer or its hardware-context pointer. The successful session validates
the implemented callback path on this build, but does not identify which of
those two pointer forms KDNET supplied. Link-field units
remain provisional. The error-string imports and `RestartKdnet` are unused.
Sleep/hibernate image registration and resume validation remain unimplemented;
keep the VM awake during this test.

## Local validation

`build.cmd all test` builds with `/W4 /WX`, validates DLL imports, export and
relocation metadata, and runs initialization and packet tests on ARM64 and
x64 (the latter under emulation on the development machine). The device
simulation uses independent ring consumer/producer indices and literal wire
fields, with discontiguous DMA pages above 4 GB.

Coverage includes multiple ring wraps; full TX reservations and outstanding
async sends; reverse-order submission and RX release; stale handles; bad
frame lengths/errors/fragments; skipped RX descriptors; sync versus async
completion; frozen-counter deadlines; retained payloads across reset; queue
errors and malformed completions; link changes; shutdown and recovery failure.
The original initialization/checkpoint tests also pass. These tests cannot
establish VMware's actual DMA ordering, packet callback ABI, or connectivity.

`sign.cmd windows` test-signs all outputs. Current hashes, sizes and version
labels are in `out/transport-manifest.json`; the matching PDB is beside each
DLL. Rebuilding requires signing again. Startup Settings option 7 is still
required on the tested ARM64 target.

## Run on the existing ARM64 VM

This procedure produced the first successful connection and can be reused
for subsequent test boots.

1. Keep a recoverable VM snapshot and console access. Copy the **transport**
   DLL and updated `tools/check-target.ps1` to the VM. In an elevated prompt,
   replace `C:\Windows\System32\kd_02_15ad.dll` with that DLL. Save its
   `Get-FileHash -Algorithm SHA256` result. Retain the init-only DLL separately.
   Do not replace the Windows inbox module on x64 24H2 or later.
2. Keep the successful target settings: vmxnet3 at `1.0.0`, host IP
   `192.168.74.132`, port `50000`, test signing on and Secure Boot off.
   Confirm that this IP still belongs to the Windows machine running WinDbg.
   Reuse the configured encryption key; do not put it in shared logs.
3. Start WinDbg on that debugger host with the same port/key, and allow the
   debugger through its firewall:

   ```text
   windbg -k net:port=50000,key=<existing-key>
   ```

   This follows Microsoft's [manual KDNET setup](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/setting-up-a-network-debugging-connection).
   The host must be reachable from the VM's selected VMware network. Leave
   WinDbg listening before rebooting the target.
4. On the target, use elevated PowerShell to clear stale diagnostic values
   and request Startup Settings:

   ```powershell
   reg delete HKLM\SYSTEM\CurrentControlSet\Services\kdnet /f
   bcdedit /set '{current}' onetimeadvancedoptions on
   shutdown /r /t 0
   ```

   Select **7 / F7: Disable driver signature enforcement** for this boot.
5. If WinDbg connects, break in, run `vertarget`, then `g`. Repeat break/resume
   several times. Save the connection banner and command output. This is the
   first acceptance check; initialization alone does not establish a working
   transport.
6. Once Windows is running, run the updated checker in elevated PowerShell:

   ```powershell
   .\check-target.ps1
   ```

   Confirm the transport variant and loaded module. Look for fresh successful
   KDNET initialization, a usable target IP, MAC `00-0C-29-79-75-CD`, and
   RX/TX success values. The observed values of 1 are not established as live
   packet counters, so use session behavior and module diagnostics for traffic
   validation. A reported zero `KdInitStatus` is initialization
   success, not proof of connection. Returning `0xE2802710` means the init-only
   variant is still installed. Save this report and relevant `vmware.log`
   entries, with the debug key redacted.

## Next validation on the working build

Private PDB symbols and matching build metadata were confirmed with
`lmDvm kd_02_15ad`. While broken into the target, inspect the runtime state:

```text
r @$t0 = poi(poi(kd_02_15ad!VmxSharedData))
dt kd_02_15ad!_VMX_CONTEXT @$t0 Running Result LastStage LastDetail
dt kd_02_15ad!_VMX_CONTEXT @$t0 Restarts TxPackets RxPackets RxDropped TxTimeouts LastTxError LastRxError
```

`VmxSharedData` is a pointer variable; its pointee starts with the `Hardware`
pointer, so the two dereferences obtain the hardware context. `$t0` is a
debugger temporary register. The commands read the driver's state; they do
not write target memory. See Microsoft's [dt reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/dt--display-type-)
for field selection.

For a running transport with no recorded faults, expect `Running=1`,
`Result=0`, and zero restarts, timeouts and queue errors. `TxPackets` counts
reclaimed TX descriptors and `RxPackets` counts packets handed to KDNET.
Take another reading after traffic; debugger communication itself also
generates traffic. A recovered fault leaves the original diagnostic result
in place, so nonzero `Result` requires interpretation alongside `Running`,
`LastStage`, `LastDetail` and `Restarts`.

The user has now passed 20 break/resume cycles and one hour idle followed by
break/resume, plus a 5.5G copy in both directions between macOS and the VM
through the Microsoft Kernel Debug Network Adapter with matching MD5 checksums.
Five post-copy break/resume cycles also passed. Post-copy counters show
4,344,512 TX completions and 8,296,600 RX packets, with zero recorded drops,
timeouts, restarts or queue errors. The later 30-second virtual-NIC interruption
test also passed: break/resume worked, counters reached 4,361,619 TX and
8,315,796 RX, and recorded drops, timeouts, restarts and queue errors stayed
zero. See [transport-results.md](transport-results.md).

Keep the current signed DLL and matching PDB as the known working baseline.
Record the installed DLL's SHA-256, debugger inspection (`!process 0 0`, `k`),
and relevant `vmware.log` entries.

### Repeated-boot checkpoint: passed

**All five reboot tests passed.** The user reported successful checker results
and working break/resume on every boot. All five context readings showed zero
drops, timeouts, restarts and saved queue errors. Per-boot packet counts are
recorded in [transport-results.md](transport-results.md).

Use five consecutive reboot tests with the same signed DLL/PDB and
debugger settings. Keep WinDbg listening. Before each reboot, save the current
diagnostics and clear the KDNET registry results as in the deployment steps.
Request Startup Settings and select option 7 on each boot; the exception from
the previous boot does not persist.

After each boot, verify that WinDbg connects and break/resume works, then save
`check-target.ps1` output and both context readings above. Record boot number,
connection outcome and any fault. Re-evaluate the context pointer with the
`r @$t0 = ...` command after every reboot; runtime counters restart with a new
context and should not be compared as one cumulative session across boots.

The five successful boots complete the initial checkpoint toward the plan's
50-boot stress target. Deliberate queue-error recovery and broader target
coverage remain separate tests.

### Current validation: explicit reset and continued operation

The working transport has now passed basic debugging, bidirectional bulk
transfers, idle, link-interruption and five-reboot checks on this ARM64 VM.
Normal traffic and link loss recorded no resets. A subsequent explicitly
requested reset has now succeeded on the ARM64 VM; actual queue-error
detection remains unverified.

The separate v0.3.3.0 `fault-once` variant now provides an explicitly armed,
one-shot call to the recovery routine and a distinct forced-reset record.
The signed v0.3.0.0
DLL/PDB baseline is archived and preserved. Follow [fault-test.md](fault-test.md)
for installation, arming, result interpretation and restoration. The next test
requires successful reset/reinitialization and a usable debugger and packet
path afterwards. It cannot validate device queue-error detection. Both earlier
descriptor triggers (v0.3.1.0 type bit and v0.3.2.0 17-descriptor packet) fired
on the VM but completed without a captured queue error, leaving recovery
unexercised. The v0.3.3.0 explicit reset then returned success with
`FaultOutcome=6`, `FaultRecoveryDone=1`, `FaultRecoveryResult=0`, `Restarts=1`
and `Running=1`. WinDbg continued automatically without reconnecting. All five
post-reset break/resume cycles and checksum-verified copies through kdnic in
both directions passed. Follow-up counters remained at one reset with zero
drops, timeouts and queue errors. The explicit-reset checkpoint is passed on
this ARM64 VM; restore the baseline for continued normal testing. Plan section
4.3 still needs real device queue-error evidence.

### Link-interruption regression procedure (passed once)

1. Save a runtime-context reading, then continue the target with `g` so it is
   running before the link is disconnected. Keep VM console access available.
2. Disconnect the debug NIC's virtual network connection in VMware for
   approximately 30 seconds, then reconnect it. Keep the same WinDbg session
   open; do not remove/re-add the NIC or change its PCI location.
3. Allow up to a minute for reconnection, then attempt break/resume five times.
   Record whether the existing session recovers without rebooting or reinstalling.
4. Read both context commands above again and save any fault stages, timeout
   counts and restart counts, along with relevant `vmware.log` entries. A timeout
   or recovery reset caused by deliberate link loss is not automatically a
   failure; check that the device is running and debugging works afterwards.

This checks link-loss behavior, not deliberately corrupted-descriptor queue
recovery. Continue reboot and broader host/build coverage in plan sections
5.3 and 5.5. Keep sleep/hibernate testing pending implementation of the
hibernate callback. For future throughput measurements, record transfer
duration and exact size as well as hashes.

## If the connection fails

First compare the variant, loaded modules and signing/boot options with the
successful init-only run. Initialization failures use the same `0xE2SSDDDD`
codes described in [bring-up.md](bring-up.md). If initialization succeeds,
check host address, key, port, firewall and VMware network reachability, then
inspect RX/TX counters. A packet capture for the target MAC can distinguish
missing ARP/DHCP traffic from a later debugger handshake problem.

Runtime diagnostics live in `VMX_CONTEXT` and are visible with the matching
PDB through an available debugger; KDNET's registry init status is not a
live copy of them. `Result`, `LastStage`, `LastDetail`, `Restarts`,
`LastTxError`, `LastRxError`, `TxTimeouts`, `TxPackets`, `RxPackets` and
`RxDropped` retain the last fault/counters. New diagnostic stages are:

| Stage | Meaning | Detail |
|---|---|---|
| `11` | Invalid TX completion/bookkeeping | Completion index, or `0100` + buffer / `0200` + slot |
| `12` | Invalid RX completion/bookkeeping | 1: fields; 2: index outside outstanding range; 3: buffer map |
| `13` | TX completion timeout | Oldest slot, waited-for buffer, or pending count at shutdown |
| `14` | Recovery failed | Original stage |

Stage `0E` during recovery uses ECR bits as detail and saves queue error words
separately. A successful recovery retains the original fault for diagnosis.
Stage `0F` overrides it if bus mastering could not be disabled/verified.

If the VM cannot boot or remains stuck, revert the snapshot. From a working
console, disable debugging with `bcdedit /debug off` and reboot to return
the NIC to Windows; reinstall the init-only DLL when isolating initialization.

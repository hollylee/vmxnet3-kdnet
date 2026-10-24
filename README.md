# vmxnet3-kdnet

A KDNET extensibility module (`kd_02_15ad.dll`) that lets Windows kernel
debugging run over VMware's vmxnet3 virtual NIC. It targets the systems where
Windows has no inbox module of its own:

| Target | Inbox vmxnet3 KDNET module | This project |
|---|---|---|
| Windows 11 ARM64 (current builds) | No | Yes |
| Windows 10 x64, Windows 11 x64 before 24H2 | No | Yes |
| Windows 11 x64 24H2 and later | Yes (`kd_02_15ad.dll`) | Not needed; do not overwrite the inbox file |

## License

The project's own code uses the [BSD-3-Clause license](LICENSE).
Copyright (c) 2026, Holly Lee <holly.lee@gmail.com>.
Third-party components retain
their respective licenses: MIT for the ReactOS KDNET headers and BSD-3-Clause
for the VMware/DPDK vmxnet3 definitions. See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

# Motivation

I have a Macbook Pro M4. In it I ran a Windows on ARM in one VMware Fusion VM for my windows developments. Sometimes I need to do some driver development stuffs, so I require an approach to do kernel debugging stuffs between 2 Windows on ARM VMs. But a) VMware Fusion on Apple silicon supports vmxnet3 NIC only. b) Windows on ARM didn't provide KDNET dll for vmxnet3.

That's why I started this project.

## Sources

No GPL code and no code derived from Microsoft or VMware binaries.

- KDNET interface: Microsoft's
  [Develop KDNET Transport Extensibility Modules](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/how-to-develop-kdnet-extensibility-modules),
  the MIT-licensed ReactOS headers in `third_party/reactos/`, and
  `DEBUG_DEVICE_DESCRIPTOR` from the WDK's `ntddk.h`.
- vmxnet3 device interface: BSD-licensed definitions (DPDK
  `drivers/net/vmxnet3/base/`, FreeBSD `if_vmx.c`).

## Status

In short, it works on my Windows 11 on ARM 25H2.

**ARM64 debugging and bidirectional bulk transfers passed; transport remains experimental.**
The project builds a read-only probe, an initialization-only diagnostic, and
a transport with polled RX/TX, buffer ownership, bounded sends and queue
recovery. All variants build for ARM64 and x64; initialization and packet
tests pass on both architectures. On Windows 11 ARM64 25H2 build 26200.9711,
the user confirmed that WinDbg connected and break/resume worked with the
transport. KDNET reported `KdInitStatus=0`, IP `192.168.74.153`, and
`KdNetRxSuccess=1` / `KdNetTxSuccess=1`. Runtime inspection in that session showed
**4,361,619 TX completions and 8,315,796 RX packets** with zero recorded drops,
timeouts, restarts or queue errors, exercising repeated ring wraparound.
The user also passed **20 break/resume cycles** and **one hour idle followed
by break/resume**, and transferred a **5.5G file in both directions between
macOS and the VM through kdnic with matching MD5 checksums**. Five further
break/resume cycles after the reverse transfer also passed. Debugging remained
functional after a **30-second virtual-NIC interruption**, with no recorded
restart or TX timeout. **All five reboot tests passed**, with successful
connections, working break/resume, and zero recorded drops, timeouts, restarts
or queue errors in every supplied context reading. The user also reported
completion of the **complete-memory-dump procedure**; its final sample showed
4,856,097 TX completions and 740,335 RX packets, `Running=1`, `Result=0`, and
zero resets, drops, timeouts or queue errors. These are cumulative session
counts; dump size and duration were not supplied. A subsequent reported
30-minute traffic run added **41,995 TX completions and 77,686 RX packets**
with no resets, drops, timeouts or queue errors. The user confirmed continuous
copying, matching checksums and successful break/resume every five minutes.
Exact transfer volume and throughput were not measured. **Booting without
WinDbg listening and attaching afterward also passed**, with `Running=1`,
`Result=0`, and zero resets, drops, timeouts or queue errors. See the
[transport result](docs/transport-results.md) and
[test guide](docs/transport.md). Broader load testing, device queue-error recovery, power
transitions and x64 target runs remain unvalidated.

A separate **v0.3.3.0 `fault-once` test variant** is available for an explicit
reset/reinitialization test. It is disarmed at boot and requires explicit
WinDbg arming. See [the fault-test guide](docs/fault-test.md). The normal
transport tested above was v0.3.0.0. The first
v0.3.1.0 and v0.3.2.0 triggers fired, but VMware completed both the altered
descriptor and the 17-descriptor packet without a captured queue error.
Neither caused a reset. The v0.3.3.0 explicit reset succeeded on the ARM64 VM:
`FaultOutcome=6`, `FaultRecoveryDone=1`, `FaultRecoveryResult=0`, `Restarts=1`
and `Running=1`, with WinDbg able to read the context afterward. A follow-up
sample showed 87,705 cumulative TX completions and 136,236 RX packets, still
one reset and zero drops, timeouts or queue errors. All five post-reset
break/resume cycles and checksum-verified transfers through kdnic in both
directions passed; WinDbg continued automatically without reconnecting.
Device queue-error detection remains unverified.

On the ARM64 25H2 VM (build 26200.9711), the earlier init-only build returned
**`0xE2802710`**: activation succeeded, link was up at **10 Gbps**, queue
checks passed, and cleanup disabled bus mastering. KDNET recorded the
module-supplied MAC **`00-0C-29-79-75-CD`**, which the user confirmed matches
the NIC. That diagnostic failure status is intentional. See the earlier
[recorded target result](docs/bring-up-results.md).
See [docs/bring-up.md](docs/bring-up.md) for deployment, status codes and
checkpoint builds.

The probe checks the ReactOS-derived KDNET headers against
real Windows builds, only reads device registers and PCI configuration, and
fails initialization on purpose so Windows boots without a debugger.

On 2026-10-22, Windows 11 ARM64 25H2 (build 26200.9711) on VMware Fusion
on Apple Silicon returned the probe summary **`0xE6FF0F21`**, confirming:

- Import/export counts of **33/15**, matching the headers.
- `Adapter` is `KDNET_SHARED_DATA`; its `Device` and `Hardware` fields match
  the debug device descriptor and the module's memory block.
- The first **64 KB** of the memory block is physically contiguous.
- PCI configuration reads identify vmxnet3, and reads of BAR1's VRRS register
  work through its mapped address.
- `KdInitializeLibrary` ran twice in the same image instance, so globals
  survived from winload into the kernel on this build.

The ARM64 gate in plan step 1.7 is passed. The init-only module uses
returned status codes for diagnostics. The ARM64 bring-up run exercised
register-write, PCI-write, stall and cycle-counter imports and the MAC output
pointer. KDNET's interpretation of the link fields and the error-string and
error-status slots still need validation. The probe must also run on the
x64 targets (Windows 10 and Windows 11 before 24H2).

**Loading limitation:** on the tested ARM64 build, both test-certificate
profiles were rejected with test signing on and Secure Boot off. Startup
Settings option 7, "Disable driver signature enforcement", allowed the probe
to run for that boot. A production signing route remains unresolved.

See [docs/probe-results.md](docs/probe-results.md) for the evidence,
[docs/PLAN.md](docs/PLAN.md) for the remaining work, and the
[draft questions to Microsoft](docs/email-microsoft-kdnet-signing.md) about
signing and official interface support.

## Layout

```
build.cmd                 build (MSVC + WDK, command line)
sign.cmd                  test-sign the output
src/common/kdnet_rt.*     import-table copy, memset/memcpy, report buffer
src/probe/                probe module
src/vmxnet3/              transport, initialization diagnostic, hardware definitions
tools/check-target.ps1    target diagnostics and probe-status decoder
third_party/reactos/      KDNET headers (MIT)
third_party/dpdk/         BSD vmxnet3 reference headers and license
out/<arch>/probe/         build output (kd_02_15ad.dll + .pdb)
out/<arch>/init-only/     initialization-only build (kd_02_15ad.dll + .pdb)
out/<arch>/transport/     experimental packet transport (kd_02_15ad.dll + .pdb)
out/<arch>/fault-once/    opt-in queue-fault test variant (separate build)
tests/                   simulated-device and status-decoder tests
```

The `out/` directories are generated locally by the build. Local test archives
under `artifacts/` are not checked in; build and sign your own DLL using the
instructions below.

## Build

Requirements: Visual Studio 2022 or later with the ARM64 and x64 C++ tools,
and WDK 10.0.26100.0.

```
build.cmd            :: arm64 and x64
build.cmd arm64
build.cmd x64
build.cmd all test   :: also run simulated-device tests
build.cmd all test fault-only :: build only the fault test DLLs; run all tests
```

Run the status-decoder tests with
`powershell -NoProfile -ExecutionPolicy Bypass -File tests\status_test.ps1`.
Builds include probe, init-only and transport variants. ARM64 test executables are
compiled but skipped on x64 development hosts. These checks do not replace
the target-VM tests in [docs/bring-up.md](docs/bring-up.md).

The build fails if the DLL has any imports, or any export other than
`KdInitializeLibrary`, because KDNET rejects such modules. Override the tool
locations with `VSINSTALL` and `WDKVER` if needed.

## Sign

```
sign.cmd
```

Use `sign.cmd windows fault-once` to sign only the separate fault-test outputs.

The default profile is `basic`. Its first run creates a self-signed
certificate `CN=vmxnet3-kdnet test` in
your CurrentUser\My store and exports it to `out\vmxnet3-kdnet-test.cer`.
`sign.cmd windows` uses a test certificate chain with Windows System
Component Verification and Code Signing usages; this was the profile used
for the successful ARM64 probe and transport runs.

Neither profile alone made winload accept the module on ARM64 25H2, even
with test signing on and Secure Boot off. Use Startup Settings option 7 for
each development boot on that target, as described below. Run signing again
after every build, because building replaces the signed DLLs.

## Manual installation and KDNET setup

The **target** is the Windows VM being debugged. The **debugger host** is the
Windows computer running WinDbg; it may be a separate VM on the same macOS
host. Run the target commands below in an **elevated native PowerShell**.
The tested target is Windows 11 ARM64 25H2 build 26200.9711 on VMware Fusion.
The x64 builds still need target-VM validation.

Use a vmxnet3 virtual NIC, keep VM console access, and take a recoverable
snapshot before changing the module or boot configuration. Turn off Secure
Boot in the VM firmware. If BitLocker is enabled, retain its recovery key
and suspend protection before changing boot/security settings.

**Do not replace the Windows inbox `kd_02_15ad.dll` on x64 Windows 11 24H2
or later.** The instructions below are for targets that need this project's
module. The debug adapter must have a network path to the debugger host.

### 1. Select and copy the transport DLL

Use the normal **transport** variant for the target architecture:

| Target | DLL and matching PDB on the build computer |
|---|---|
| ARM64 | `out/arm64/transport/` |
| x64 before Windows 11 24H2 | `out/x64/transport/` |

On the build computer, run `build.cmd arm64` or `build.cmd x64` for your
target, then `sign.cmd windows`. The paths above contain the resulting DLL
and PDB. Use the DLL and PDB from the same build.
The `probe`, `init-only`, and `fault-once` variants are diagnostic builds;
choose `transport` for normal debugging.

After signing, record the DLL's SHA-256 hash on the build computer. For ARM64,
run this from the project directory (use `x64` instead for an x64 target):

```powershell
Get-FileHash -LiteralPath .\out\arm64\transport\kd_02_15ad.dll -Algorithm SHA256
```

On the target VM, create a staging folder:

```powershell
New-Item -ItemType Directory -Force -Path C:\KDNET | Out-Null
```

Transfer the selected `kd_02_15ad.dll` and `tools/check-target.ps1` into
`C:\KDNET`. Put the matching `kd_02_15ad.pdb` in a symbol folder on the
**debugger host**, for example `C:\Symbols\vmxnet3`; it need not be copied
into the target's System32 directory.

Inspect the staged DLL:

```powershell
(Get-Item -LiteralPath C:\KDNET\kd_02_15ad.dll).VersionInfo |
    Format-List FileVersion, FileDescription
Get-FileHash -LiteralPath C:\KDNET\kd_02_15ad.dll -Algorithm SHA256
```

The current normal transport source builds version **0.3.0.0** with description
**`vmxnet3-kdnet transport (experimental)`**. Compare the staged DLL's hash with
the hash you recorded after signing on the build computer. A local rebuild
or a different signing certificate can produce a different hash from the
historical test binary. Preserve a copy of any existing project module before
replacing it; stop if the existing file's origin is unknown or it belongs to
Windows. Then copy the staged DLL into the target's system directory:

```powershell
$modulePath = Join-Path $env:SystemRoot 'System32\kd_02_15ad.dll'
Copy-Item -LiteralPath C:\KDNET\kd_02_15ad.dll -Destination $modulePath -Force
Get-FileHash -LiteralPath $modulePath -Algorithm SHA256
```

The installed hash should match the staged file. KDNET loads the module at
boot; copying it does not replace an already loaded module in memory.

### 2. Find the vmxnet3 PCI location

On the target:

```powershell
Get-NetAdapterHardwareInfo | Format-Table Name, Bus, Device, Function
```

Identify the vmxnet3 adapter. If it is not listed, open Device Manager,
select **vmxnet3 Ethernet Adapter**, and read **Properties > General >
Location**. Use the bus, device and function as decimal `bus.device.function`.
The tested VM uses **`1.0.0`**; use your VM's actual location.

### 3. Configure the target's debugger connection

If the VM already has a working KDNET configuration, retain its host IP,
port, key and PCI location. For a new setup, replace the example debugger-host
IP and PCI location below with yours:

```powershell
bcdedit /set '{current}' testsigning on
bcdedit /debug '{current}' on
bcdedit /dbgsettings net hostip:192.168.74.132 port:50000 newkey
bcdedit /set '{dbgsettings}' busparams 1.0.0
```

`newkey` generates a connection key. Save it privately for WinDbg; do not
include it in shared logs. To reuse an existing key when changing network
settings, replace `newkey` with `key:<your-existing-key>`; do not specify both.
The host IP is the address of the computer running WinDbg, not the target
VM's address. The example uses UDP port 50000; the host and target must agree
on the port and key. `/dbgsettings` is global across boot entries. See
[Microsoft's BCDEdit reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/bcdedit--dbgsettings).

### 4. Start WinDbg on the debugger host

Allow WinDbg through the host firewall for the selected network and UDP
port. Start a NET kernel-debugging session with the same port and key:

```text
windbg -k net:port=50000,key=<your-key>
```

Alternatively, enter the port/key in WinDbg's kernel-debugging NET dialog.
Leave WinDbg listening. After it connects, add the matching private symbols:

```text
.sympath+ C:\Symbols\vmxnet3
.reload /f kd_02_15ad.dll
lmDvm kd_02_15ad
```

### 5. Reboot the target with signature enforcement disabled

**On the tested ARM64 25H2 VM, test signing on and Secure Boot off were not
sufficient. Startup Settings option 7 is required for each development boot.**
This is the loading procedure validated for the working transport.

To obtain fresh diagnostic results, save any old checker output first, then
clear the KDNET diagnostic key. Windows repopulates it during initialization;
a missing key on the first setup is harmless:

```powershell
reg.exe delete HKLM\SYSTEM\CurrentControlSet\Services\kdnet /f
bcdedit /set '{current}' onetimeadvancedoptions on
shutdown /r /t 0
```

At Startup Settings, select **7 / F7: Disable driver signature enforcement**.
The exception applies only to that boot. WinDbg should connect; if the target
is stopped in the debugger, use `g` to let Windows continue booting.

### 6. Verify the connection

On the running target, use an elevated PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File C:\KDNET\check-target.ps1
```

Check for the transport variant, a loaded `kd_02_15ad.dll`,
**`KdInitStatus=0`**, a usable target IP address and the NIC's MAC address.
The Microsoft Kernel Debug Network Adapter should be present. Its existence
and the registry success values alone are not proof of a working connection:
break into the target in WinDbg, run `vertarget`, and resume with `g`.
Repeat break/resume several times.

With the matching private PDB loaded, inspect live driver state while stopped:

```text
r @$t0 = poi(poi(kd_02_15ad!VmxSharedData))
dt kd_02_15ad!_VMX_CONTEXT @$t0 Running Result Restarts TxPackets RxPackets RxDropped TxTimeouts LastTxError LastRxError
```

For an error-free normal session, expect `Running=1`, `Result=0`, increasing
packet counters, and zero drops, timeouts, restarts and saved queue errors.
Re-evaluate the context pointer after every reboot. See
[transport validation](docs/transport.md) for recovery diagnostics and further
checks. The completed probe evidence remains in [probe-results.md](docs/probe-results.md).

### Troubleshooting

| Symptom | Check |
|---|---|
| Only `kd.dll` is loaded | Confirm debug is enabled for the current boot entry, the correct signed DLL was copied, Secure Boot is off, and option 7 was selected on the tested ARM64 target. |
| `KdInitStatus=0xE2802710` | The init-only diagnostic was installed. Copy the normal transport DLL and reboot. |
| Transport initializes but WinDbg does not connect | Check the debugger-host IP, matching UDP port/key, host firewall, VMware network connectivity and selected PCI location. |
| Signature shows `UnknownError` | The tested component certificate chain produced this result too. Check signer identity and the installed hash; use loaded-module/status output to establish whether the loader accepted the DLL. `NotSigned` or a missing signer requires checking the build/signing step. |
| Private symbols do not load | Use the PDB from the exact DLL build, add its folder with `.sympath+`, and reload the module. |
| vmxnet3 has Device Manager status Error | This was observed while KDNET owned the NIC in successful tests. Check the kernel debug adapter, initialization status and actual WinDbg connection. |

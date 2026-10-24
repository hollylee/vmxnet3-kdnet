# Collects the facts needed to tell why KDNET did or did not load the
# kd_02_15ad.dll module on this machine. Run in an elevated PowerShell on the
# target after rebooting with debugging configured:
#
#   powershell -ExecutionPolicy Bypass -File check-target.ps1
#
# The output never includes the debug key.
#
# KDNET only overwrites the values it has new data for, so clear its key
# before each test boot, or old values will look like new results:
#
#   reg delete HKLM\SYSTEM\CurrentControlSet\Services\kdnet /f

# Decode a saved status without inspecting this machine or requiring elevation:
#   .\tools\check-target.ps1 -DecodeStatus 0xE2802710
param([long] $DecodeStatus)

function Format-InitStatus([long] $RawStatus) {
    $status = [BitConverter]::ToUInt32([BitConverter]::GetBytes($RawStatus), 0)
    $stage = ($status -shr 16) -band 0xFF
    $detail = $status -band 0xFFFF
    $stages = @{
        1 = 'import/export table counts'; 2 = 'required import slot'
        3 = 'adapter/device identity'; 4 = 'memory block'; 5 = 'mapped BARs'
        6 = 'PCI configuration'; 7 = 'revision negotiation'; 8 = 'reset and interrupt masks'
        9 = 'DMA layout'; 10 = 'cycle counter/stall'; 11 = 'MAC address'
        12 = 'device activation'; 13 = 'link'; 14 = 'queue status'
        15 = 'cleanup (bus mastering could not be disabled/verified)'; 16 = 'shared output pointers'
        17 = 'TX completion'; 18 = 'RX completion'; 19 = 'TX timeout'; 20 = 'queue recovery'
    }
    if ($stage -eq 0x80) {
        '  Init-only: activation, link and queue checks passed; device stopped.'
        '    observed link speed         {0} Mbps' -f $detail
        '    initialization intentionally failed; this variant has no packet transport'
    } elseif ($stage -in @(0x46, 0x48, 0x49, 0x4A, 0x4C)) {
        '  Init-only checkpoint passed: {0}; device stopped.' -f $stages[[int]($stage -band 0x3F)]
    } else {
        $name = if ($stages.ContainsKey([int]$stage)) { $stages[[int]$stage] } else { 'unknown stage' }
        '  vmxnet3 failure: stage {0} ({1}), detail 0x{2:X4}' -f $stage, $name, $detail
        if ($stage -eq 15) {
            '    original stage              0x{0:X2}; inspect VM before another run' -f $detail
        }
        '    See docs/bring-up.md and docs/transport.md for detail codes.'
    }
}

function Format-KdnetStatus([long] $RawStatus) {
    # Handle both signed REG_DWORD and positive Int64 representations.
    $status = [BitConverter]::ToUInt32([BitConverter]::GetBytes($RawStatus), 0)
    'KdInitStatus:      0x{0:X8}' -f $status
    if ($status -eq 0) {
        '  KDNET reports initialization success; verify packet counters and a WinDbg connection.'
    } elseif (($status -shr 24) -eq 0xE2) {
        Format-InitStatus $RawStatus
    } elseif (($status -shr 29) -eq 7 -and (($status -shr 26) -band 3) -eq 1) {
        function Bit($n) { [int](($status -shr $n) -band 1) }
        '  Probe summary (format 1):'
        '    set via                     {0}' -f $(if (Bit 28) { 'KdNetErrorStatus pointer' } else { 'KdInitializeController return value' })
        '    KdInitializeLibrary calls   {0}' -f (($status -shr 24) -band 3)
        '    import / export counts      {0} / {1}   (header expects 33 / 15)' -f ($status -band 0xFF), (($status -shr 8) -band 0xFF)
        '    devmatch / hwmatch          {0} / {1}   (1/1: Adapter is KDNET_SHARED_DATA)' -f (Bit 16), (Bit 17)
        '    memory block contiguous     {0}' -f (Bit 18)
        '    PCI config read = vmxnet3   {0}' -f (Bit 19)
        '    VRRS register read OK       {0}' -f (Bit 20)
        '    KdNetErrorString slot       {0}' -f (Bit 21)
        '    KdNetErrorStatus slot       {0}' -f (Bit 22)
        '    KdNetDbgPrintf slot         {0}' -f (Bit 23)
    } elseif (($status -shr 16) -eq 0xE15A -and ($status -band 0xFFFF) -eq 1) {
        '  Probe v1 "report done" code (carries no data)'
    } elseif (($status -shr 16) -eq 0xE15B) {
        '  Probe rejected the tables: imports {0}, exports {1}' -f (($status -shr 8) -band 0xFF), ($status -band 0xFF)
    }
}

if ($PSBoundParameters.ContainsKey('DecodeStatus')) {
    Format-KdnetStatus $DecodeStatus
    exit
}

$ErrorActionPreference = 'Continue'
$module = Join-Path $env:SystemRoot 'System32\kd_02_15ad.dll'

function Section($title) {
    ''
    "=== $title"
}

Section 'Windows'
$cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
"Build:        $($cv.CurrentBuild).$($cv.UBR) ($($cv.DisplayVersion))"
"Architecture: $env:PROCESSOR_ARCHITECTURE"

Section 'Options the loader passed to the kernel (SystemStartOptions)'
# DEBUG here means winload set up a kernel debug transport for this boot.
(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control').SystemStartOptions -replace 'ENCRYPTION_KEY=\S+', 'ENCRYPTION_KEY=<hidden>'

Section 'Secure Boot (must be False for test-signed modules)'
try { Confirm-SecureBootUEFI } catch { "Unknown: $($_.Exception.Message)" }

Section 'Boot entry in use (bcdedit /enum {current})'
bcdedit /enum '{current}' | Select-String -Pattern 'identifier|path|testsigning|nointegritychecks|debug|bootdebug|hypervisor|loadoptions|kernel'

Section 'Debug settings (bcdedit /dbgsettings, key hidden)'
bcdedit /dbgsettings | ForEach-Object { if ($_ -match '^\s*key\s') { 'key                     <hidden>' } else { $_ } }

Section 'Module file'
if (Test-Path $module) {
    $bytes = [IO.File]::ReadAllBytes($module)
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    $machine = [BitConverter]::ToUInt16($bytes, $pe + 4)
    $arch = switch ($machine) { 0xAA64 { 'ARM64' } 0x8664 { 'x64' } default { 'other' } }
    "Path:      $module"
    "Size:      $($bytes.Length) bytes"
    $version = (Get-Item $module).VersionInfo
    "Variant:   $(if ($version.FileDescription) { $version.FileDescription } else { '(no version resource: built before variants were labeled)' })"
    "Modified:  $((Get-Item $module).LastWriteTime)"
    "Machine:   0x{0:X4} ({1})" -f $machine, $arch
    $sig = Get-AuthenticodeSignature $module
    "Signature: $($sig.Status)"
    "Signer:    $(if ($sig.SignerCertificate) { $sig.SignerCertificate.Subject } else { '(none)' })"
    if ($sig.SignerCertificate) {
        "Issuer:    $($sig.SignerCertificate.Issuer)"
        "Usages:    " + (($sig.SignerCertificate.EnhancedKeyUsageList | ForEach-Object { "$($_.FriendlyName) ($($_.ObjectId))" }) -join ', ')
    }
} else {
    "Not found: $module"
}

Section 'vmxnet3 devices (PCI VEN_15AD DEV_07B0)'
$devices = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like 'PCI\VEN_15AD&DEV_07B0*' }
if (-not $devices) { 'None present' }
foreach ($d in $devices) {
    $location = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_LocationInfo' -ErrorAction SilentlyContinue).Data
    "$($d.FriendlyName) | status $($d.Status) | $location"
}

Section 'Kernel debug network adapter (present only when KDNET is running)'
$kdnic = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.FriendlyName -like '*Kernel Debug Network Adapter*' }
if ($kdnic) { $kdnic | ForEach-Object { "$($_.FriendlyName) | status $($_.Status)" } } else { 'Not present' }

Section 'Loaded kernel debug transport modules'
# When network debugging works, kdnet.dll and the extensibility module
# (kd_02_15ad.dll, under its own name) are listed. kd.dll on its own means the
# boot loader fell back to the local-only kernel debugger, for example because
# it refused to load the extensibility module.
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class KernelModules {
    [DllImport("ntdll.dll")]
    static extern int NtQuerySystemInformation(int infoClass, IntPtr buffer, int length, out int returnLength);

    // RTL_PROCESS_MODULE_INFORMATION on 64-bit Windows: 296 bytes, entries start at offset 8.
    const int SystemModuleInformation = 11;
    const int EntrySize = 296;
    const int SizeOffset = 24;
    const int FileNameOffsetOffset = 38;
    const int PathOffset = 40;

    public static string[] List() {
        int length = 1 << 20;
        IntPtr buffer = Marshal.AllocHGlobal(length);
        try {
            int needed;
            int status = NtQuerySystemInformation(SystemModuleInformation, buffer, length, out needed);
            if (status != 0) {
                return new[] { string.Format("NtQuerySystemInformation failed: 0x{0:X8}", status) };
            }

            int count = Marshal.ReadInt32(buffer);
            var result = new List<string>();
            for (int i = 0; i < count; i++) {
                IntPtr entry = IntPtr.Add(buffer, 8 + i * EntrySize);
                uint size = (uint)Marshal.ReadInt32(entry, SizeOffset);
                byte[] path = new byte[256];
                Marshal.Copy(IntPtr.Add(entry, PathOffset), path, 0, path.Length);
                int end = Array.IndexOf(path, (byte)0);
                string fullPath = Encoding.ASCII.GetString(path, 0, end < 0 ? path.Length : end);
                result.Add(string.Format("0x{0:X8}|{1}", size, fullPath));
            }
            return result.ToArray();
        } finally {
            Marshal.FreeHGlobal(buffer);
        }
    }
}
'@
$modules = [KernelModules]::List()
$kdModules = $modules | Where-Object { ($_ -split '\|', 2)[1] -match '\\kd[^\\]*\.dll$' }
if ($kdModules) {
    $kdModules | ForEach-Object { $s, $p = $_ -split '\|', 2; "image size $s  $p" }
} else {
    "No kd*.dll kernel modules found ($($modules.Count) modules listed)"
}
foreach ($name in 'kd_02_15ad.dll', 'kdstub.dll', 'kdnet.dll', 'kd.dll') {
    $file = Join-Path $env:SystemRoot "System32\$name"
    if (Test-Path $file) {
        $b = [IO.File]::ReadAllBytes($file)
        $pe = [BitConverter]::ToInt32($b, 0x3C)
        "On disk: {0,-15} SizeOfImage 0x{1:X8}  {2}" -f $name, [BitConverter]::ToUInt32($b, $pe + 24 + 56), (Get-Item $file).VersionInfo.FileDescription
    }
}

Section 'Code Integrity events mentioning kd* modules (last 7 days)'
$ciEvents = Get-WinEvent -LogName 'Microsoft-Windows-CodeIntegrity/Operational' -ErrorAction SilentlyContinue |
    Where-Object { $_.TimeCreated -gt (Get-Date).AddDays(-7) -and $_.Message -match '\\kd[^\\]*\.dll' }
if ($ciEvents) {
    $ciEvents | Select-Object -First 10 | ForEach-Object { "$($_.TimeCreated) id $($_.Id): $($_.Message)" }
} else {
    'None'
}

Section 'KDNET status values anywhere under HKLM\SYSTEM\CurrentControlSet'
$found = reg query HKLM\SYSTEM\CurrentControlSet /s /f KdInit /v 2>$null | Where-Object { $_ -match '\S' -and $_ -notmatch '^End of search' }
if ($found) { $found } else { 'No values named KdInit* found' }
$probeText = reg query HKLM\SYSTEM\CurrentControlSet /s /f 'kd_02_15ad probe' /d 2>$null | Where-Object { $_ -match '\S' -and $_ -notmatch '^End of search' }
if ($probeText) { $probeText } else { 'No registry data containing "kd_02_15ad probe" found' }

Section 'Debug ports described by firmware (ACPI DBG2 table)'
# On ARM64, Windows can use a serial port for kernel or boot debugging only if
# the firmware lists it in DBG2. Port types: 0x8000 serial, 0x8001 1394,
# 0x8002 USB, 0x8003 net.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class FirmwareTables {
    [DllImport("kernel32.dll")]
    static extern uint GetSystemFirmwareTable(uint provider, uint id, byte[] buffer, uint size);

    const uint ACPI = 0x41435049;   // 'ACPI'
    const uint DBG2 = 0x32474244;   // 'DBG2' as stored in the table header

    [DllImport("kernel32.dll")]
    static extern uint EnumSystemFirmwareTables(uint provider, byte[] buffer, uint size);

    public static string AcpiTables() {
        uint size = EnumSystemFirmwareTables(ACPI, null, 0);
        byte[] b = new byte[size];
        EnumSystemFirmwareTables(ACPI, b, size);
        var names = new System.Text.StringBuilder();
        for (int i = 0; i + 4 <= b.Length; i += 4) {
            names.Append(System.Text.Encoding.ASCII.GetString(b, i, 4)).Append(' ');
        }
        return names.ToString().Trim();
    }

    public static string[] Dbg2Devices() {
        uint size = GetSystemFirmwareTable(ACPI, DBG2, null, 0);
        if (size == 0) {
            return new[] { "No DBG2 table" };
        }

        byte[] t = new byte[size];
        GetSystemFirmwareTable(ACPI, DBG2, t, size);
        uint offset = BitConverter.ToUInt32(t, 36);
        uint count = BitConverter.ToUInt32(t, 40);
        var result = new System.Collections.Generic.List<string>();
        for (uint i = 0; i < count && offset + 22 <= size; i++) {
            ushort length = BitConverter.ToUInt16(t, (int)offset + 1);
            ushort nsLength = BitConverter.ToUInt16(t, (int)offset + 4);
            ushort nsOffset = BitConverter.ToUInt16(t, (int)offset + 6);
            ushort portType = BitConverter.ToUInt16(t, (int)offset + 12);
            ushort portSubtype = BitConverter.ToUInt16(t, (int)offset + 14);
            string ns = (nsLength > 1 && offset + nsOffset + nsLength <= size)
                ? System.Text.Encoding.ASCII.GetString(t, (int)(offset + nsOffset), nsLength - 1)
                : "";
            result.Add(string.Format("PortType 0x{0:X4} PortSubtype 0x{1:X4} {2}", portType, portSubtype, ns));
            if (length == 0) break;
            offset += length;
        }
        return result.ToArray();
    }
}
'@
"ACPI tables: $([FirmwareTables]::AcpiTables())"
[FirmwareTables]::Dbg2Devices()

Section 'KDNET status (HKLM\SYSTEM\CurrentControlSet\Services\kdnet)'
$kdnetKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\kdnet'
if (Test-Path $kdnetKey) {
    $values = Get-ItemProperty $kdnetKey
    if ($null -ne $values.KdInitStatus) {
        Format-KdnetStatus ([long]$values.KdInitStatus)
    }
    # KDNET stores the module's KdNetErrorString text here.
    "KdInitResultString: $($values.KdInitResultString)"
    $values | Format-List | Out-String
} else {
    'Key not present'
}

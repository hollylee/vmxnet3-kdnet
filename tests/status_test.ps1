$ErrorActionPreference = 'Stop'
$checker = Join-Path $PSScriptRoot '..\tools\check-target.ps1'
$cases = @(
    @(0, 'KDNET reports initialization success'),
    @(0xE2110FFF, 'failure: stage 17 \(TX completion\), detail 0x0FFF'),
    @(0xE2120001, 'failure: stage 18 \(RX completion\)'),
    @(0xE2130002, 'failure: stage 19 \(TX timeout\)'),
    @(0xE214000E, 'failure: stage 20 \(queue recovery\)'),
    @([long]3800049424, 'observed link speed\s+10000 Mbps'),
    @([long]-494917872, 'observed link speed\s+10000 Mbps'),
    @(0xE2460000, 'checkpoint passed: PCI configuration'),
    @(0xE2480000, 'checkpoint passed: reset and interrupt masks'),
    @(0xE2490000, 'checkpoint passed: DMA layout'),
    @(0xE24A0000, 'checkpoint passed: cycle counter/stall'),
    @(0xE24C0000, 'checkpoint passed: device activation'),
    @(0xE20C0007, 'failure: stage 12 \(device activation\), detail 0x0007'),
    @(0xE20F0080, 'original stage\s+0x80'),
    @(0xE6FF0F21, 'import / export counts\s+33 / 15'),
    @(0xF6FF0F21, 'set via\s+KdNetErrorStatus pointer'),
    @(0xE15A0001, 'Probe v1'),
    @(0xE15B210F, 'imports 33, exports 15'),
    @(0xC0000001, 'KdInitStatus:\s+0xC0000001')
)
foreach ($case in $cases) {
    $output = (& $checker -DecodeStatus $case[0]) -join "`n"
    if ($output -notmatch $case[1]) { throw "Unexpected decode for $($case[0]): $output" }
    if ($output -match '=== Windows') { throw 'Offline decoder inspected the target' }
}
'Status decoder: {0} cases passed.' -f $cases.Count

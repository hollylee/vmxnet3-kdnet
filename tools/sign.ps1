# Test-signs every built kd_*.dll under out\.
#
#   powershell -ExecutionPolicy Bypass -File tools\sign.ps1 [-Profile Basic|Windows]
#
# Profiles:
#   Basic    Self-signed leaf certificate with the Code Signing usage only.
#            This is what earlier builds used; winload on Windows 11 ARM64
#            25H2 refused to load a module signed this way.
#   Windows  A test root CA issuing a leaf with Code Signing plus Windows
#            System Component Verification (1.3.6.1.4.1.311.10.3.6), the same
#            shape as the Microsoft certificates on the inbox kd_*.dll modules.
#
# Certificates are created on first use in CurrentUser\My and reused after
# that. The public certificates are exported to out\ so they can be inspected
# or installed on a target. On the tested ARM64 25H2 build, test signing and
# Secure Boot off are insufficient: Startup Settings option 7 is also needed.

param(
    [ValidateSet('Basic', 'Windows')]
    [string] $Profile = 'Basic',

    [string] $WdkVersion = '10.0.26100.0',

    [ValidateSet('All', 'fault-once')]
    [string] $Variant = 'All'
)

$ErrorActionPreference = 'Stop'

# A fresh -NoProfile session may not load the Cert: provider merely from a
# provider-path lookup. Load it before searching for an existing certificate.
# Use this engine's module, not a PowerShell 7 module inherited through PSModulePath.
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1') -ErrorAction Stop
Import-Module (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\Modules\PKI\PKI.psd1') -ErrorAction Stop

$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'out'
$signtool = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin\$WdkVersion\x64\signtool.exe"

$codeSigning = '1.3.6.1.5.5.7.3.3'
$windowsComponent = '1.3.6.1.4.1.311.10.3.6'

if (-not (Test-Path $signtool)) { throw "signtool not found at $signtool" }
if (-not (Test-Path $out)) { throw 'Nothing to sign. Run build.cmd first.' }

function Get-TestCertificate([string] $Subject) {
    Get-ChildItem Cert:\CurrentUser\My |
        Where-Object { $_.Subject -eq $Subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date) } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1
}

function Export-Public($Certificate, [string] $FileName) {
    Export-Certificate -Cert $Certificate -FilePath (Join-Path $out $FileName) | Out-Null
}

switch ($Profile) {
    'Basic' {
        $subject = 'CN=vmxnet3-kdnet test'
        $signer = Get-TestCertificate $subject
        if (-not $signer) {
            $signer = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
                -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 2048 `
                -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
        }
        Export-Public $signer 'vmxnet3-kdnet-test.cer'
    }

    'Windows' {
        $rootSubject = 'CN=vmxnet3-kdnet test root'
        $leafSubject = 'CN=vmxnet3-kdnet test (Windows component)'

        $ca = Get-TestCertificate $rootSubject
        if (-not $ca) {
            $ca = New-SelfSignedCertificate -Type Custom -Subject $rootSubject `
                -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 2048 `
                -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10) `
                -KeyUsage CertSign, CRLSign, DigitalSignature -KeyExportPolicy NonExportable `
                -TextExtension @('2.5.29.19={critical}{text}ca=1&pathlength=0')
        }

        $signer = Get-TestCertificate $leafSubject
        if (-not $signer) {
            $signer = New-SelfSignedCertificate -Type Custom -Subject $leafSubject -Signer $ca `
                -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 2048 `
                -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5) `
                -KeyUsage DigitalSignature -KeyExportPolicy NonExportable `
                -TextExtension @("2.5.29.37={text}$windowsComponent,$codeSigning", '2.5.29.19={text}ca=0')
        }

        Export-Public $ca 'vmxnet3-kdnet-test-root.cer'
        Export-Public $signer 'vmxnet3-kdnet-test-windows.cer'
    }
}

"Signing with: $($signer.Subject) (thumbprint $($signer.Thumbprint))"
"Issued by:    $($signer.Issuer)"
'Usages:       ' + (($signer.EnhancedKeyUsageList | ForEach-Object { $_.FriendlyName }) -join ', ')

# /ac gives signtool the test root for building the chain. As on the inbox
# modules, the root itself is not embedded in the signature, only the leaf.
$chainArgs = @()
if ($Profile -eq 'Windows') {
    $caFile = Join-Path $out 'vmxnet3-kdnet-test-root.cer'
    $chainArgs = @('/ac', $caFile)
}

$files = Get-ChildItem $out -Recurse -Filter 'kd_*.dll' | Where-Object { $_.FullName -notlike '*\obj\*' }
if ($Variant -ne 'All') { $files = $files | Where-Object { $_.Directory.Name -eq $Variant } }
if (-not $files) { throw "No DLLs found for variant $Variant" }
foreach ($file in $files) {
    & $signtool sign /q /fd SHA256 /sha1 $signer.Thumbprint @chainArgs $file.FullName
    if ($LASTEXITCODE -ne 0) { throw "signtool failed for $($file.FullName)" }
    "signed $($file.FullName.Substring($root.Length + 1))"
}

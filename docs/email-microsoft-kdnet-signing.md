# Draft email: signing third-party KDNET extensibility modules

**To:** kdnet@microsoft.com
**Subject:** Signing and loading a third-party KDNET extensibility module (vmxnet3, ARM64)

---

Hello,

I'm developing a KDNET extensibility module for VMware's vmxnet3 virtual NIC
(PCI 15AD:07B0), following the "Develop KDNET Transport Extensibility
Modules" guide on Microsoft Learn. Windows 11 x64 24H2 and later include
kd_02_15ad.dll, but Windows 11 on ARM64 does not, so kernel debugging over
the network isn't currently possible in ARM64 VMs on VMware hosts such as
VMware Fusion on Apple Silicon. Those VMs offer no e1000e adapter and no ACPI
DBG2 table, so there is no other debug transport either.

What I have observed on Windows 11 ARM64 25H2 (build 26200.9711) in a VMware
Fusion VM, with test signing on and Secure Boot off:

- A module named kd_02_15ad.dll in System32, signed with a self-signed test
  certificate, is not loaded. winload falls back to kd.dll and never calls
  KdInitializeLibrary.
- The same happens with a test certificate chain that mirrors the inbox
  modules' certificates: a test root issuing a leaf with the Windows System
  Component Verification and Code Signing usages.
- When I boot with "Disable driver signature enforcement" (Startup
  Settings), the module loads, KdInitializeLibrary and KdInitializeController
  are called, and the KdInitializeController status is recorded under
  HKLM\SYSTEM\CurrentControlSet\Services\kdnet.
- A Microsoft-signed inbox module copied under the same file name loads
  normally, so the file name, location and debug settings are not the issue.

My questions:

1. Is there a supported way to load a test-signed KDNET extensibility module
   during development, other than disabling signature enforcement on every
   boot?
2. What signing route is available for a third-party KDNET extensibility
   module that is distributed publicly? For example, attestation signing
   through Partner Center, or including the module in Windows, as with the
   inbox modules from other vendors.
3. Is an ARM64 version of kd_02_15ad.dll planned for Windows 11? If so, my
   module would only be needed for older x64 builds.
4. The guide refers to kdnetextensibility.h, which I could not find in the
   WDK (10.0.26100). Is that header available to module developers, and is
   there guidance on which import/export table counts each Windows release
   uses?

I'm happy to share more detail, or to test builds on ARM64 VMware VMs if
that would help.

Thank you,

[Your name]
[Contact details]

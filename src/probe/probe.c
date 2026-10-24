/*
 * kd_02_15ad.dll probe module.
 *
 * This module never brings up the NIC. It checks how KDNET calls an
 * extensibility module on the running Windows build and reports what it
 * finds, so the ReactOS-derived header can be verified before the real
 * vmxnet3 module depends on it. The report contains:
 *
 *   - the import/export table counts the OS passes
 *   - what the "Adapter" argument points to
 *   - the debug device descriptor (PCI location, BARs, memory block)
 *   - read-only vmxnet3 identification (PCI IDs, VRRS/UVRS, MAC address)
 *   - whether the memory block is physically contiguous
 *
 * KdInitializeController then fails on purpose, so Windows boots without a
 * debugger. The report goes out through every channel available:
 *
 *   1. A summary packed into the failure status (see PROBE_SUMMARY_*). KDNET
 *      saves it as KdInitStatus under
 *      HKLM\SYSTEM\CurrentControlSet\Services\kdnet. On Windows 11 ARM64 25H2
 *      this is the one channel known to work.
 *   2. The full text report through KdNetErrorString, which KDNET saves as
 *      KdInitResultString.
 *   3. KdNetDbgPrintf (visible in a boot debugger).
 *   4. If the tables are too short to use, KdInitializeLibrary returns a
 *      status with the counts encoded in it (PROBE_STATUS_BAD_COUNTS).
 *
 * The probe only reads device registers and PCI config space; it never
 * writes to them.
 */

#include "kdnet_rt.h"

#define PROBE_VERSION 2

/*
 * PROBE_MODE selects a diagnostic variant. The hang variants answer "how far
 * did the boot loader get?" on machines with no boot debugger: if the VM
 * freezes during boot, the hang point was reached. They depend on nothing but
 * the entry point being called, not on the table layout.
 *
 *   0  normal probe (report and fail initialization)
 *   1  hang on entry to KdInitializeLibrary: the loader accepted and called us
 *   2  hang once the table checks in KdInitializeLibrary pass
 */
#ifndef PROBE_MODE
#error PROBE_MODE must be defined by the build (0, 1 or 2)
#endif

#if PROBE_MODE < 0 || PROBE_MODE > 2
#error PROBE_MODE must be 0, 1 or 2
#endif

/* Large enough to span several pages, so the contiguity check means something. */
#define PROBE_CONTEXT_SIZE (64 * 1024)

/*
 * Summary status returned by KdInitializeController (and written through
 * KdNetErrorStatus). The top three bits are 111 (error severity, customer
 * code), so it can't be mistaken for a Windows status. tools\check-target.ps1
 * decodes it.
 *
 *   31-29  111
 *   28     1 = value written through the KdNetErrorStatus pointer,
 *          0 = KdInitializeController's return value
 *   27-26  format version (01). Version 00 is the v1 probe's fixed
 *          0xE15A0001, which carried no data.
 *   25-24  KdInitializeLibrary calls in this image instance (max 3)
 *   23     KdNetDbgPrintf slot present and non-null
 *   22     KdNetErrorStatus slot present and non-null
 *   21     KdNetErrorString slot present and non-null
 *   20     vmxnet3 VRRS register read returned a plausible value
 *   19     PCI config read through the import table returned 15AD:07B0
 *   18     first 64 KB of the memory block is physically contiguous
 *   17     Adapter->Hardware is the memory block (hwmatch)
 *   16     Adapter->Device is the descriptor seen in KdInitializeLibrary (devmatch)
 *   15-8   export FunctionCount (max 255)
 *   7-0    import FunctionCount (max 255)
 */
#define PROBE_SUMMARY_BASE          0xE4000000UL
#define PROBE_SUMMARY_VIA_POINTER   (1UL << 28)
#define PROBE_SUMMARY_CALLS_SHIFT   24
#define PROBE_SUMMARY_DBGPRINT      (1UL << 23)
#define PROBE_SUMMARY_ERRSTATUS     (1UL << 22)
#define PROBE_SUMMARY_ERRSTRING     (1UL << 21)
#define PROBE_SUMMARY_VRRS_OK       (1UL << 20)
#define PROBE_SUMMARY_PCI_OK        (1UL << 19)
#define PROBE_SUMMARY_CONTIG        (1UL << 18)
#define PROBE_SUMMARY_HWMATCH       (1UL << 17)
#define PROBE_SUMMARY_DEVMATCH      (1UL << 16)
#define PROBE_SUMMARY_EXPORTS_SHIFT 8

/* Low 16 bits: (min(imports, 255) << 8) | min(exports, 255). */
#define PROBE_STATUS_BAD_COUNTS ((NTSTATUS)0xE15B0000L)

/* The ten packet-interface exports documented by Microsoft, in table order. */
#define PROBE_DOCUMENTED_EXPORTS 10

#define VMXNET3_PCI_VENDOR_ID 0x15AD
#define VMXNET3_PCI_DEVICE_ID 0x07B0

/* BAR1 ("VD") register offsets, from the BSD-licensed vmxnet3 interface definitions. */
#define VMXNET3_BAR1_INDEX 1
#define VMXNET3_REG_VRRS   0x00
#define VMXNET3_REG_UVRS   0x08
#define VMXNET3_REG_MACL   0x28
#define VMXNET3_REG_MACH   0x30

#define PCI_COMMAND_OFFSET        0x04
#define PCI_COMMAND_MEMORY_ENABLE 0x0002

static ULONG ProbeLibraryCalls;
static ULONG ProbeExportCount;
static ULONG ProbeMemoryLengthIn;
static PDEBUG_DEVICE_DESCRIPTOR ProbeLibraryDevice;

/* PROBE_SUMMARY_* flag bits gathered while building the report. */
static ULONG ProbeSummaryFlags;

#if PROBE_MODE != 0
static
DECLSPEC_NOINLINE
VOID
ProbeHang (
    VOID
    )
{
    volatile ULONG Spin = 1;

    while (Spin != 0) {
        YieldProcessor();
    }
}
#endif

static
ULONG
ProbeClampByte (
    _In_ ULONG Value
    )
{
    return (Value > 0xFF) ? 0xFF : Value;
}

static
VOID
ProbePutField (
    _In_z_ PCSTR Name,
    _In_ ULONG64 Value,
    _In_ ULONG Digits
    )
{
    KdxPutStr(" ");
    KdxPutStr(Name);
    KdxPutStr("=");
    KdxPutHex(Value, Digits);
}

static
VOID
ProbePutPointer (
    _In_z_ PCSTR Name,
    _In_opt_ const VOID *Pointer
    )
{
    ProbePutField(Name, (ULONG64)(ULONG_PTR)Pointer, 16);
}

static
VOID
ProbeReportImports (
    VOID
    )
{
    KdxPutStr(" imp=");
    KdxPutDec(KdxImportCount);
    KdxPutStr(" exp=");
    KdxPutDec(ProbeExportCount);
    KdxPutStr(" libcalls=");
    KdxPutDec(ProbeLibraryCalls);

    if (KdxHasImport(FIELD_OFFSET(KDNET_EXTENSIBILITY_IMPORTS, ExecutionEnvironment))) {
        ProbePutField("env", KdxImports.ExecutionEnvironment, 8);
    }

    if (KDX_HAS_IMPORT(KdNetHardwareID)) {
        ProbePutField("hwid", *KdxImports.KdNetHardwareID, 8);
    }

    if (KDX_HAS_IMPORT(ReadCycleCounter)) {
        ULONG64 Frequency = 0;

        KdxImports.ReadCycleCounter(&Frequency);
        KdxPutStr(" cyclehz=");
        KdxPutDec(Frequency);
    }
}

static
VOID
ProbeReportSharedData (
    _In_ PKDNET_SHARED_DATA KdNet,
    _In_opt_ PDEBUG_DEVICE_DESCRIPTOR Device
    )
{
    ProbePutPointer("adapter", KdNet);
    ProbePutPointer("hw", KdNet->Hardware);
    ProbePutPointer("kddev", KdNet->Device);
    ProbePutPointer("libdev", ProbeLibraryDevice);
    ProbePutField("devmatch", KdNet->Device == ProbeLibraryDevice, 1);
    if (KdNet->Device != NULL && KdNet->Device == ProbeLibraryDevice) {
        ProbeSummaryFlags |= PROBE_SUMMARY_DEVMATCH;
    }

    if (Device != NULL) {
        ProbePutField("hwmatch", KdNet->Hardware == Device->Memory.VirtualAddress, 1);
        if (KdNet->Hardware != NULL && KdNet->Hardware == Device->Memory.VirtualAddress) {
            ProbeSummaryFlags |= PROBE_SUMMARY_HWMATCH;
        }
    }
    ProbePutPointer("macptr", KdNet->TargetMacAddress);
    ProbePutPointer("linkptr", KdNet->LinkState);
    ProbePutField("kdflags", KdNet->Flags, 8);
}

static
VOID
ProbeReportDevice (
    _In_ PDEBUG_DEVICE_DESCRIPTOR Device
    )
{
    ULONG Index;

    ProbePutField("bus", Device->Bus, 2);
    ProbePutField("slot", Device->Slot, 8);
    ProbePutField("seg", Device->Segment, 4);
    ProbePutField("ven", Device->VendorID, 4);
    ProbePutField("did", Device->DeviceID, 4);
    ProbePutField("class", Device->BaseClass, 2);
    ProbePutField("ddflags", Device->Flags, 2);
    ProbePutField("init", Device->Initialized, 1);
    ProbePutField("conf", Device->Configured, 1);

    for (Index = 0; Index < MAXIMUM_DEBUG_BARS; Index += 1) {
        PDEBUG_DEVICE_ADDRESS Bar = &Device->BaseAddress[Index];

        if (!Bar->Valid) {
            continue;
        }

        KdxPutStr(" bar");
        KdxPutDec(Index);
        KdxPutStr("=t");
        KdxPutHex(Bar->Type, 2);
        KdxPutStr(":");
        KdxPutHex((ULONG64)(ULONG_PTR)Bar->TranslatedAddress, 16);
        KdxPutStr("/");
        KdxPutHex(Bar->Length, 8);
    }

    ProbePutField("memstart", (ULONG64)Device->Memory.Start.QuadPart, 16);
    ProbePutField("memmax", (ULONG64)Device->Memory.MaxEnd.QuadPart, 16);
    ProbePutPointer("memva", Device->Memory.VirtualAddress);
    ProbePutField("memlen", Device->Memory.Length, 8);
    ProbePutField("memlenin", ProbeMemoryLengthIn, 8);
    ProbePutField("cached", Device->Memory.Cached, 1);
    ProbePutField("aligned", Device->Memory.Aligned, 1);
}

/*
 * Checks that every page of the block we asked for maps to consecutive
 * physical pages. vmxnet3 rings need one contiguous physical base each.
 */
static
VOID
ProbeReportContiguity (
    _In_ PDEBUG_DEVICE_DESCRIPTOR Device
    )
{
    PUCHAR Base = (PUCHAR)Device->Memory.VirtualAddress;
    PHYSICAL_ADDRESS First;
    ULONG Offset;
    BOOLEAN Contiguous = TRUE;

    if (Base == NULL || !KDX_HAS_IMPORT(GetPhysicalAddress)) {
        KdxPutStr(" contig=unknown");
        return;
    }

    First = KdxImports.GetPhysicalAddress(Base);
    for (Offset = PAGE_SIZE; Offset < PROBE_CONTEXT_SIZE; Offset += PAGE_SIZE) {
        PHYSICAL_ADDRESS Page = KdxImports.GetPhysicalAddress(Base + Offset);

        if (Page.QuadPart != First.QuadPart + Offset) {
            Contiguous = FALSE;
            break;
        }
    }

    ProbePutField("pa", (ULONG64)First.QuadPart, 16);
    ProbePutField("contig", Contiguous, 1);
    if (Contiguous && First.QuadPart != 0) {
        ProbeSummaryFlags |= PROBE_SUMMARY_CONTIG;
    }
}

static
VOID
ProbeReportVmxnet3 (
    _In_ PDEBUG_DEVICE_DESCRIPTOR Device
    )
{
    PDEBUG_DEVICE_ADDRESS Bar = &Device->BaseAddress[VMXNET3_BAR1_INDEX];
    ULONG ConfigId = 0;
    USHORT Command = 0;
    ULONG Vrrs;
    ULONG MacLow;
    ULONG MacHigh;

    if (!KDX_HAS_IMPORT(GetPciDataByOffset)) {
        KdxPutStr(" cfg=noimport");
        return;
    }

    KdxImports.GetPciDataByOffset(Device->Bus, Device->Slot, &ConfigId, 0, sizeof(ConfigId));
    KdxImports.GetPciDataByOffset(Device->Bus, Device->Slot, &Command,
                                  PCI_COMMAND_OFFSET, sizeof(Command));

    ProbePutField("cfgid", ConfigId, 8);
    ProbePutField("cmd", Command, 4);

    if (ConfigId != ((VMXNET3_PCI_DEVICE_ID << 16) | VMXNET3_PCI_VENDOR_ID)) {
        KdxPutStr(" vmx=notvmxnet3");
        return;
    }

    ProbeSummaryFlags |= PROBE_SUMMARY_PCI_OK;

    if ((Command & PCI_COMMAND_MEMORY_ENABLE) == 0) {
        KdxPutStr(" vmx=memdecodeoff");
        return;
    }

    if (!Bar->Valid || Bar->TranslatedAddress == NULL || !KDX_HAS_IMPORT(ReadRegisterULong)) {
        KdxPutStr(" vmx=nobar1");
        return;
    }

    /* VRRS is a bitmask of supported revisions: non-zero, and never all ones. */
    Vrrs = KdxImports.ReadRegisterULong((PULONG)(Bar->TranslatedAddress + VMXNET3_REG_VRRS));
    ProbePutField("vrrs", Vrrs, 8);
    if (Vrrs != 0 && Vrrs != 0xFFFFFFFF) {
        ProbeSummaryFlags |= PROBE_SUMMARY_VRRS_OK;
    }

    ProbePutField("uvrs", KdxImports.ReadRegisterULong((PULONG)(Bar->TranslatedAddress + VMXNET3_REG_UVRS)), 8);

    MacLow = KdxImports.ReadRegisterULong((PULONG)(Bar->TranslatedAddress + VMXNET3_REG_MACL));
    MacHigh = KdxImports.ReadRegisterULong((PULONG)(Bar->TranslatedAddress + VMXNET3_REG_MACH));
    KdxPutStr(" mac=");
    KdxPutHex(MacLow & 0xFF, 2);
    KdxPutHex((MacLow >> 8) & 0xFF, 2);
    KdxPutHex((MacLow >> 16) & 0xFF, 2);
    KdxPutHex((MacLow >> 24) & 0xFF, 2);
    KdxPutHex(MacHigh & 0xFF, 2);
    KdxPutHex((MacHigh >> 8) & 0xFF, 2);
}

static
ULONG
ProbeSummary (
    VOID
    )
{
    ULONG Calls = (ProbeLibraryCalls > 3) ? 3 : ProbeLibraryCalls;
    ULONG Summary = PROBE_SUMMARY_BASE | ProbeSummaryFlags;

    if (KDX_HAS_IMPORT(KdNetDbgPrintf)) {
        Summary |= PROBE_SUMMARY_DBGPRINT;
    }

    if (KDX_HAS_IMPORT(KdNetErrorStatus)) {
        Summary |= PROBE_SUMMARY_ERRSTATUS;
    }

    if (KDX_HAS_IMPORT(KdNetErrorString)) {
        Summary |= PROBE_SUMMARY_ERRSTRING;
    }

    Summary |= Calls << PROBE_SUMMARY_CALLS_SHIFT;
    Summary |= ProbeClampByte(ProbeExportCount) << PROBE_SUMMARY_EXPORTS_SHIFT;
    Summary |= ProbeClampByte(KdxImportCount);
    return Summary;
}

/*
 * Publishes the report and returns the status KdInitializeController should
 * return. The pointer copy of the summary has PROBE_SUMMARY_VIA_POINTER set,
 * so KdInitStatus shows which of the two KDNET recorded.
 */
static
NTSTATUS
ProbePublishReport (
    VOID
    )
{
    ULONG Summary = ProbeSummary();

    KdxPutStr(" summary=");
    KdxPutHex(Summary, 8);

    if (KDX_HAS_IMPORT(KdNetErrorString)) {
        *KdxImports.KdNetErrorString = KdxReport.Wide;
    }

    if (KDX_HAS_IMPORT(KdNetErrorStatus)) {
        *KdxImports.KdNetErrorStatus = (NTSTATUS)(Summary | PROBE_SUMMARY_VIA_POINTER);
    }

    KdxLogReport();
    return (NTSTATUS)Summary;
}

static
NTSTATUS
NTAPI
ProbeInitializeController (
    _In_ PKDNET_SHARED_DATA KdNet
    )
{
    PDEBUG_DEVICE_DESCRIPTOR Device;

    ProbeSummaryFlags = 0;
    KdxReportReset();
    KdxPutStr("kd_02_15ad probe v");
    KdxPutDec(PROBE_VERSION);
    ProbeReportImports();

    /*
     * The ReactOS header says the argument is KDNET_SHARED_DATA; Microsoft's
     * docs only call it "Adapter". Prefer the descriptor saved in
     * KdInitializeLibrary unless the shared data's copy matches it.
     */
    Device = ProbeLibraryDevice;
    if (KdNet != NULL) {
        ProbeReportSharedData(KdNet, (Device != NULL) ? Device : KdNet->Device);
    } else {
        KdxPutStr(" adapter=null");
    }

    if (Device == NULL) {
        KdxPutStr(" dev=null");
    } else {
        ProbeReportDevice(Device);
        ProbeReportContiguity(Device);
        ProbeReportVmxnet3(Device);
    }

    return ProbePublishReport();
}

static
VOID
NTAPI
ProbeShutdownController (
    _In_ PVOID Adapter
    )
{
    UNREFERENCED_PARAMETER(Adapter);
}

static
VOID
NTAPI
ProbeSetHibernateRange (
    VOID
    )
{
}

static
NTSTATUS
NTAPI
ProbeGetRxPacket (
    _In_ PVOID Adapter,
    _Out_ PULONG Handle,
    _Out_ PVOID *Packet,
    _Out_ PULONG Length
    )
{
    UNREFERENCED_PARAMETER(Adapter);

    *Handle = 0;
    *Packet = NULL;
    *Length = 0;
    return STATUS_IO_TIMEOUT;
}

static
VOID
NTAPI
ProbeReleaseRxPacket (
    _In_ PVOID Adapter,
    _In_ ULONG Handle
    )
{
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Handle);
}

static
NTSTATUS
NTAPI
ProbeGetTxPacket (
    _In_ PVOID Adapter,
    _Out_ PULONG Handle
    )
{
    UNREFERENCED_PARAMETER(Adapter);

    *Handle = 0;
    return STATUS_IO_TIMEOUT;
}

static
NTSTATUS
NTAPI
ProbeSendTxPacket (
    _In_ PVOID Adapter,
    _In_ ULONG Handle,
    _In_ ULONG Length
    )
{
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Handle);
    UNREFERENCED_PARAMETER(Length);
    return STATUS_IO_TIMEOUT;
}

static
PVOID
NTAPI
ProbeGetPacketAddress (
    _In_ PVOID Adapter,
    _In_ ULONG Handle
    )
{
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Handle);
    return NULL;
}

static
ULONG
NTAPI
ProbeGetPacketLength (
    _In_ PVOID Adapter,
    _In_ ULONG Handle
    )
{
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Handle);
    return 0;
}

static
ULONG
NTAPI
ProbeGetHardwareContextSize (
    _In_ struct _DEBUG_DEVICE_DESCRIPTOR *Device
    )
{
    UNREFERENCED_PARAMETER(Device);
    return PROBE_CONTEXT_SIZE;
}

/*
 * Called twice during boot: by winload (to size the memory block) and by
 * KDNET when the kernel initializes debugging.
 *
 * Unlike a production module, the probe accepts import/export counts that
 * differ from the header's, because reporting them is the point. It only
 * writes the ten documented export slots, so a shorter OS table is never
 * overrun.
 */
NTSTATUS
NTAPI
KdInitializeLibrary (
    _In_ PKDNET_EXTENSIBILITY_IMPORTS ImportTable,
    _In_opt_ PCHAR LoaderOptions,
    _Inout_ struct _DEBUG_DEVICE_DESCRIPTOR *Device
    )
{
    PKDNET_EXTENSIBILITY_EXPORTS Exports;

    UNREFERENCED_PARAMETER(LoaderOptions);

    __security_init_cookie();

#if PROBE_MODE == 1
    ProbeHang();
#endif

    ProbeLibraryCalls += 1;
    if (ImportTable == NULL || Device == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    KdxCaptureImports(ImportTable);
    Exports = ImportTable->Exports;
    ProbeExportCount = (Exports != NULL) ? Exports->FunctionCount : 0;

    if (Exports == NULL ||
        ProbeExportCount < PROBE_DOCUMENTED_EXPORTS ||
        !KDX_HAS_IMPORT(GetPhysicalAddress)) {

        return (NTSTATUS)((ULONG)PROBE_STATUS_BAD_COUNTS |
                          (ProbeClampByte(KdxImportCount) << 8) |
                          ProbeClampByte(ProbeExportCount));
    }

#if PROBE_MODE == 2
    ProbeHang();
#endif

    Exports->KdInitializeController = ProbeInitializeController;
    Exports->KdShutdownController = ProbeShutdownController;
    Exports->KdSetHibernateRange = ProbeSetHibernateRange;
    Exports->KdGetRxPacket = ProbeGetRxPacket;
    Exports->KdReleaseRxPacket = ProbeReleaseRxPacket;
    Exports->KdGetTxPacket = ProbeGetTxPacket;
    Exports->KdSendTxPacket = ProbeSendTxPacket;
    Exports->KdGetPacketAddress = ProbeGetPacketAddress;
    Exports->KdGetPacketLength = ProbeGetPacketLength;
    Exports->KdGetHardwareContextSize = ProbeGetHardwareContextSize;

    ProbeMemoryLengthIn = Device->Memory.Length;
    Device->Memory.Length = ProbeGetHardwareContextSize(Device);
    ProbeLibraryDevice = Device;
    return STATUS_SUCCESS;
}

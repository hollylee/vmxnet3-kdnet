/*
 * Minimal runtime for KDNET extensibility modules.
 *
 * KDNET modules must have exactly one export (KdInitializeLibrary) and no
 * imports. Everything the module may call arrives through the import table
 * passed to KdInitializeLibrary, so this file keeps a private copy of it and
 * provides the few CRT routines the compiler may emit calls to.
 */

#pragma once

#include <ntddk.h>
#include "kdnetextensibility.h"

/*
 * The import table is a ULONG FunctionCount followed by pointer-sized slots
 * (ExecutionEnvironment is a ULONG, but it is padded to a full slot on 64-bit
 * targets). FunctionCount counts the slots. These asserts pin that assumption
 * to the ReactOS layout so a header change cannot silently break it.
 */
#define KDX_IMPORT_SLOT_BASE FIELD_OFFSET(KDNET_EXTENSIBILITY_IMPORTS, Exports)

C_ASSERT(sizeof(PVOID) == 8);
C_ASSERT(FIELD_OFFSET(KDNET_EXTENSIBILITY_IMPORTS, KdNetHardwareID) ==
         KDX_IMPORT_SLOT_BASE + (KDNET_EXT_IMPORTS - 1) * sizeof(PVOID));
C_ASSERT(sizeof(KDNET_EXTENSIBILITY_IMPORTS) ==
         KDX_IMPORT_SLOT_BASE + KDNET_EXT_IMPORTS * sizeof(PVOID));

/*
 * From bufferoverflowfastfailk.lib. KdInitializeLibrary must call it first,
 * before any function that has a security cookie check runs.
 */
void __cdecl __security_init_cookie (void);

extern KDNET_EXTENSIBILITY_IMPORTS KdxImports;

/* FunctionCount exactly as passed by the OS (may differ from KDNET_EXT_IMPORTS). */
extern ULONG KdxImportCount;

/*
 * Copies the slots the OS provided (at most KDNET_EXT_IMPORTS) into KdxImports
 * and zeroes the rest.
 */
VOID
KdxCaptureImports (
    _In_ PKDNET_EXTENSIBILITY_IMPORTS ImportTable
    );

/* TRUE if the OS table is long enough to contain the slot at FieldOffset. */
BOOLEAN
KdxHasImport (
    _In_ SIZE_T FieldOffset
    );

#define KDX_HAS_IMPORT(Field) \
    (KdxHasImport(FIELD_OFFSET(KDNET_EXTENSIBILITY_IMPORTS, Field)) && \
     (KdxImports.Field != NULL))

/* Callers validate all required slots before using these wrappers. */
__forceinline ULONG KdxRead32(PUCHAR Base, ULONG Offset)
{
    return KdxImports.ReadRegisterULong((PULONG)(Base + Offset));
}

__forceinline VOID KdxWrite32(PUCHAR Base, ULONG Offset, ULONG Value)
{
    KdxImports.WriteRegisterULong((PULONG)(Base + Offset), Value);
}

__forceinline ULONG KdxReadPci(PDEBUG_DEVICE_DESCRIPTOR Device, ULONG Offset,
                             PVOID Buffer, ULONG Length)
{
    return KdxImports.GetPciDataByOffset(Device->Bus, Device->Slot, Buffer, Offset, Length);
}

__forceinline ULONG KdxWritePci(PDEBUG_DEVICE_DESCRIPTOR Device, ULONG Offset,
                              PVOID Buffer, ULONG Length)
{
    return KdxImports.SetPciDataByOffset(Device->Bus, Device->Slot, Buffer, Offset, Length);
}

__forceinline ULONG64 KdxPhysicalAddress(PVOID Address)
{
    return (ULONG64)KdxImports.GetPhysicalAddress(Address).QuadPart;
}

__forceinline VOID KdxStall(ULONG Microseconds)
{
    KdxImports.StallExecutionProcessor(Microseconds);
}

__forceinline ULONG64 KdxCycles(ULONG64 *Frequency)
{
    return KdxImports.ReadCycleCounter(Frequency);
}

/*
 * Report buffer: one line of plain text kept in both ASCII (for
 * KdNetDbgPrintf) and UTF-16 (for KdNetErrorString). Output is truncated
 * silently when the buffer fills. Strings must not contain '%', because the
 * ASCII copy is passed to KdNetDbgPrintf as its format string.
 */
#define KDX_REPORT_CHARS 1536

typedef struct _KDX_REPORT {
    ULONG Length;
    CHAR Ascii[KDX_REPORT_CHARS];
    WCHAR Wide[KDX_REPORT_CHARS];
} KDX_REPORT, *PKDX_REPORT;

extern KDX_REPORT KdxReport;

VOID KdxReportReset (VOID);
VOID KdxPutStr (_In_z_ PCSTR String);
VOID KdxPutHex (_In_ ULONG64 Value, _In_ ULONG Digits);
VOID KdxPutDec (_In_ ULONG64 Value);

/* Sends the ASCII report through KdNetDbgPrintf when that import exists. */
VOID KdxLogReport (VOID);

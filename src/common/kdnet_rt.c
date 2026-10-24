/*
 * Minimal runtime for KDNET extensibility modules. See kdnet_rt.h.
 */

#include "kdnet_rt.h"

KDNET_EXTENSIBILITY_IMPORTS KdxImports;
ULONG KdxImportCount;
KDX_REPORT KdxReport;

/*
 * The compiler may turn struct copies and zeroing into memcpy/memset calls.
 * With no CRT linked they must be defined here. The volatile pointers stop
 * the optimizer from recognizing the loops and calling these routines
 * recursively.
 */
#if !defined(KDX_USE_HOST_CRT)
#pragma function(memset, memcpy)

void *
__cdecl
memset (
    void *Destination,
    int Value,
    size_t Count
    )
{
    volatile unsigned char *Bytes = (volatile unsigned char *)Destination;

    while (Count-- != 0) {
        *Bytes++ = (unsigned char)Value;
    }

    return Destination;
}

void *
__cdecl
memcpy (
    void *Destination,
    const void *Source,
    size_t Count
    )
{
    volatile unsigned char *To = (volatile unsigned char *)Destination;
    const volatile unsigned char *From = (const volatile unsigned char *)Source;

    while (Count-- != 0) {
        *To++ = *From++;
    }

    return Destination;
}

#endif /* KDX_USE_HOST_CRT: user-mode simulated-device tests only */

VOID
KdxCaptureImports (
    _In_ PKDNET_EXTENSIBILITY_IMPORTS ImportTable
    )
{
    ULONG Slots;

    KdxImportCount = ImportTable->FunctionCount;
    Slots = KdxImportCount;
    if (Slots > KDNET_EXT_IMPORTS) {
        Slots = KDNET_EXT_IMPORTS;
    }

    memset(&KdxImports, 0, sizeof(KdxImports));
    memcpy(&KdxImports, ImportTable, KDX_IMPORT_SLOT_BASE + Slots * sizeof(PVOID));
}

BOOLEAN
KdxHasImport (
    _In_ SIZE_T FieldOffset
    )
{
    return (BOOLEAN)(FieldOffset < KDX_IMPORT_SLOT_BASE + (SIZE_T)KdxImportCount * sizeof(PVOID));
}

VOID
KdxReportReset (
    VOID
    )
{
    KdxReport.Length = 0;
    KdxReport.Ascii[0] = '\0';
    KdxReport.Wide[0] = L'\0';
}

static
VOID
KdxPutChar (
    _In_ CHAR Character
    )
{
    if (KdxReport.Length + 1 >= KDX_REPORT_CHARS) {
        return;
    }

    KdxReport.Ascii[KdxReport.Length] = Character;
    KdxReport.Wide[KdxReport.Length] = (WCHAR)(UCHAR)Character;
    KdxReport.Length += 1;
    KdxReport.Ascii[KdxReport.Length] = '\0';
    KdxReport.Wide[KdxReport.Length] = L'\0';
}

VOID
KdxPutStr (
    _In_z_ PCSTR String
    )
{
    while (*String != '\0') {
        KdxPutChar(*String++);
    }
}

VOID
KdxPutHex (
    _In_ ULONG64 Value,
    _In_ ULONG Digits
    )
{
    static const CHAR HexDigits[] = "0123456789abcdef";
    ULONG Index;

    if (Digits == 0 || Digits > 16) {
        Digits = 16;
    }

    for (Index = Digits; Index != 0; Index -= 1) {
        KdxPutChar(HexDigits[(Value >> ((Index - 1) * 4)) & 0xF]);
    }
}

VOID
KdxPutDec (
    _In_ ULONG64 Value
    )
{
    CHAR Digits[20];
    ULONG Count = 0;

    do {
        Digits[Count++] = (CHAR)('0' + (Value % 10));
        Value /= 10;
    } while (Value != 0);

    while (Count != 0) {
        KdxPutChar(Digits[--Count]);
    }
}

VOID
KdxLogReport (
    VOID
    )
{
    if (KDX_HAS_IMPORT(KdNetDbgPrintf)) {
        KdxImports.KdNetDbgPrintf(KdxReport.Ascii);
        KdxImports.KdNetDbgPrintf("\n");
    }
}

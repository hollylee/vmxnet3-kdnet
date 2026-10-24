/* User-mode fault-injection harness using the actual WDK types and driver.
 * No real PCI/MMIO calls: all KDNET imports point to this simulated device.
 */
#include "../src/vmxnet3/vmxnet3.c"
#include <stdio.h>
static ULONG Failures, Checks;
#define CHECK(Condition) do { ++Checks; if (!(Condition)) { \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #Condition); ++Failures; } } while (0)

static __declspec(align(4096)) UCHAR Memory[VMX_CONTEXT_SIZE + 4096];
static ULONG Bar0[1024], Bar1[1024];
static DEBUG_DEVICE_DESCRIPTOR Device;
static KDNET_SHARED_DATA Shared;
static KDNET_EXTENSIBILITY_IMPORTS Imports;
static KDNET_EXTENSIBILITY_EXPORTS Exports;
static UCHAR Mac[6], LinkState;
static USHORT PciCommand;
static ULONG CommandResult, Activations, LinkPolls, RegisterWrites, Resets;
static ULONG ActivationError, QueueError, BadPci, BadDma, ShortPciWrite;
static BOOLEAN LinkDown, ZeroClock, FrozenClock, FreezeAfterActivation, CleanupFailure;
static ULONG64 Cycles;
static BOOLEAN Active;
static VOID (*ExtraWrite)(PULONG, ULONG);
static VOID (*ExtraStall)(VOID);

static ULONG ReadWire32(const VOID *Address)
{
    const UCHAR *Bytes = Address;
    return Bytes[0] | ((ULONG)Bytes[1] << 8) | ((ULONG)Bytes[2] << 16) | ((ULONG)Bytes[3] << 24);
}

static PHYSICAL_ADDRESS NTAPI FakePhysical(PVOID Address)
{
    ULONG_PTR Offset = (ULONG_PTR)Address - (ULONG_PTR)Memory;
    PHYSICAL_ADDRESS Pa;
    CHECK(Offset < sizeof(Memory));
    /* Discontiguous physical pages above 4 GB test both page layout and DSAH. */
    Pa.QuadPart = 0x100000000ULL + (Offset / 4096) * 8192 + Offset % 4096;
    if (BadDma && Offset == 4096 + sizeof(VMX_SHARED) - 1) {
        Pa.QuadPart += 1;
    }
    return Pa;
}

static ULONG NTAPI FakePciRead(ULONG Bus, ULONG Slot, PVOID Buffer, ULONG Offset, ULONG Length)
{
    ULONG Id = BadPci ? 0x12345678 : 0x07B015AD;
    CHECK(Bus == 1 && Slot == 0);
    if (Offset == 0 && Length == 4) {
        memcpy(Buffer, &Id, 4);
    } else {
        CHECK(Offset == 4 && Length == 2);
        memcpy(Buffer, &PciCommand, 2);
    }
    return Length;
}

static ULONG NTAPI FakePciWrite(ULONG Bus, ULONG Slot, PVOID Buffer, ULONG Offset, ULONG Length)
{
    USHORT Command;
    CHECK(Bus == 1 && Slot == 0 && Offset == 4 && Length == 2);
    memcpy(&Command, Buffer, 2);
    if (CleanupFailure && Activations != 0 && (Command & VMX_PCI_MASTER) == 0) {
        return 0;
    }
    if (ShortPciWrite != 0) {
        --ShortPciWrite;
        return 0;
    }
    PciCommand = Command;
    return Length;
}

static ULONG NTAPI FakeRead32(PULONG Register)
{
    if (Register == &Bar1[VMX_REG_CMD / 4]) {
        return CommandResult;
    }
    return *Register;
}

static VOID CheckActivation(VOID)
{
    VMX_CONTEXT *Context = Shared.Hardware;
    UCHAR *Ds = Context->Region[0].Va;
    UCHAR *Q = Context->Region[1].Va;
    ULONG I;
    CHECK((PciCommand & VMX_PCI_MASTER) != 0);
    CHECK(Bar1[VMX_REG_DSAL / 4] == (ULONG)Context->Region[0].Pa);
    CHECK(Bar1[VMX_REG_DSAH / 4] == 1);
    /* Check actual wire offsets, independently of the reduced C structures. */
    CHECK(ReadWire32(Ds) == 0xBABEFEE1);
    CHECK(ReadWire32(Ds + 12) == 10); /* GOS: Windows, 64-bit */
    CHECK(ReadWire32(Ds + 16) == 1 && ReadWire32(Ds + 20) == 1);
    CHECK(ReadWire32(Ds + 24) == 0); /* no offloads */
    CHECK(ReadWire32(Ds + 52) == 512 && ReadWire32(Ds + 56) == 1500);
    CHECK(Ds[62] == 1 && Ds[63] == 1);
    CHECK(Ds[81] == 1 && ReadWire32(Ds + 108) == 1);
    CHECK(ReadWire32(Ds + 120) == 13 && ReadWire32(Ds + 136) == 1);
    CHECK(ReadWire32(Q + 56) == 32 && ReadWire32(Q + 60) == 32 && ReadWire32(Q + 64) == 32);
    CHECK(ReadWire32(Q + 256 + 56) == 32 && ReadWire32(Q + 256 + 60) == 32);
    CHECK(ReadWire32(Q + 256 + 64) == 64);
    for (I = 0; I < 25; ++I) {
        CHECK(Bar0[I * 2] == 1);
    }
}

static VOID CheckReceive(VOID)
{
    VMX_CONTEXT *Context = Shared.Hardware;
    ULONG Ring, I;
    for (Ring = 0; Ring < 2; ++Ring) {
        VMX_DESC *Desc = Context->Region[5 + Ring].Va;
        CHECK(Bar0[(Ring ? VMX_REG_RXPROD2 : VMX_REG_RXPROD) / 4] == 31);
        for (I = 0; I < 31; ++I) {
            CHECK(Desc[I].Address == Context->BufferPA[(1 + Ring) * 32 + I]);
            CHECK(Desc[I].Word2 == (0x80000800UL | (Ring ? 0x4000UL : 0)));
        }
        CHECK(Desc[31].Word2 == 0);
    }
}

static VOID NTAPI FakeWrite32(PULONG Register, ULONG Value)
{
    ++RegisterWrites;
    if (ExtraWrite != NULL) { ExtraWrite(Register, Value); }
    if (Register == &Bar1[VMX_REG_ECR / 4]) { *Register &= ~Value; return; }
    *Register = Value;
    if (Register != &Bar1[VMX_REG_CMD / 4]) {
        return;
    }
    CommandResult = 0;
    switch (Value) {
    case VMX_CMD_ACTIVATE:
        CheckActivation();
        ++Activations;
        Active = (BOOLEAN)(ActivationError == 0);
        CommandResult = ActivationError;
        break;
    case VMX_CMD_QUIESCE:
        Active = FALSE;
        break;
    case VMX_CMD_RESET:
        Bar1[VMX_REG_ECR / 4] = 0;
        ++Resets;
        Active = FALSE;
        break;
    case VMX_CMD_LINK:
        CHECK(Active);
        if (LinkPolls++ == 0) {
            CheckReceive();
        }
        CommandResult = (10000UL << 16) | (LinkDown ? 0 : 1);
        break;
    case VMX_CMD_QUEUE:
        if (QueueError) {
            VMX_CONTEXT *Context = Shared.Hardware;
            VMX_QUEUES *Q = Context->Region[1].Va;
            Q->Rx.Status.Error = 0x80000001;
        }
        break;
    default:
        break;
    }
}

static VOID NTAPI FakeStall(ULONG Microseconds)
{
    if (ExtraStall != NULL) { ExtraStall(); }
    if (!FrozenClock && !(FreezeAfterActivation && Activations != 0)) {
        Cycles += Microseconds;
    }
}

static ULONG64 NTAPI FakeCycles(ULONG64 *Frequency)
{
    if (Frequency != NULL) {
        *Frequency = ZeroClock ? 0 : 1000000;
    }
    return Cycles;
}

static VOID Setup(VOID)
{
    ExtraWrite = NULL; ExtraStall = NULL;
    memset(Memory, 0xCC, sizeof(Memory));
    memset(Bar0, 0, sizeof(Bar0)); memset(Bar1, 0, sizeof(Bar1));
    memset(&Device, 0, sizeof(Device)); memset(&Shared, 0, sizeof(Shared));
    memset(&Imports, 0, sizeof(Imports)); memset(&Exports, 0, sizeof(Exports));
    memset(Mac, 0, sizeof(Mac)); LinkState = 0;
    CommandResult = Activations = LinkPolls = RegisterWrites = Resets = 0;
    ActivationError = QueueError = BadPci = BadDma = ShortPciWrite = 0;
    LinkDown = ZeroClock = FrozenClock = FreezeAfterActivation = CleanupFailure = FALSE;
    Cycles = 100; Active = FALSE;
    PciCommand = VMX_PCI_MEMORY | VMX_PCI_MASTER;
    Bar1[VMX_REG_VRRS / 4] = 7; Bar1[VMX_REG_UVRS / 4] = 1;
    Bar1[VMX_REG_MACL / 4] = 0x56341202; Bar1[VMX_REG_MACH / 4] = 0x9A78;
    Device.Bus = 1; Device.VendorID = 0x15AD; Device.DeviceID = 0x07B0;
    Device.DbgBarsMapped = TRUE;
    Device.BaseAddress[0].Valid = Device.BaseAddress[1].Valid = TRUE;
    Device.BaseAddress[0].Type = Device.BaseAddress[1].Type = CmResourceTypeMemory;
    Device.BaseAddress[0].TranslatedAddress = (PUCHAR)Bar0;
    Device.BaseAddress[1].TranslatedAddress = (PUCHAR)Bar1;
    Device.BaseAddress[0].Length = Device.BaseAddress[1].Length = 4096;
    /* Eight-byte aligned but NOT page-aligned: exercises alignment slack. */
    Device.Memory.VirtualAddress = Memory + 8;
    Device.Memory.Cached = TRUE;
    Shared.Device = &Device; Shared.Hardware = Memory + 8;
    Shared.TargetMacAddress = Mac; Shared.LinkState = &LinkState;
    Imports.FunctionCount = 33; Imports.Exports = &Exports; Exports.FunctionCount = 15;
    Imports.GetPhysicalAddress = FakePhysical; Imports.GetPciDataByOffset = FakePciRead;
    Imports.SetPciDataByOffset = FakePciWrite; Imports.ReadRegisterULong = FakeRead32;
    Imports.WriteRegisterULong = FakeWrite32; Imports.StallExecutionProcessor = FakeStall;
    Imports.ReadCycleCounter = FakeCycles;
    CHECK(KdInitializeLibrary(&Imports, NULL, &Device) == STATUS_SUCCESS);
    CHECK(KdInitializeLibrary(&Imports, NULL, &Device) == STATUS_SUCCESS);
    CHECK(Device.Memory.Length == VMX_CONTEXT_SIZE);
    CHECK(Exports.KdDeviceControl == NULL && Exports.KdReadSerialByte == NULL);
}

static VOID CheckGuards(VOID)
{
    ULONG I;
    for (I = 0; I < 8; ++I) { CHECK(Memory[I] == 0xCC); }
    for (I = VMX_CONTEXT_SIZE + 8; I < sizeof(Memory); ++I) { CHECK(Memory[I] == 0xCC); }
}

#ifndef VMX_PACKET_TEST
static VOID RunExpect(ULONG Stage, ULONG Detail, BOOLEAN Cleanup)
{
    NTSTATUS Status = Exports.KdInitializeController(&Shared);
    CHECK(Status == VmxStatus(Stage, Detail));
    CHECK(!NT_SUCCESS(Status));
    if (Cleanup) {
        CHECK((PciCommand & VMX_PCI_MASTER) == 0);
        CHECK(!Active);
        CHECK(Bar1[VMX_REG_DSAL / 4] == 0 && Bar1[VMX_REG_DSAH / 4] == 0);
        Exports.KdShutdownController(&Shared); /* repeated shutdown is harmless */
        CHECK((PciCommand & VMX_PCI_MASTER) == 0);
    }
    CheckGuards();
}

int main(VOID)
{
    Setup();
#if VMX_INIT_STOP_AFTER != 0
    RunExpect(0x40 | VMX_INIT_STOP_AFTER, 0, TRUE);
    CHECK(Activations == (VMX_INIT_STOP_AFTER == 12 ? 1UL : 0UL));
    CHECK(LinkPolls == 0);
#else
    RunExpect(VmxComplete, 10000, TRUE);
    CHECK(Activations == 1 && Resets >= 2);
    CHECK(Mac[0] == 2 && Mac[1] == 0x12 && Mac[5] == 0x9A);
    CHECK(Shared.LinkSpeed == 10000 && Shared.LinkDuplex == 2 && LinkState == 1);
    /* Re-initialize the same memory after cleanup. */
    RunExpect(VmxComplete, 10000, TRUE);
    CHECK(Activations == 2);

    Setup(); ActivationError = 7; RunExpect(VmxActivate, 7, TRUE);
    CHECK(LinkPolls == 0);
    Setup(); LinkDown = TRUE; RunExpect(VmxLink, 1, TRUE);
    CHECK(LinkPolls > 1 && LinkPolls <= VMX_LINK_POLLS);
    Setup(); LinkDown = FreezeAfterActivation = TRUE; RunExpect(VmxLink, 1, TRUE);
    CHECK(LinkPolls == VMX_LINK_POLLS); /* counter failure cannot loop forever */
    Setup(); QueueError = 1; RunExpect(VmxQueues, 2, TRUE);
    Setup(); BadDma = 1; RunExpect(VmxDma, 1, TRUE); CHECK(Activations == 0);
    Setup(); ZeroClock = TRUE; RunExpect(VmxClock, 1, TRUE); CHECK(Activations == 0);
    Setup(); FrozenClock = TRUE; RunExpect(VmxClock, 2, TRUE);
    Setup(); Bar1[VMX_REG_VRRS / 4] = 2; RunExpect(VmxRevision, 1, TRUE);
    Setup(); Bar1[VMX_REG_UVRS / 4] = 0; RunExpect(VmxRevision, 2, TRUE);
    Setup(); Bar1[VMX_REG_MACL / 4] = 1; RunExpect(VmxMac, 1, TRUE);
    Setup(); ShortPciWrite = 1; RunExpect(VmxPci, 3, TRUE);
    Setup(); BadPci = 1; RunExpect(VmxPci, 1, FALSE); CHECK(RegisterWrites == 0);
    Setup(); Device.Segment = 1; RunExpect(VmxAdapter, 2, FALSE); CHECK(RegisterWrites == 0);
    Setup(); Shared.Hardware = Memory; RunExpect(VmxAdapter, 1, FALSE);
    Setup(); Device.Memory.Length = 4096; RunExpect(VmxMemory, 1, FALSE);
    Setup(); Device.DbgBarsMapped = FALSE; RunExpect(VmxBars, 0xFF, FALSE);
    Setup(); Device.BaseAddress[0].Length = VMX_REG_RXPROD2; RunExpect(VmxBars, 0, FALSE);
    Setup(); Shared.TargetMacAddress = NULL; RunExpect(VmxPublish, 1, FALSE);
    Setup(); CleanupFailure = TRUE; RunExpect(VmxCleanup, VmxComplete, FALSE);
    CHECK(((VMX_CONTEXT *)Shared.Hardware)->LastStage == VmxComplete);

    Setup(); Imports.FunctionCount = 32;
    CHECK(KdInitializeLibrary(&Imports, NULL, &Device) == VmxStatus(VmxTables, 32 << 8));
    Setup(); Exports.FunctionCount = 14;
    CHECK(KdInitializeLibrary(&Imports, NULL, &Device) == VmxStatus(VmxTables, 0x210E));
    Setup(); Imports.WriteRegisterULong = NULL;
    CHECK(KdInitializeLibrary(&Imports, NULL, &Device) == VmxStatus(VmxImports, 11));
    CHECK(RegisterWrites == 0);
#endif
    printf("vmxnet3 init tests (checkpoint %u): %lu checks, %lu failures\n",
           (unsigned)VMX_INIT_STOP_AFTER, Checks, Failures);
    return Failures ? 1 : 0;
}
#endif

/*
 * Polled KDNET transport, with a separate initialization-only diagnostic build.
 * Diagnostics deliberately avoid the still-unverified error/printf slots.
 */
#include "kdnet_rt.h"
#include <intrin.h>
#include "vmxnet3_hw.h"

#ifndef VMX_TRANSPORT
#define VMX_TRANSPORT 0
#endif
#ifndef VMX_FAULT_INJECT
#define VMX_FAULT_INJECT 0
#endif
#if VMX_FAULT_INJECT && !VMX_TRANSPORT
#error Fault injection requires the transport variant
#endif

#ifndef VMX_INIT_STOP_AFTER
#define VMX_INIT_STOP_AFTER 0
#endif
#if VMX_INIT_STOP_AFTER != 0 && VMX_INIT_STOP_AFTER != 6 && \
    VMX_INIT_STOP_AFTER != 8 && VMX_INIT_STOP_AFTER != 9 && \
    VMX_INIT_STOP_AFTER != 10 && VMX_INIT_STOP_AFTER != 12
#error VMX_INIT_STOP_AFTER must be 0, 6, 8, 9, 10 or 12
#endif
#if VMX_TRANSPORT && VMX_INIT_STOP_AFTER
#error Transport and initialization checkpoints are separate builds
#endif

#define VMX_RING_COUNT 32
#define VMX_BUFFER_SIZE 2048
#define VMX_DMA_PAGE 4096
#define VMX_BUFFER_COUNT (3 * VMX_RING_COUNT)
/* Context + alignment slack + eight DMA pages + 96 two-KB buffers. */
#define VMX_CONTEXT_SIZE (10 * VMX_DMA_PAGE + VMX_BUFFER_COUNT * VMX_BUFFER_SIZE)
#define VMX_CONTEXT_MAGIC 0x564D5849UL
#define VMX_PCI_MEMORY 0x0002
#define VMX_PCI_MASTER 0x0004
#define VMX_PCI_INTX_DISABLE 0x0400
#define VMX_LINK_POLLS 3000
#define VMX_TX_POLLS 10000 /* 10 us each, also bounded if the clock stops. */
#define VMX_FRAME_MAX 1514
#define VMX_INVALID_SLOT 0xFF
#define VMX_HANDLE_INDEX 0x3F
#define VMX_TOKEN_MASK 0x007FFFFF
enum { VmxBufferFree, VmxBufferHeld, VmxBufferPosted };

/* E2SSDDDD: SS is the stage; DDDD is its detail (low 16 bits). */
enum {
    VmxTables = 1, VmxImports, VmxAdapter, VmxMemory, VmxBars, VmxPci,
    VmxRevision, VmxReset, VmxDma, VmxClock, VmxMac, VmxActivate,
    VmxLink, VmxQueues, VmxCleanup, VmxPublish, VmxTxCompletion,
    VmxRxCompletion, VmxTxTimeout, VmxRecovery, VmxComplete = 0x80
};

typedef struct _VMX_REGION {
    PVOID Va;
    ULONG64 Pa;
} VMX_REGION;

typedef struct _VMX_CONTEXT {
    ULONG Magic;
    ULONG LastStage, LastDetail;
    NTSTATUS Result;
    PDEBUG_DEVICE_DESCRIPTOR Device;
    PUCHAR Bar0, Bar1;
    BOOLEAN PciTouched, MmioReady;
    USHORT OriginalCommand;
    VMX_REGION Region[8];
    ULONG64 BufferPA[VMX_BUFFER_COUNT];
    ULONG64 CycleHz;
    ULONG Link;
    UCHAR Mac[6];
    BOOLEAN Running;
    PKDNET_SHARED_DATA Shared;
    PUCHAR Buffers;
    ULONG HandleSequence;
    UCHAR TxState[VMX_RING_COUNT], TxSlot[VMX_RING_COUNT];
    ULONG TxToken[VMX_RING_COUNT];
    ULONG64 TxStarted[VMX_RING_COUNT];
    ULONG TxFill, TxNext, TxUsed, TxOwned, TxGen, TxComp, TxCompGen;
    UCHAR RxState[2 * VMX_RING_COUNT], RxSlot[2][VMX_RING_COUNT];
    ULONG RxToken[2 * VMX_RING_COUNT], RxLength[2 * VMX_RING_COUNT];
    ULONG RxFill[2], RxNext[2], RxPosted[2], RxGen[2], RxComp, RxCompGen;
    ULONG ServiceCalls, Restarts, TxPackets, RxPackets, RxDropped, TxTimeouts;
    ULONG LastTxError, LastRxError;
#if VMX_FAULT_INJECT
    ULONG FaultCount, FaultOutcome, FaultSlot, FaultBuffer;
    ULONG FaultWord2, FaultWord3, FaultRestartsBefore;
    ULONG FaultEvents, FaultTxError, FaultRxError, FaultTxStopped, FaultRxStopped;
    ULONG FaultRecoveryStage, FaultRecoveryDone;
    NTSTATUS FaultRecoveryResult;
    ULONG FaultDescriptors, FaultLastSlot;
#endif
} VMX_CONTEXT;

C_ASSERT(sizeof(VMX_CONTEXT) <= VMX_DMA_PAGE);
C_ASSERT(VMX_RING_COUNT * sizeof(VMX_DESC) <= VMX_DMA_PAGE);
C_ASSERT(VMX_RING_COUNT * 128 == VMX_DMA_PAGE);
C_ASSERT(2 * VMX_RING_COUNT * sizeof(VMX_DESC) <= VMX_DMA_PAGE);
C_ASSERT((VMX_RING_COUNT % 32) == 0);

static PDEBUG_DEVICE_DESCRIPTOR VmxLibraryDevice;
static PKDNET_SHARED_DATA VmxSharedData;
#if VMX_FAULT_INJECT
/* Explicit debugger opt-in; never set by the module itself. */
#define VMX_FAULT_ARM 0x5146554CUL
#define VMX_RESET_ARM 0x52535431UL
#define VMX_TEST_RESET_STAGE 0x15UL
#define VMX_FAULT_DESCRIPTORS 17 /* BSD rev-1 limit is 16 for non-TSO packets. */
static volatile ULONG VmxFaultArm;
#endif

static NTSTATUS VmxStatus(ULONG Stage, ULONG Detail)
{
    return (NTSTATUS)(0xE2000000UL | (Stage << 16) | (Detail & 0xFFFF));
}

static VOID VmxBarrier(VOID)
{
    /* Publish descriptors/shared memory before ownership bits and MMIO.
     * SY is conservative for device DMA; no inner-shareable assumption. */
#if defined(_ARM64_)
    __dmb(_ARM64_BARRIER_SY);
#else
    _ReadWriteBarrier();
    _mm_mfence();
#endif
}

static VOID VmxWrite(PUCHAR Bar, ULONG Offset, ULONG Value)
{
    VmxBarrier();
    KdxWrite32(Bar, Offset, Value);
}

static ULONG VmxCommand(VMX_CONTEXT *Context, ULONG Command)
{
    VmxWrite(Context->Bar1, VMX_REG_CMD, Command);
    /* Flush the write. Only GET/ACTIVATE commands define a result. */
    return KdxRead32(Context->Bar1, VMX_REG_CMD);
}

static BOOLEAN VmxStopDevice(VMX_CONTEXT *Context)
{
    USHORT Command, Readback = VMX_PCI_MASTER;
    ULONG Index;
    BOOLEAN Ok;

    if (!Context->PciTouched) {
        return TRUE;
    }
    if (Context->MmioReady) {
        for (Index = 0; Index < VMX_MAX_INTRS; ++Index) {
            VmxWrite(Context->Bar0, VMX_REG_IMR + Index * 8, 1);
        }
        (VOID)VmxCommand(Context, VMX_CMD_QUIESCE);
        (VOID)VmxCommand(Context, VMX_CMD_RESET);
        VmxWrite(Context->Bar1, VMX_REG_DSAL, 0);
        VmxWrite(Context->Bar1, VMX_REG_DSAH, 0);
        (VOID)KdxRead32(Context->Bar1, VMX_REG_DSAH);
    }
    /* Restore other original PCI bits, but never leave this probe doing DMA. */
    Command = Context->OriginalCommand & (USHORT)~VMX_PCI_MASTER;
    Ok = (BOOLEAN)(KdxWritePci(Context->Device, 4, &Command, sizeof(Command)) == sizeof(Command));
    if (KdxReadPci(Context->Device, 4, &Readback, sizeof(Readback)) != sizeof(Readback) ||
        (Readback & VMX_PCI_MASTER) != 0) {
        Ok = FALSE;
    }
    if (Ok) {
        Context->PciTouched = FALSE;
        Context->MmioReady = FALSE;
        Context->Running = FALSE;
    }
    return Ok;
}

static NTSTATUS VmxFinish(VMX_CONTEXT *Context, ULONG Stage, ULONG Detail)
{
    Context->LastStage = Stage;
    Context->LastDetail = Detail;
    Context->Result = VmxStatus(Stage, Detail);
    if (!VmxStopDevice(Context)) {
        /* Preserve the original full detail in the context for a boot debugger. */
        Context->Result = VmxStatus(VmxCleanup, Stage);
    }
    return Context->Result;
}

#define VMX_CHECKPOINT(Stage) \
    if (VMX_INIT_STOP_AFTER == (Stage)) { \
        return VmxFinish(Context, 0x40 | (Stage), 0); \
    }

static BOOLEAN VmxPhysicalRegion(PVOID Address, ULONG Length, ULONG Alignment,
                                 ULONG64 *Physical)
{
    ULONG_PTR Start = (ULONG_PTR)Address;
    ULONG_PTR Last, Cursor;
    ULONG64 Base = KdxPhysicalAddress(Address);

    if (Length == 0 || Start > MAXULONG_PTR - Length || Base == 0 ||
        (Base & (Alignment - 1)) != 0 || Base > (~0ULL) - Length) {
        return FALSE;
    }
    Last = Start + Length - 1;
    /* Check every crossed page and the last byte, including short regions. */
    Cursor = (Start & ~(ULONG_PTR)(VMX_DMA_PAGE - 1)) + VMX_DMA_PAGE;
    while (Cursor <= Last) {
        if (KdxPhysicalAddress((PVOID)Cursor) != Base + Cursor - Start) {
            return FALSE;
        }
        Cursor += VMX_DMA_PAGE;
    }
    if (KdxPhysicalAddress((PVOID)Last) != Base + Length - 1) {
        return FALSE;
    }
    *Physical = Base;
    return TRUE;
}

static ULONG VmxLayout(VMX_CONTEXT *Context)
{
    static const ULONG Sizes[8] = {
        sizeof(VMX_SHARED), sizeof(VMX_QUEUES), VMX_RING_COUNT * sizeof(VMX_DESC),
        VMX_RING_COUNT * 128, VMX_RING_COUNT * sizeof(VMX_DESC),
        VMX_RING_COUNT * sizeof(VMX_DESC), VMX_RING_COUNT * sizeof(VMX_DESC),
        2 * VMX_RING_COUNT * sizeof(VMX_DESC)
    };
    ULONG_PTR Base = (ULONG_PTR)Context;
    ULONG_PTR First = (Base + sizeof(*Context) + VMX_DMA_PAGE - 1) &
                      ~(ULONG_PTR)(VMX_DMA_PAGE - 1);
    ULONG_PTR Buffers = First + 8 * VMX_DMA_PAGE;
    ULONG Index;

    if (Buffers + VMX_BUFFER_COUNT * VMX_BUFFER_SIZE > Base + VMX_CONTEXT_SIZE) {
        return 0xFFFF;
    }
    Context->Buffers = (PUCHAR)Buffers;
    for (Index = 0; Index < 8; ++Index) {
        Context->Region[Index].Va = (PVOID)(First + Index * VMX_DMA_PAGE);
        if (!VmxPhysicalRegion(Context->Region[Index].Va, Sizes[Index],
                               512, &Context->Region[Index].Pa)) {
            return Index + 1;
        }
    }
    for (Index = 0; Index < VMX_BUFFER_COUNT; ++Index) {
        if (!VmxPhysicalRegion((PVOID)(Buffers + Index * VMX_BUFFER_SIZE),
                               VMX_BUFFER_SIZE, 64, &Context->BufferPA[Index])) {
            return 0x100 + Index;
        }
    }
    /* Only touch the memory we requested, and only after reset has stopped DMA. */
    memset((PVOID)First, 0, 8 * VMX_DMA_PAGE + VMX_BUFFER_COUNT * VMX_BUFFER_SIZE);
    return 0;
}

static VOID VmxConfigure(VMX_CONTEXT *Context)
{
    VMX_SHARED *Shared = (VMX_SHARED *)Context->Region[0].Va;
    VMX_QUEUES *Queues = (VMX_QUEUES *)Context->Region[1].Va;

    Shared->Magic = VMX_MAGIC;
    Shared->Misc.DriverInfo.Version = 0x00010000;
    Shared->Misc.DriverInfo.Gos = VMX_GOS_WIN64;
    Shared->Misc.DriverInfo.Vmxnet3RevSpt = 1;
    Shared->Misc.DriverInfo.UptVerSpt = 1;
    Shared->Misc.QueueDescPA = Context->Region[1].Pa;
    Shared->Misc.QueueDescLen = sizeof(*Queues);
    Shared->Misc.Mtu = 1500;
    Shared->Misc.MaxNumRxSG = 1;
    Shared->Misc.NumTxQueues = 1;
    Shared->Misc.NumRxQueues = 1;
    Shared->Intr.NumIntrs = 1;
    Shared->Intr.IntrCtrl = VMX_INTR_DISABLE;
    Shared->Filter.RxMode = VMX_RX_MODE;
    Shared->Filter.VfTable[0] = 1; /* Untagged traffic (VLAN 0). */

    Queues->Tx.Threshold = 1;
    Queues->Tx.Conf.RingPA = Context->Region[2].Pa;
    Queues->Tx.Conf.DataPA = Context->Region[3].Pa;
    Queues->Tx.Conf.CompPA = Context->Region[4].Pa;
    Queues->Tx.Conf.RingSize = VMX_RING_COUNT;
    Queues->Tx.Conf.DataSize = VMX_RING_COUNT;
    Queues->Tx.Conf.CompSize = VMX_RING_COUNT;
    Queues->Rx.Conf.RingPA[0] = Context->Region[5].Pa;
    Queues->Rx.Conf.RingPA[1] = Context->Region[6].Pa;
    Queues->Rx.Conf.CompPA = Context->Region[7].Pa;
    Queues->Rx.Conf.RingSize[0] = VMX_RING_COUNT;
    Queues->Rx.Conf.RingSize[1] = VMX_RING_COUNT;
    Queues->Rx.Conf.CompSize = 2 * VMX_RING_COUNT;
}

static BOOLEAN VmxRecycleRx(VMX_CONTEXT *Context, ULONG Buffer)
{
    ULONG Ring = Buffer / VMX_RING_COUNT;
    ULONG Slot = Context->RxFill[Ring];
    ULONG Word = VMX_BUFFER_SIZE | (Ring ? VMX_RX_BODY : 0);
    volatile VMX_DESC *Desc = (volatile VMX_DESC *)Context->Region[5 + Ring].Va + Slot;
    if (Context->RxState[Buffer] != VmxBufferFree || Context->RxPosted[Ring] >= VMX_RING_COUNT - 1 ||
        Context->RxSlot[Ring][Slot] != VMX_INVALID_SLOT) {
        return FALSE;
    }
    /* Keep the old generation invisible while updating a reused descriptor. */
    Desc->Word2 = Word | (Context->RxGen[Ring] ? 0 : VMX_RX_GEN);
    Desc->Address = Context->BufferPA[VMX_RING_COUNT + Buffer];
    Desc->Word3 = 0;
    Context->RxSlot[Ring][Slot] = (UCHAR)Buffer;
    Context->RxState[Buffer] = VmxBufferPosted;
    ++Context->RxPosted[Ring];
    VmxBarrier(); /* Buffer address/length before device ownership. */
    Desc->Word2 = Word | (Context->RxGen[Ring] ? VMX_RX_GEN : 0);
    if (++Context->RxFill[Ring] == VMX_RING_COUNT) {
        Context->RxFill[Ring] = 0;
        Context->RxGen[Ring] ^= 1;
    }
    VmxWrite(Context->Bar0, Ring ? VMX_REG_RXPROD2 : VMX_REG_RXPROD, Context->RxFill[Ring]);
    return TRUE;
}

static VOID VmxPrepareRings(VMX_CONTEXT *Context)
{
    ULONG Index;
    Context->TxFill = Context->TxNext = Context->TxUsed = Context->TxComp = Context->TxOwned = 0;
    Context->TxGen = Context->TxCompGen = 1;
    Context->RxComp = 0; Context->RxCompGen = 1;
    memset(Context->TxSlot, VMX_INVALID_SLOT, sizeof(Context->TxSlot));
    memset(Context->RxSlot, VMX_INVALID_SLOT, sizeof(Context->RxSlot));
    for (Index = 0; Index < VMX_RING_COUNT; ++Index) {
        /* Prepared TX buffers remain owned by KDNET across recovery. */
        if (Context->TxState[Index] == VmxBufferHeld) {
            ++Context->TxOwned;
        } else {
            Context->TxState[Index] = VmxBufferFree;
        }
    }
    for (Index = 0; Index < 2; ++Index) {
        Context->RxFill[Index] = Context->RxNext[Index] = Context->RxPosted[Index] = 0;
        Context->RxGen[Index] = 1;
    }
    for (Index = 0; Index < 2 * VMX_RING_COUNT; ++Index) {
        if (Context->RxState[Index] != VmxBufferHeld) {
            Context->RxState[Index] = VmxBufferFree;
        }
    }
}

static VOID VmxPostReceive(VMX_CONTEXT *Context)
{
    ULONG Index;
    for (Index = 0; Index < 2 * VMX_RING_COUNT; ++Index) {
        if (Context->RxState[Index] == VmxBufferFree) {
            (VOID)VmxRecycleRx(Context, Index);
        }
    }
}

static NTSTATUS NTAPI VmxInitializeController(PKDNET_SHARED_DATA KdNet)
{
    PDEBUG_DEVICE_DESCRIPTOR Device = VmxLibraryDevice;
    VMX_CONTEXT *Context;
    ULONG Value, Index, MacLow, MacHigh, Link = 0;
    USHORT Command, Readback;
    ULONG64 Start, Now, Frequency;
    volatile VMX_QUEUES *Queues;

    if (KdNet == NULL || Device == NULL || KdNet->Device != Device ||
        KdNet->Hardware == NULL || KdNet->Hardware != Device->Memory.VirtualAddress) {
        return VmxStatus(VmxAdapter, 1);
    }
    if (Device->Memory.Length < VMX_CONTEXT_SIZE ||
        ((ULONG_PTR)KdNet->Hardware & 7) != 0 ||
        (ULONG_PTR)KdNet->Hardware > MAXULONG_PTR - VMX_CONTEXT_SIZE) {
        return VmxStatus(VmxMemory, 1);
    }
    if (Device->Segment != 0 || Device->VendorID != 0x15AD || Device->DeviceID != 0x07B0) {
        return VmxStatus(VmxAdapter, 2);
    }
    if (KdNet->TargetMacAddress == NULL || KdNet->LinkState == NULL) {
        return VmxStatus(VmxPublish, 1);
    }
    /* The ARM64 probe confirmed pre-mapped BARs. Fail before MMIO otherwise. */
    if (!Device->DbgBarsMapped) {
        return VmxStatus(VmxBars, 0xFF);
    }
    for (Index = 0; Index < 2; ++Index) {
        PDEBUG_DEVICE_ADDRESS Bar = &Device->BaseAddress[Index];
        ULONG Needed = Index ? VMX_REG_ECR + 4 : VMX_REG_RXPROD2 + 4;
        if (!Bar->Valid || Bar->Type != CmResourceTypeMemory ||
            Bar->TranslatedAddress == NULL || ((ULONG_PTR)Bar->TranslatedAddress & 3) != 0 ||
            Bar->Length < Needed) {
            return VmxStatus(VmxBars, Index);
        }
    }
    Context = (VMX_CONTEXT *)KdNet->Hardware;
    memset(Context, 0, sizeof(*Context));
#if VMX_FAULT_INJECT
    VmxFaultArm = 0; /* A full controller initialization requires fresh consent. */
#endif
    Context->Magic = VMX_CONTEXT_MAGIC;
    Context->Device = Device;
    Context->Shared = KdNet;
    VmxSharedData = KdNet;
    Context->Bar0 = Device->BaseAddress[0].TranslatedAddress;
    Context->Bar1 = Device->BaseAddress[1].TranslatedAddress;

    if (KdxReadPci(Device, 0, &Value, sizeof(Value)) != sizeof(Value) || Value != 0x07B015AD) {
        return VmxFinish(Context, VmxPci, 1);
    }
    if (KdxReadPci(Device, 4, &Command, sizeof(Command)) != sizeof(Command)) {
        return VmxFinish(Context, VmxPci, 2);
    }
    Context->OriginalCommand = Command;
    /* Keep DMA off until all physical addresses and descriptors are ready. */
    Command = (Command | VMX_PCI_MEMORY | VMX_PCI_INTX_DISABLE) & (USHORT)~VMX_PCI_MASTER;
    Context->PciTouched = TRUE;
    if (KdxWritePci(Device, 4, &Command, sizeof(Command)) != sizeof(Command) ||
        KdxReadPci(Device, 4, &Readback, sizeof(Readback)) != sizeof(Readback) || Readback != Command) {
        return VmxFinish(Context, VmxPci, 3);
    }
    Context->MmioReady = TRUE;
    VMX_CHECKPOINT(VmxPci);

    Value = KdxRead32(Context->Bar1, VMX_REG_VRRS);
    if (Value == MAXULONG || (Value & 1) == 0) {
        return VmxFinish(Context, VmxRevision, 1);
    }
    Value = KdxRead32(Context->Bar1, VMX_REG_UVRS);
    if (Value == MAXULONG || (Value & 1) == 0) {
        return VmxFinish(Context, VmxRevision, 2);
    }
    VmxWrite(Context->Bar1, VMX_REG_VRRS, 1);
    VmxWrite(Context->Bar1, VMX_REG_UVRS, 1);
    (VOID)VmxCommand(Context, VMX_CMD_QUIESCE);
    (VOID)VmxCommand(Context, VMX_CMD_RESET);
    for (Index = 0; Index < VMX_MAX_INTRS; ++Index) {
        VmxWrite(Context->Bar0, VMX_REG_IMR + Index * 8, 1);
    }
    VMX_CHECKPOINT(VmxReset);

    Value = VmxLayout(Context);
    if (Value != 0) {
        return VmxFinish(Context, VmxDma, Value);
    }
    VmxConfigure(Context);
    VmxPrepareRings(Context);
    VMX_CHECKPOINT(VmxDma);

    Frequency = 0;
    Start = KdxCycles(&Frequency);
    if (Frequency == 0 || Frequency > (~0ULL) / 3) {
        return VmxFinish(Context, VmxClock, 1);
    }
    Context->CycleHz = Frequency;
    KdxStall(1000);
    Now = KdxCycles(NULL);
    if (Now - Start == 0 || Now - Start > Frequency * 3) {
        return VmxFinish(Context, VmxClock, 2);
    }
    VMX_CHECKPOINT(VmxClock);

    MacLow = KdxRead32(Context->Bar1, VMX_REG_MACL);
    MacHigh = KdxRead32(Context->Bar1, VMX_REG_MACH);
    for (Index = 0; Index < 4; ++Index) {
        Context->Mac[Index] = (UCHAR)(MacLow >> (Index * 8));
    }
    Context->Mac[4] = (UCHAR)MacHigh;
    Context->Mac[5] = (UCHAR)(MacHigh >> 8);
    if ((MacLow == 0 && (MacHigh & 0xFFFF) == 0) || (MacLow & 1) != 0) {
        return VmxFinish(Context, VmxMac, 1);
    }
    /* Confirmed on ARM64 25H2: KDNET records the VM's actual NIC MAC. */
    memcpy(KdNet->TargetMacAddress, Context->Mac, sizeof(Context->Mac));

    VmxWrite(Context->Bar1, VMX_REG_DSAL, (ULONG)Context->Region[0].Pa);
    VmxWrite(Context->Bar1, VMX_REG_DSAH, (ULONG)(Context->Region[0].Pa >> 32));
    Command |= VMX_PCI_MASTER;
    if (KdxWritePci(Device, 4, &Command, sizeof(Command)) != sizeof(Command) ||
        KdxReadPci(Device, 4, &Readback, sizeof(Readback)) != sizeof(Readback) || Readback != Command) {
        return VmxFinish(Context, VmxPci, 4);
    }
    Value = VmxCommand(Context, VMX_CMD_ACTIVATE);
    if (Value != 0) {
        return VmxFinish(Context, VmxActivate, Value);
    }
    VMX_CHECKPOINT(VmxActivate);

    VmxPostReceive(Context);
    (VOID)VmxCommand(Context, VMX_CMD_RX_MODE);
    (VOID)VmxCommand(Context, VMX_CMD_MAC_FILTER);
    (VOID)VmxCommand(Context, VMX_CMD_VLAN);
    Start = KdxCycles(NULL);
    for (Index = 0; Index < VMX_LINK_POLLS; ++Index) {
        Link = VmxCommand(Context, VMX_CMD_LINK);
        if (Link == MAXULONG) {
            return VmxFinish(Context, VmxLink, 2);
        }
        if ((Link & 1) != 0) {
            break;
        }
        if (KdxCycles(NULL) - Start >= Frequency * 3) {
            break;
        }
        KdxStall(1000);
    }
    if ((Link & 1) == 0) {
        return VmxFinish(Context, VmxLink, 1);
    }
    if ((Link >> 16) == 0) {
        return VmxFinish(Context, VmxLink, 3);
    }
    Context->Link = Link;
    (VOID)VmxCommand(Context, VMX_CMD_QUEUE);
    VmxBarrier(); /* Observe device-written queue status after the GET command. */
    Queues = (volatile VMX_QUEUES *)Context->Region[1].Va;
    if (Queues->Tx.Status.Stopped || Queues->Tx.Status.Error != 0) {
        return VmxFinish(Context, VmxQueues, 1);
    }
    if (Queues->Rx.Status.Stopped || Queues->Rx.Status.Error != 0) {
        return VmxFinish(Context, VmxQueues, 2);
    }
    if ((KdxRead32(Context->Bar1, VMX_REG_ECR) & VMX_QUEUE_ERRORS) != 0) {
        return VmxFinish(Context, VmxQueues, 3);
    }
    /* Provisional KDNET conventions: Mbps, full duplex=2, connected=1. */
    KdNet->LinkSpeed = Link >> 16;
    KdNet->LinkDuplex = 2;
    *KdNet->LinkState = 1;
#if VMX_TRANSPORT
    Context->Running = TRUE;
    Context->Result = STATUS_SUCCESS;
    return STATUS_SUCCESS;
#else
    /* Report the observed link, then stop DMA before Windows reclaims memory. */
    return VmxFinish(Context, VmxComplete, Link >> 16);
#endif
}

#if VMX_TRANSPORT
#include "vmxnet3_packet.c"
#else
static VOID NTAPI VmxShutdownController(PVOID Adapter)
{
    PKDNET_SHARED_DATA Shared = (PKDNET_SHARED_DATA)Adapter;
    VMX_CONTEXT *Context;
    if (Shared == NULL || Shared->Device != VmxLibraryDevice || Shared->Hardware == NULL) {
        return;
    }
    Context = (VMX_CONTEXT *)Shared->Hardware;
    if (Context->Magic == VMX_CONTEXT_MAGIC && Context->Device == Shared->Device) {
        (VOID)VmxStopDevice(Context);
    }
}

static NTSTATUS NTAPI VmxGetRx(PVOID Adapter, PULONG Handle, PVOID *Packet, PULONG Length)
{
    UNREFERENCED_PARAMETER(Adapter);
    *Handle = 0; *Packet = NULL; *Length = 0;
    return STATUS_IO_TIMEOUT;
}
static VOID NTAPI VmxReleaseRx(PVOID Adapter, ULONG Handle)
{
    UNREFERENCED_PARAMETER(Adapter); UNREFERENCED_PARAMETER(Handle);
}
static NTSTATUS NTAPI VmxGetTx(PVOID Adapter, PULONG Handle)
{
    UNREFERENCED_PARAMETER(Adapter); *Handle = 0;
    return STATUS_IO_TIMEOUT;
}
static NTSTATUS NTAPI VmxSendTx(PVOID Adapter, ULONG Handle, ULONG Length)
{
    UNREFERENCED_PARAMETER(Adapter); UNREFERENCED_PARAMETER(Handle);
    UNREFERENCED_PARAMETER(Length);
    return STATUS_IO_TIMEOUT;
}
static PVOID NTAPI VmxPacketAddress(PVOID Adapter, ULONG Handle)
{
    UNREFERENCED_PARAMETER(Adapter); UNREFERENCED_PARAMETER(Handle);
    return NULL;
}
static ULONG NTAPI VmxPacketLength(PVOID Adapter, ULONG Handle)
{
    UNREFERENCED_PARAMETER(Adapter); UNREFERENCED_PARAMETER(Handle);
    return 0;
}
#endif

/* Hibernate image registration is phase 5 work; this build is for awake VMs. */
static VOID NTAPI VmxHibernate(VOID) { }
static ULONG NTAPI VmxContextSize(PDEBUG_DEVICE_DESCRIPTOR Device)
{
    UNREFERENCED_PARAMETER(Device);
    return VMX_CONTEXT_SIZE;
}

NTSTATUS NTAPI KdInitializeLibrary(PKDNET_EXTENSIBILITY_IMPORTS ImportTable,
                                  PCHAR LoaderOptions, PDEBUG_DEVICE_DESCRIPTOR Device)
{
    PKDNET_EXTENSIBILITY_EXPORTS Exports;
    UNREFERENCED_PARAMETER(LoaderOptions);
    __security_init_cookie();
    VmxLibraryDevice = NULL;
    VmxSharedData = NULL;
    if (ImportTable == NULL || Device == NULL) {
        return VmxStatus(VmxTables, 0);
    }
    /* Exact counts until probe evidence establishes another supported layout. */
    if (ImportTable->FunctionCount != KDNET_EXT_IMPORTS) {
        return VmxStatus(VmxTables, (ImportTable->FunctionCount & 0xFF) << 8);
    }
    Exports = ImportTable->Exports;
    if (Exports == NULL || Exports->FunctionCount != KDNET_EXT_EXPORTS) {
        return VmxStatus(VmxTables, (KDNET_EXT_IMPORTS << 8) |
                         (Exports ? Exports->FunctionCount & 0xFF : 0));
    }
    KdxCaptureImports(ImportTable);
#define VMX_REQUIRE(Field) \
    if (!KDX_HAS_IMPORT(Field)) { \
        return VmxStatus(VmxImports, (FIELD_OFFSET(KDNET_EXTENSIBILITY_IMPORTS, Field) - \
                          KDX_IMPORT_SLOT_BASE) / sizeof(PVOID)); \
    }
    VMX_REQUIRE(GetPciDataByOffset);
    VMX_REQUIRE(SetPciDataByOffset);
    VMX_REQUIRE(GetPhysicalAddress);
    VMX_REQUIRE(StallExecutionProcessor);
    VMX_REQUIRE(ReadRegisterULong);
    VMX_REQUIRE(WriteRegisterULong);
    VMX_REQUIRE(ReadCycleCounter);
#undef VMX_REQUIRE
    /* Clear optional serial/device-control callbacks so this is packet-only. */
    memset(&Exports->KdInitializeController, 0,
           sizeof(*Exports) - FIELD_OFFSET(KDNET_EXTENSIBILITY_EXPORTS, KdInitializeController));
    Exports->KdInitializeController = VmxInitializeController;
    Exports->KdShutdownController = VmxShutdownController;
    Exports->KdSetHibernateRange = VmxHibernate;
    Exports->KdGetRxPacket = VmxGetRx;
    Exports->KdReleaseRxPacket = VmxReleaseRx;
    Exports->KdGetTxPacket = VmxGetTx;
    Exports->KdSendTxPacket = VmxSendTx;
    Exports->KdGetPacketAddress = VmxPacketAddress;
    Exports->KdGetPacketLength = VmxPacketLength;
    Exports->KdGetHardwareContextSize = VmxContextSize;
    Device->Memory.Length = VMX_CONTEXT_SIZE;
    VmxLibraryDevice = Device;
    return STATUS_SUCCESS;
}

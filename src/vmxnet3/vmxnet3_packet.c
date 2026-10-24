/* Included by vmxnet3.c for the transport build. All state is in KDNET memory.
 * KDNET serializes these callbacks; no locks or kernel/CRT imports are used.
 */
static VMX_CONTEXT *VmxGetContext(PVOID Adapter)
{
    VMX_CONTEXT *Context;
    if (VmxSharedData == NULL || VmxLibraryDevice == NULL ||
        (Adapter != VmxSharedData && Adapter != VmxLibraryDevice->Memory.VirtualAddress)) {
        return NULL;
    }
    Context = (VMX_CONTEXT *)VmxLibraryDevice->Memory.VirtualAddress;
    if (Context == NULL || Context->Magic != VMX_CONTEXT_MAGIC ||
        Context->Device != VmxLibraryDevice || Context->Shared != VmxSharedData) {
        return NULL;
    }
    return Context;
}

static ULONG VmxNewHandle(VMX_CONTEXT *Context, ULONG Buffer)
{
    Context->HandleSequence = (Context->HandleSequence + 1) & VMX_TOKEN_MASK;
    if (Context->HandleSequence == 0) { Context->HandleSequence = 1; }
    return (Context->HandleSequence << 6) | Buffer;
}

static VOID VmxUpdateLink(VMX_CONTEXT *Context, ULONG Link)
{
    if (Link == MAXULONG) { Link = 0; }
    Context->Link = Link;
    Context->Shared->LinkSpeed = Link >> 16;
    Context->Shared->LinkDuplex = 2;
    *Context->Shared->LinkState = (UCHAR)(Link & 1);
}

#if VMX_FAULT_INJECT
/* Outcomes: 0 idle, 1 posted, 2 device queue error, 3 completed without
 * queue error, 4 timeout, 5 another recovery cause, 6 explicit test reset.
 * Only 2 qualifies as device queue-error evidence. */
static VOID VmxFaultRecoveryFinished(VMX_CONTEXT *Context, NTSTATUS Status)
{
    if (Context->FaultCount && Context->FaultRecoveryStage && !Context->FaultRecoveryDone) {
        Context->FaultRecoveryResult = Status;
        Context->FaultRecoveryDone = 1;
    }
}
#endif

static BOOLEAN VmxRecover(VMX_CONTEXT *Context, ULONG Stage, ULONG Detail)
{
    USHORT Command, Readback;
    ULONG Index, Result;
    volatile VMX_QUEUES *Queues = Context->Region[1].Va;

#if VMX_FAULT_INJECT
    if (Context->FaultCount && !Context->FaultRecoveryStage && Context->FaultOutcome != 3) {
        Context->FaultRecoveryStage = Stage;
        if (Context->FaultOutcome == 1) {
            Context->FaultOutcome = Stage == VmxTxTimeout ? 4 : 5;
        }
    }
#endif

    Context->LastStage = Stage; Context->LastDetail = Detail;
    Context->Result = VmxStatus(Stage, Detail);
    Context->Running = FALSE;
    VmxUpdateLink(Context, 0);
    if (!VmxStopDevice(Context)) {
        Context->Result = VmxStatus(VmxCleanup, Stage);
#if VMX_FAULT_INJECT
        VmxFaultRecoveryFinished(Context, Context->Result);
#endif
        return FALSE;
    }
    /* Buffer bytes, held RX/TX ownership and handle tokens survive this reset.
     * Only device-owned TX packets are discarded; no retained RX is overwritten. */
    Command = (Context->OriginalCommand | VMX_PCI_MEMORY | VMX_PCI_INTX_DISABLE) &
              (USHORT)~VMX_PCI_MASTER;
    Context->PciTouched = TRUE;
    if (KdxWritePci(Context->Device, 4, &Command, 2) != 2 ||
        KdxReadPci(Context->Device, 4, &Readback, 2) != 2 || Readback != Command) {
        goto Failed;
    }
    Context->MmioReady = TRUE;
    VmxWrite(Context->Bar1, VMX_REG_VRRS, 1);
    VmxWrite(Context->Bar1, VMX_REG_UVRS, 1);
    (VOID)VmxCommand(Context, VMX_CMD_RESET);
    for (Index = 0; Index < VMX_MAX_INTRS; ++Index) {
        VmxWrite(Context->Bar0, VMX_REG_IMR + Index * 8, 1);
    }
    memset(Context->Region[0].Va, 0, 8 * VMX_DMA_PAGE);
    VmxPrepareRings(Context);
    VmxConfigure(Context);
    VmxWrite(Context->Bar1, VMX_REG_DSAL, (ULONG)Context->Region[0].Pa);
    VmxWrite(Context->Bar1, VMX_REG_DSAH, (ULONG)(Context->Region[0].Pa >> 32));
    Command |= VMX_PCI_MASTER;
    if (KdxWritePci(Context->Device, 4, &Command, 2) != 2 ||
        KdxReadPci(Context->Device, 4, &Readback, 2) != 2 || Readback != Command) {
        goto Failed;
    }
    Result = VmxCommand(Context, VMX_CMD_ACTIVATE);
    if (Result != 0) { goto Failed; }
    VmxPostReceive(Context);
    (VOID)VmxCommand(Context, VMX_CMD_RX_MODE);
    (VOID)VmxCommand(Context, VMX_CMD_MAC_FILTER);
    (VOID)VmxCommand(Context, VMX_CMD_VLAN);
    (VOID)VmxCommand(Context, VMX_CMD_QUEUE);
    VmxBarrier(); /* Device status writes precede CPU inspection. */
    if (Queues->Tx.Status.Stopped || Queues->Rx.Status.Stopped ||
        Queues->Tx.Status.Error || Queues->Rx.Status.Error) { goto Failed; }
    VmxUpdateLink(Context, VmxCommand(Context, VMX_CMD_LINK));
    Context->Running = TRUE;
    ++Context->Restarts;
#if VMX_FAULT_INJECT
    VmxFaultRecoveryFinished(Context, STATUS_SUCCESS);
#endif
    /* RestartKdnet remains untouched: its meaning has not been established. */
    return TRUE;
Failed:
    if (!VmxStopDevice(Context)) {
        Context->Result = VmxStatus(VmxCleanup, VmxRecovery);
    } else {
        Context->Result = VmxStatus(VmxRecovery, Stage);
    }
#if VMX_FAULT_INJECT
    VmxFaultRecoveryFinished(Context, Context->Result);
#endif
    return FALSE;
}

static BOOLEAN VmxService(VMX_CONTEXT *Context, BOOLEAN Force)
{
    ULONG Events;
    volatile VMX_QUEUES *Queues = Context->Region[1].Va;
    if (!Context->Running) { return FALSE; }
#if VMX_FAULT_INJECT
    /* Do not let the periodic event poll lose the injected fault to the TX
     * watchdog. Hardware still supplies ECR and GET_QUEUE_STATUS evidence. */
    if (Context->FaultOutcome == 1) { Force = TRUE; }
#endif
    if (!Force && (++Context->ServiceCalls & 63) != 0) { return TRUE; }
    Events = KdxRead32(Context->Bar1, VMX_REG_ECR);
    if ((Events & VMX_QUEUE_ERRORS) != 0) {
        (VOID)VmxCommand(Context, VMX_CMD_QUEUE);
        VmxBarrier();
        Context->LastTxError = Queues->Tx.Status.Error;
        Context->LastRxError = Queues->Rx.Status.Error;
#if VMX_FAULT_INJECT
        if (Context->FaultOutcome == 1) {
            Context->FaultEvents = Events;
            Context->FaultTxError = Queues->Tx.Status.Error;
            Context->FaultRxError = Queues->Rx.Status.Error;
            Context->FaultTxStopped = Queues->Tx.Status.Stopped;
            Context->FaultRxStopped = Queues->Rx.Status.Stopped;
            Context->FaultOutcome = 2;
        }
#endif
        (VOID)VmxRecover(Context, VmxQueues, Events);
        return FALSE;
    }
    if ((Events & VMX_EVENT_LINK) != 0 || (Context->Link & 1) == 0) {
        VmxUpdateLink(Context, VmxCommand(Context, VMX_CMD_LINK));
        if (Events & VMX_EVENT_LINK) { VmxWrite(Context->Bar1, VMX_REG_ECR, VMX_EVENT_LINK); }
    }
    return TRUE;
}

static BOOLEAN VmxCollectTx(VMX_CONTEXT *Context)
{
    ULONG Budget;
    volatile VMX_COMPLETION *Ring = Context->Region[4].Va;
    for (Budget = 0; Budget < VMX_RING_COUNT; ++Budget) {
        volatile VMX_COMPLETION *Completion = &Ring[Context->TxComp];
        ULONG Last = Completion->Word3, Index, Count, Buffer;
        if ((Last >> 31) != Context->TxCompGen) { break; }
        VmxBarrier(); /* Completion ownership before the device-written index. */
        Index = Completion->Word0 & 0xFFF;
        Count = (Index - Context->TxNext) & (VMX_RING_COUNT - 1);
        if (VMX_COMP_TYPE(Last) != 0 || Index >= VMX_RING_COUNT || Count >= Context->TxUsed) {
            (VOID)VmxRecover(Context, VmxTxCompletion, Index);
            return FALSE;
        }
        /* Completion of this EOP also retires preceding descriptors in order. */
        do {
            Buffer = Context->TxSlot[Context->TxNext];
            if (Buffer >= VMX_RING_COUNT || Context->TxState[Buffer] != VmxBufferPosted) {
                (VOID)VmxRecover(Context, VmxTxCompletion, 0x100 | Buffer);
                return FALSE;
            }
            Context->TxState[Buffer] = VmxBufferFree;
#if VMX_FAULT_INJECT
            if (Context->FaultOutcome == 1 && Context->TxNext == Context->FaultLastSlot) {
                Context->FaultOutcome = 3; /* NIC accepted the chain: inconclusive. */
            }
#endif
            Context->TxSlot[Context->TxNext] = VMX_INVALID_SLOT;
            Context->TxNext = (Context->TxNext + 1) & (VMX_RING_COUNT - 1);
            --Context->TxUsed; --Context->TxOwned; ++Context->TxPackets;
        } while (Count-- != 0);
        if (++Context->TxComp == VMX_RING_COUNT) {
            Context->TxComp = 0; Context->TxCompGen ^= 1;
        }
    }
    /* Async-only traffic must also recover if the NIC stops completing sends.
     * This check never waits, and receive polling services it as well. */
    if (Context->TxUsed && KdxCycles(NULL) - Context->TxStarted[Context->TxNext] >=
        Context->CycleHz / 10) {
        ++Context->TxTimeouts;
        (VOID)VmxRecover(Context, VmxTxTimeout, Context->TxNext);
        return FALSE;
    }
    return TRUE;
}

static NTSTATUS VmxWaitTx(VMX_CONTEXT *Context, ULONG Buffer)
{
    ULONG Poll;
    ULONG64 Start = KdxCycles(NULL), Limit = Context->CycleHz / 10;
    if (Limit == 0) { Limit = 1; }
    for (Poll = 0; Poll < VMX_TX_POLLS; ++Poll) {
        if (!VmxService(Context, FALSE) || !VmxCollectTx(Context)) { return STATUS_IO_TIMEOUT; }
        if (Buffer == MAXULONG ? Context->TxUsed == 0 : Context->TxState[Buffer] == VmxBufferFree) {
            return STATUS_SUCCESS;
        }
        if (KdxCycles(NULL) - Start >= Limit) { break; }
        KdxStall(10);
    }
    ++Context->TxTimeouts;
    if (Buffer != MAXULONG) {
        (VOID)VmxRecover(Context, VmxTxTimeout, Buffer);
    } else {
        Context->LastStage = VmxTxTimeout; Context->LastDetail = Context->TxUsed;
        Context->Result = VmxStatus(VmxTxTimeout, Context->TxUsed);
    }
    return STATUS_IO_TIMEOUT;
}

static VOID NTAPI VmxShutdownController(PVOID Adapter)
{
    VMX_CONTEXT *Context = VmxGetContext(Adapter);
    if (Context == NULL) { return; }
    if (Context->Running && Context->TxUsed) { (VOID)VmxWaitTx(Context, MAXULONG); }
    if (!VmxStopDevice(Context)) { Context->Result = VmxStatus(VmxCleanup, 0); }
    VmxUpdateLink(Context, 0);
}

static NTSTATUS NTAPI VmxGetTx(PVOID Adapter, PULONG Handle)
{
    VMX_CONTEXT *Context = VmxGetContext(Adapter);
    ULONG Buffer;
    if (Handle == NULL) { return STATUS_INVALID_PARAMETER; }
    *Handle = 0;
    if (Context == NULL) { return STATUS_INVALID_PARAMETER; }
    if (!VmxService(Context, FALSE) || !VmxCollectTx(Context) || (Context->Link & 1) == 0) {
        return STATUS_IO_TIMEOUT;
    }
    if (Context->TxOwned >= VMX_RING_COUNT - 1) { return STATUS_IO_TIMEOUT; }
    for (Buffer = 0; Buffer < VMX_RING_COUNT; ++Buffer) {
        if (Context->TxState[Buffer] == VmxBufferFree) {
            Context->TxState[Buffer] = VmxBufferHeld;
            Context->TxToken[Buffer] = VmxNewHandle(Context, Buffer);
            ++Context->TxOwned;
            *Handle = TRANSMIT_HANDLE | Context->TxToken[Buffer];
            return STATUS_SUCCESS;
        }
    }
    return STATUS_IO_TIMEOUT;
}

#if VMX_FAULT_INJECT
static BOOLEAN VmxFaultSendChain(VMX_CONTEXT *Context, ULONG Buffer, ULONG WireLength)
{
    ULONG Selected[VMX_FAULT_DESCRIPTORS], Count = 1, Index, Slot, Length, Offset = 0;
    ULONG First = Context->TxFill, FirstGen = Context->TxGen, Gen = FirstGen;
    ULONG64 Started = KdxCycles(NULL);
    volatile VMX_DESC *Ring = Context->Region[2].Va;
    /* Reserve distinct buffers for all fragments so even an accepted chain
     * uses the normal completion/ownership bookkeeping. Do not displace any
     * caller-held reservation or fill beyond the usual 31-entry limit. */
    if (Context->TxUsed || Context->TxOwned > VMX_RING_COUNT - VMX_FAULT_DESCRIPTORS) {
        return FALSE;
    }
    Selected[0] = Buffer;
    for (Index = 0; Index < VMX_RING_COUNT && Count < VMX_FAULT_DESCRIPTORS; ++Index) {
        if (Context->TxState[Index] == VmxBufferFree) { Selected[Count++] = Index; }
    }
    if (Count != VMX_FAULT_DESCRIPTORS) { return FALSE; }
    for (Index = 0; Index < Count; ++Index) {
        if (Context->TxSlot[(First + Index) & (VMX_RING_COUNT - 1)] != VMX_INVALID_SLOT) {
            return FALSE;
        }
    }
    VmxFaultArm = 0;
    Context->FaultCount = 1; Context->FaultOutcome = 1;
    Context->FaultSlot = First; Context->FaultBuffer = Buffer;
    Context->FaultDescriptors = Count;
    Context->FaultLastSlot = (First + Count - 1) & (VMX_RING_COUNT - 1);
    Context->FaultRestartsBefore = Context->Restarts;
    Context->FaultWord2 = (WireLength - (Count - 1)) | (FirstGen ? VMX_TX_GEN : 0);
    Context->FaultWord3 = 0; /* EOP/CQ appear only on the final fragment. */
    Slot = First;
    for (Index = 0; Index < Count; ++Index) {
        ULONG Fragment = Selected[Index];
        Length = Index == 0 ? WireLength - (Count - 1) : 1;
        if (Index != 0) {
            memcpy(Context->Buffers + Fragment * VMX_BUFFER_SIZE,
                   Context->Buffers + Buffer * VMX_BUFFER_SIZE + Offset, Length);
        }
        Ring[Slot].Word2 = Length | (Gen ? 0 : VMX_TX_GEN);
        Ring[Slot].Address = Context->BufferPA[Fragment];
        Ring[Slot].Word3 = Index == Count - 1 ? VMX_TX_EOP_CQ : 0;
        Context->TxSlot[Slot] = (UCHAR)Fragment;
        Context->TxState[Fragment] = VmxBufferPosted;
        Context->TxStarted[Slot] = Started;
        Offset += Length;
        if (++Slot == VMX_RING_COUNT) { Slot = 0; Gen ^= 1; }
    }
    Context->TxFill = Slot; Context->TxGen = Gen;
    Context->TxUsed = Count; Context->TxOwned += Count - 1;
    VmxBarrier();
    /* Publish the first descriptor last so a polling NIC cannot consume a
     * partially constructed packet. All fragments reference valid DMA buffers. */
    for (Index = 1; Index < Count; ++Index) {
        Ring[(First + Index) & (VMX_RING_COUNT - 1)].Word2 ^= VMX_TX_GEN;
    }
    VmxBarrier();
    Ring[First].Word2 ^= VMX_TX_GEN;
    VmxWrite(Context->Bar0, VMX_REG_TXPROD, Context->TxFill);
    return TRUE;
}
#endif

static NTSTATUS NTAPI VmxSendTx(PVOID Adapter, ULONG Handle, ULONG Length)
{
    VMX_CONTEXT *Context = VmxGetContext(Adapter);
    ULONG Buffer = Handle & VMX_HANDLE_INDEX, Token = Handle & ~HANDLE_FLAGS;
    ULONG Slot, WireLength;
    volatile VMX_DESC *Descriptor;
    if (Context == NULL || !(Handle & TRANSMIT_HANDLE) || Buffer >= VMX_RING_COUNT ||
        Context->TxState[Buffer] != VmxBufferHeld || Context->TxToken[Buffer] != Token) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Length < 14 || Length > VMX_FRAME_MAX) {
        Context->TxState[Buffer] = VmxBufferFree; --Context->TxOwned;
        return STATUS_INVALID_PARAMETER;
    }
    if (!VmxService(Context, FALSE) || !VmxCollectTx(Context) || (Context->Link & 1) == 0) {
        Context->TxState[Buffer] = VmxBufferFree; --Context->TxOwned;
        return STATUS_IO_TIMEOUT;
    }
#if VMX_FAULT_INJECT
    /* Exercise the real stop/reset/reinitialization routine without relying
     * on VMware rejecting a descriptor. No device event/status is fabricated.
     * Drop this caller's unsent packet and report the same transient failure
     * as a recovery initiated by VmxService; other held buffers survive. */
    if (VmxFaultArm == VMX_RESET_ARM && !Context->FaultCount && (Handle & TRANSMIT_ASYNC)) {
        VmxFaultArm = 0;
        Context->FaultCount = 1; Context->FaultOutcome = 6;
        Context->FaultRestartsBefore = Context->Restarts;
        (VOID)VmxRecover(Context, VMX_TEST_RESET_STAGE, 1);
        Context->TxState[Buffer] = VmxBufferFree; --Context->TxOwned;
        return STATUS_IO_TIMEOUT;
    }
#endif
    Slot = Context->TxFill;
    if (Context->TxUsed >= VMX_RING_COUNT - 1 || Context->TxSlot[Slot] != VMX_INVALID_SLOT) {
        Context->TxState[Buffer] = VmxBufferFree; --Context->TxOwned;
        (VOID)VmxRecover(Context, VmxTxCompletion, 0x200 | Slot);
        return STATUS_IO_TIMEOUT;
    }
    WireLength = Length < 60 ? 60 : Length;
    if (WireLength != Length) {
        memset(Context->Buffers + Buffer * VMX_BUFFER_SIZE + Length, 0, WireLength - Length);
    }
#if VMX_FAULT_INJECT
    /* 17 descriptors exceed the non-TSO limit of 16 in the BSD rev-1
     * definitions. Hardware alone supplies the event/status response. */
    if (VmxFaultArm == VMX_FAULT_ARM && !Context->FaultCount && (Handle & TRANSMIT_ASYNC) &&
        VmxFaultSendChain(Context, Buffer, WireLength)) {
        return STATUS_SUCCESS;
    }
#endif
    Descriptor = (volatile VMX_DESC *)Context->Region[2].Va + Slot;
    Descriptor->Word2 = WireLength | (Context->TxGen ? 0 : VMX_TX_GEN);
    Descriptor->Address = Context->BufferPA[Buffer];
    Descriptor->Word3 = VMX_TX_EOP_CQ;
    Context->TxSlot[Slot] = (UCHAR)Buffer;
    Context->TxState[Buffer] = VmxBufferPosted;
    Context->TxStarted[Slot] = KdxCycles(NULL);
    ++Context->TxUsed;
    VmxBarrier(); /* Packet payload and descriptor before generation ownership. */
    Descriptor->Word2 = WireLength | (Context->TxGen ? VMX_TX_GEN : 0);
    if (++Context->TxFill == VMX_RING_COUNT) { Context->TxFill = 0; Context->TxGen ^= 1; }
    VmxWrite(Context->Bar0, VMX_REG_TXPROD, Context->TxFill);
    if (Handle & TRANSMIT_ASYNC) { return STATUS_SUCCESS; }
    return VmxWaitTx(Context, Buffer);
}

static NTSTATUS NTAPI VmxGetRx(PVOID Adapter, PULONG Handle, PVOID *Packet, PULONG Length)
{
    VMX_CONTEXT *Context = VmxGetContext(Adapter);
    ULONG Budget;
    volatile VMX_COMPLETION *Ring;
    if (Handle == NULL || Packet == NULL || Length == NULL) { return STATUS_INVALID_PARAMETER; }
    *Handle = 0; *Packet = NULL; *Length = 0;
    if (Context == NULL) { return STATUS_INVALID_PARAMETER; }
    if (!VmxService(Context, FALSE) || !VmxCollectTx(Context)) { return STATUS_IO_TIMEOUT; }
    Ring = Context->Region[7].Va;
    /* A bounded scan can discard bad frames without waiting for new traffic. */
    for (Budget = 0; Budget < 2 * VMX_RING_COUNT; ++Budget) {
        volatile VMX_COMPLETION *Completion = &Ring[Context->RxComp];
        ULONG Last = Completion->Word3, First, Data, Index, RxRing, Count, Buffer = 0;
        if ((Last >> 31) != Context->RxCompGen) { break; }
        VmxBarrier(); /* Ownership before completion fields and packet payload. */
        First = Completion->Word0; Data = Completion->Word2;
        Index = First & 0xFFF; RxRing = (First >> 16) & 0x3FF;
        if (VMX_COMP_TYPE(Last) != 3 || Index >= VMX_RING_COUNT || RxRing >= 2) {
            (VOID)VmxRecover(Context, VmxRxCompletion, 1);
            return STATUS_IO_TIMEOUT;
        }
        Count = (Index - Context->RxNext[RxRing]) & (VMX_RING_COUNT - 1);
        if (Count >= Context->RxPosted[RxRing]) {
            (VOID)VmxRecover(Context, VmxRxCompletion, 2);
            return STATUS_IO_TIMEOUT;
        }
        /* vmxnet3 may skip descriptors; recycle skipped buffers too. Within
         * each RX ring, completion indices advance in device consumption order. */
        do {
            ULONG Slot = Context->RxNext[RxRing];
            Buffer = Context->RxSlot[RxRing][Slot];
            if (Buffer >= 2 * VMX_RING_COUNT || Context->RxState[Buffer] != VmxBufferPosted) {
                (VOID)VmxRecover(Context, VmxRxCompletion, 3);
                return STATUS_IO_TIMEOUT;
            }
            Context->RxSlot[RxRing][Slot] = VMX_INVALID_SLOT;
            Context->RxState[Buffer] = VmxBufferFree;
            Context->RxNext[RxRing] = (Slot + 1) & (VMX_RING_COUNT - 1);
            --Context->RxPosted[RxRing];
            if (Count != 0) { (VOID)VmxRecycleRx(Context, Buffer); ++Context->RxDropped; }
        } while (Count-- != 0);
        if (++Context->RxComp == 2 * VMX_RING_COUNT) { Context->RxComp = 0; Context->RxCompGen ^= 1; }
        if (RxRing != 0 || (First & VMX_RX_SOP_EOP) != VMX_RX_SOP_EOP ||
            (Data & VMX_RX_ERROR) || (Data & 0x3FFF) < 14 || (Data & 0x3FFF) > VMX_FRAME_MAX) {
            ++Context->RxDropped;
            (VOID)VmxRecycleRx(Context, Buffer);
            continue;
        }
        Context->RxState[Buffer] = VmxBufferHeld;
        Context->RxLength[Buffer] = Data & 0x3FFF;
        Context->RxToken[Buffer] = VmxNewHandle(Context, Buffer);
        *Handle = Context->RxToken[Buffer];
        *Packet = Context->Buffers + (VMX_RING_COUNT + Buffer) * VMX_BUFFER_SIZE;
        *Length = Context->RxLength[Buffer];
        ++Context->RxPackets;
        return STATUS_SUCCESS;
    }
    return STATUS_IO_TIMEOUT;
}

static VOID NTAPI VmxReleaseRx(PVOID Adapter, ULONG Handle)
{
    VMX_CONTEXT *Context = VmxGetContext(Adapter);
    ULONG Buffer = Handle & VMX_HANDLE_INDEX;
    if (Context == NULL || (Handle & HANDLE_FLAGS) ||
        Context->RxState[Buffer] != VmxBufferHeld || Context->RxToken[Buffer] != Handle) { return; }
    Context->RxState[Buffer] = VmxBufferFree;
    Context->RxLength[Buffer] = 0;
    if (Context->Running) { (VOID)VmxRecycleRx(Context, Buffer); }
}

static PVOID NTAPI VmxPacketAddress(PVOID Adapter, ULONG Handle)
{
    VMX_CONTEXT *Context = VmxGetContext(Adapter);
    ULONG Buffer = Handle & VMX_HANDLE_INDEX, Token = Handle & ~HANDLE_FLAGS;
    if (Context == NULL) { return NULL; }
    if (Handle & TRANSMIT_HANDLE) {
        if (Buffer >= VMX_RING_COUNT || Context->TxState[Buffer] != VmxBufferHeld ||
            Context->TxToken[Buffer] != Token) { return NULL; }
    } else {
        if ((Handle & HANDLE_FLAGS) || Context->RxState[Buffer] != VmxBufferHeld ||
            Context->RxToken[Buffer] != Token) { return NULL; }
        Buffer += VMX_RING_COUNT;
    }
    return Context->Buffers + Buffer * VMX_BUFFER_SIZE;
}

static ULONG NTAPI VmxPacketLength(PVOID Adapter, ULONG Handle)
{
    VMX_CONTEXT *Context = VmxGetContext(Adapter);
    if (VmxPacketAddress(Adapter, Handle) == NULL) { return 0; }
    return (Handle & TRANSMIT_HANDLE) ? VMX_BUFFER_SIZE : Context->RxLength[Handle & VMX_HANDLE_INDEX];
}

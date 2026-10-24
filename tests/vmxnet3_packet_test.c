/* Packet tests share the initialization fixture, but emulate device ring
 * consumers/producers independently of the driver's bookkeeping. */
#define VMX_PACKET_TEST 1
#define VMX_TRANSPORT 1
#include "vmxnet3_init_test.c"

static ULONG DevTx, DevTxGen, DevTc, DevTcGen, DevRx[2], DevRxGen[2], DevRc, DevRcGen;
static ULONG TxDoorbell, WirePackets, LastWireLength, StallCalls;
static ULONG DevPacketLength, DevFragments;
static UCHAR LastWire[2048];
static BOOLEAN AutoTx, StallTx;
#if VMX_FAULT_INJECT
static ULONG FaultBehavior, FaultSeen;
static BOOLEAN DevTxStopped;
#endif

static PUCHAR DmaAddress(ULONG64 Pa)
{
    ULONG64 Delta = Pa - 0x100000000ULL;
    ULONG64 Offset = (Delta / 8192) * 4096 + Delta % 8192;
    CHECK(Delta % 8192 < 4096 && Offset < sizeof(Memory));
    return Memory + (ULONG)Offset;
}

static VOID DeviceTransmit(VOID)
{
    VMX_CONTEXT *Context = Shared.Hardware;
    VMX_DESC *Ring = Context->Region[2].Va;
    VMX_COMPLETION *Completions = Context->Region[4].Va;
    ULONG Budget;
#if VMX_FAULT_INJECT
    if (DevTxStopped) { return; }
#endif
    for (Budget = 0; DevTx != TxDoorbell && Budget < 32; ++Budget) {
        VMX_DESC *Desc = &Ring[DevTx];
        ULONG Length = Desc->Word2 & 0x3FFF;
        CHECK(((Desc->Word2 >> 14) & 1) == DevTxGen);
        CHECK(Desc->Word3 == 0 || Desc->Word3 == 0x3000);
        CHECK(Length > 0 && Length <= 1514 && DevPacketLength + Length <= 1514);
        CHECK((Desc->Word2 & ~0x7FFFUL) == 0);
        ++DevFragments;
#if VMX_FAULT_INJECT
        if (DevFragments == 17) {
            /* Independently count non-TSO packet fragments. Real VMware
             * enforcement of the rev-1 limit still requires a target test. */
            VMX_QUEUES *Queues = Context->Region[1].Va;
            (VOID)DmaAddress(Desc->Address);
            ++FaultSeen;
            if (FaultBehavior != 1) {
                DevTxStopped = TRUE;
                if (FaultBehavior == 0) {
                    Queues->Tx.Status.Stopped = 1;
                    Queues->Tx.Status.Error = 0x80000002;
                    Bar1[VMX_REG_ECR / 4] |= 1;
                }
                return;
            }
        }
#endif
        memcpy(LastWire + DevPacketLength, DmaAddress(Desc->Address), Length);
        DevPacketLength += Length;
        if (Desc->Word3 & 0x1000) {
            CHECK(DevPacketLength >= 60 && (Desc->Word3 & 0x2000));
            LastWireLength = DevPacketLength;
            ++WirePackets;
            Completions[DevTc].Word0 = DevTx;
            Completions[DevTc].Word3 = DevTcGen << 31;
            if (++DevTc == 32) { DevTc = 0; DevTcGen ^= 1; }
            DevPacketLength = DevFragments = 0;
        }
        if (++DevTx == 32) { DevTx = 0; DevTxGen ^= 1; }
    }
}

static VOID DeviceWrite(PULONG Register, ULONG Value)
{
    if (Register == &Bar1[VMX_REG_CMD / 4] && Value == VMX_CMD_RESET) {
        DevTx = DevTc = DevRx[0] = DevRx[1] = DevRc = TxDoorbell = 0;
        DevTxGen = DevTcGen = DevRxGen[0] = DevRxGen[1] = DevRcGen = 1;
        DevPacketLength = DevFragments = 0;
#if VMX_FAULT_INJECT
        DevTxStopped = FALSE;
#endif
    }
    if (Register == &Bar0[VMX_REG_TXPROD / 4]) {
        TxDoorbell = Value;
        if (AutoTx) { DeviceTransmit(); }
    }
}

static VOID DeviceStall(VOID)
{
    ++StallCalls;
    if (StallTx) { DeviceTransmit(); }
}

static VOID Start(VOID)
{
    Setup(); ExtraWrite = DeviceWrite; ExtraStall = DeviceStall;
    AutoTx = TRUE; StallTx = FALSE; WirePackets = StallCalls = 0;
#if VMX_FAULT_INJECT
    FaultBehavior = FaultSeen = 0;
#endif
    CHECK(Exports.KdInitializeController(&Shared) == STATUS_SUCCESS);
    CHECK(Active && ((VMX_CONTEXT *)Shared.Hardware)->Running);
    StallCalls = 0;
}

static VOID Stop(VOID)
{
    Exports.KdShutdownController(&Shared);
    CHECK(!Active && !(PciCommand & 4));
    CheckGuards();
}

static ULONG AcquireTx(UCHAR Fill)
{
    ULONG Handle = 0;
    PVOID Packet;
    CHECK(Exports.KdGetTxPacket(&Shared, &Handle) == STATUS_SUCCESS);
    Packet = Exports.KdGetPacketAddress(Shared.Hardware, Handle);
    CHECK(Packet != NULL && Exports.KdGetPacketLength(&Shared, Handle) >= 1514);
    if (Packet != NULL) { memset(Packet, Fill, 1514); }
    return Handle;
}

static VOID InjectRx(ULONG Ring, ULONG Skip, ULONG Length, ULONG Flags, UCHAR Fill)
{
    VMX_CONTEXT *Context = Shared.Hardware;
    VMX_DESC *Descriptors = Context->Region[5 + Ring].Va;
    VMX_COMPLETION *Completions = Context->Region[7].Va;
    ULONG Index;
    do {
        VMX_DESC *Desc = &Descriptors[DevRx[Ring]];
        CHECK((Desc->Word2 >> 31) == DevRxGen[Ring]);
        CHECK((Desc->Word2 & 0x3FFF) == 2048);
        CHECK(((Desc->Word2 >> 14) & 1) == Ring);
        Index = DevRx[Ring];
        if (Skip == 0) { memset(DmaAddress(Desc->Address), Fill, Length & 0x3FFF); }
        if (++DevRx[Ring] == 32) { DevRx[Ring] = 0; DevRxGen[Ring] ^= 1; }
    } while (Skip-- != 0);
    Completions[DevRc].Word0 = Index | (Ring << 16) | Flags;
    Completions[DevRc].Word2 = Length;
    Completions[DevRc].Word3 = (DevRcGen << 31) | 0x03000000;
    if (++DevRc == 64) { DevRc = 0; DevRcGen ^= 1; }
}

static ULONG Receive(UCHAR Fill, ULONG ExpectedLength)
{
    ULONG Handle = 0, Length = 0, I;
    PVOID Packet = NULL;
    CHECK(Exports.KdGetRxPacket(&Shared, &Handle, &Packet, &Length) == STATUS_SUCCESS);
    CHECK(Packet != NULL && Length == ExpectedLength);
    CHECK(Exports.KdGetPacketAddress(Shared.Hardware, Handle) == Packet);
    CHECK(Exports.KdGetPacketLength(&Shared, Handle) == ExpectedLength);
    if (Packet != NULL) {
        for (I = 0; I < Length; ++I) { CHECK(((PUCHAR)Packet)[I] == Fill); }
    }
    return Handle;
}

static VOID EmptyRx(VOID)
{
    ULONG Handle = 1, Length = 1, Before = StallCalls;
    PVOID Packet = Memory;
    CHECK(Exports.KdGetRxPacket(&Shared, &Handle, &Packet, &Length) == STATUS_IO_TIMEOUT);
    CHECK(Handle == 0 && Packet == NULL && Length == 0 && StallCalls == Before);
}

#if VMX_FAULT_INJECT
static VOID ResetTests(VOID)
{
    VMX_CONTEXT *Context;
    ULONG Handle, HeldTx, HeldRx, I, Mode;
    PUCHAR Packet;
    Start(); Context = Shared.Hardware;
    VmxFaultArm = VMX_RESET_ARM;
    Handle = AcquireTx(1);
    CHECK(Exports.KdSendTxPacket(&Shared, Handle, 60) == STATUS_SUCCESS);
    CHECK(Context->FaultCount == 0 && VmxFaultArm == VMX_RESET_ARM);
    Handle = AcquireTx(2);
    CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 10) == STATUS_INVALID_PARAMETER);
    CHECK(Context->FaultCount == 0 && VmxFaultArm == VMX_RESET_ARM);
    VmxFaultArm = 0;
    InjectRx(0, 0, 60, 0xC000, 0xB6); HeldRx = Receive(0xB6, 60);
    HeldTx = AcquireTx(0xD7);
    AutoTx = FALSE;
    Handle = AcquireTx(3);
    CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    Handle = AcquireTx(4);
    CHECK(Context->TxUsed == 1 && Context->TxOwned == 3);
    VmxFaultArm = VMX_RESET_ARM;
    CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_IO_TIMEOUT);
    CHECK(Context->FaultCount == 1 && Context->FaultOutcome == 6 && VmxFaultArm == 0);
    CHECK(Context->FaultRecoveryStage == VMX_TEST_RESET_STAGE && Context->FaultRecoveryDone == 1);
    CHECK(Context->FaultRecoveryResult == STATUS_SUCCESS && Context->FaultRestartsBefore == 0);
    CHECK(Context->Running && Context->Restarts == 1 && Context->Result == VmxStatus(VMX_TEST_RESET_STAGE, 1));
    CHECK(Context->FaultEvents == 0 && Context->FaultTxError == 0 && Context->FaultRxError == 0);
    CHECK(Context->FaultTxStopped == 0 && Context->FaultRxStopped == 0 && Context->FaultDescriptors == 0);
    CHECK(Context->TxTimeouts == 0 && Context->TxUsed == 0 && Context->TxOwned == 1 && FaultSeen == 0);
    CHECK(Exports.KdGetPacketAddress(&Shared, Handle) == NULL);
    Packet = Exports.KdGetPacketAddress(&Shared, HeldRx);
    CHECK(Packet != NULL && Packet[0] == 0xB6 && Packet[59] == 0xB6);
    Packet = Exports.KdGetPacketAddress(&Shared, HeldTx);
    CHECK(Packet != NULL && Packet[0] == 0xD7 && Packet[1513] == 0xD7);
    Exports.KdReleaseRxPacket(&Shared, HeldRx); AutoTx = TRUE;
    CHECK(Exports.KdSendTxPacket(&Shared, HeldTx, 60) == STATUS_SUCCESS);
    for (I = 0; I < 70; ++I) {
        VmxFaultArm = (I & 1) ? VMX_RESET_ARM : VMX_FAULT_ARM;
        Handle = AcquireTx((UCHAR)I);
        CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
        InjectRx(0, 0, 60, 0xC000, (UCHAR)I); Handle = Receive((UCHAR)I, 60);
        Exports.KdReleaseRxPacket(&Shared, Handle);
    }
    CHECK(Context->FaultCount == 1 && Context->FaultOutcome == 6 && Context->Restarts == 1 && FaultSeen == 0);
    Stop();
    CHECK(Exports.KdInitializeController(&Shared) == STATUS_SUCCESS);
    CHECK(Context->FaultCount == 0 && VmxFaultArm == 0); Stop();

    /* Failures must remain observable and must not fabricate a successful reset. */
    for (Mode = 0; Mode < 2; ++Mode) {
        Start(); Context = Shared.Hardware;
        Handle = AcquireTx(5); VmxFaultArm = VMX_RESET_ARM;
        if (Mode == 0) { ActivationError = 7; } else { CleanupFailure = TRUE; }
        CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_IO_TIMEOUT);
        CHECK(Context->FaultOutcome == 6 && Context->FaultRecoveryDone == 1);
        CHECK(Context->FaultRecoveryResult == VmxStatus(Mode == 0 ? VmxRecovery : VmxCleanup, VMX_TEST_RESET_STAGE));
        CHECK(!Context->Running && Context->Restarts == 0 && Context->TxOwned == 0);
        CHECK(Context->FaultEvents == 0 && Context->FaultTxError == 0);
        CleanupFailure = FALSE; Stop();
    }
}

static VOID FaultTests(VOID)
{
    VMX_CONTEXT *Context;
    ULONG Handle, HeldTx, HeldRx, I, J, Held[16];
    PUCHAR Packet;
    Start(); Context = Shared.Hardware;
    CHECK(VmxFaultArm == 0 && Context->FaultCount == 0);
    Handle = AcquireTx(1);
    CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    EmptyRx(); CHECK(FaultSeen == 0);
    VmxFaultArm = 1; /* Wrong opt-in value must not trigger. */
    Handle = AcquireTx(2);
    CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    EmptyRx(); CHECK(FaultSeen == 0);
    VmxFaultArm = VMX_FAULT_ARM;
    Handle = AcquireTx(3);
    CHECK(Exports.KdSendTxPacket(&Shared, Handle, 60) == STATUS_SUCCESS);
    CHECK(FaultSeen == 0 && VmxFaultArm == VMX_FAULT_ARM); /* Sync debugger traffic stays normal. */
    InjectRx(0, 0, 60, 0xC000, 0xB6); HeldRx = Receive(0xB6, 60);
    HeldTx = AcquireTx(0xD7);
    Handle = AcquireTx(4);
    CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    CHECK(Context->FaultCount == 1 && Context->FaultOutcome == 1 && VmxFaultArm == 0);
    CHECK((Context->FaultWord2 & 0x3FFF) == 44 && Context->FaultWord3 == 0);
    CHECK(Context->FaultDescriptors == 17 && Context->FaultLastSlot == 19);
    CHECK(Context->FaultBuffer == (Handle & 0x3F) && Context->FaultRestartsBefore == 0);
    EmptyRx(); /* Forced polling catches the actual simulated queue error. */
    CHECK(Context->FaultOutcome == 2 && FaultSeen == 1 && Context->Running);
    CHECK(Context->FaultEvents == 1 && Context->FaultTxError == 0x80000002);
    CHECK(Context->FaultTxStopped == 1 && Context->FaultRxError == 0 && Context->FaultRxStopped == 0);
    CHECK(Context->FaultRecoveryStage == VmxQueues && Context->FaultRecoveryDone == 1);
    CHECK(Context->FaultRecoveryResult == STATUS_SUCCESS && Context->Restarts == 1);
    CHECK(Context->Result == VmxStatus(VmxQueues, 1) && Context->TxTimeouts == 0);
    Packet = Exports.KdGetPacketAddress(&Shared, HeldRx);
    CHECK(Packet != NULL && Packet[0] == 0xB6 && Packet[59] == 0xB6);
    Packet = Exports.KdGetPacketAddress(&Shared, HeldTx);
    CHECK(Packet != NULL && Packet[0] == 0xD7 && Packet[1513] == 0xD7);
    Exports.KdReleaseRxPacket(&Shared, HeldRx);
    CHECK(Exports.KdSendTxPacket(&Shared, HeldTx, 60) == STATUS_SUCCESS);
    VmxFaultArm = VMX_FAULT_ARM; /* Re-arming cannot inject twice after reset. */
    for (I = 0; I < 70; ++I) {
        Handle = AcquireTx((UCHAR)I);
        CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
        InjectRx(0, 0, 60, 0xC000, (UCHAR)I); Handle = Receive((UCHAR)I, 60);
        Exports.KdReleaseRxPacket(&Shared, Handle);
    }
    CHECK(FaultSeen == 1 && Context->FaultCount == 1 && Context->Restarts == 1);
    Stop();
    CHECK(Exports.KdInitializeController(&Shared) == STATUS_SUCCESS);
    CHECK(Context->FaultCount == 0 && VmxFaultArm == 0); Stop();

    Start(); Context = Shared.Hardware; FaultBehavior = 1; VmxFaultArm = VMX_FAULT_ARM;
    Handle = AcquireTx(5);
    CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    EmptyRx();
    CHECK(Context->FaultOutcome == 3 && Context->Restarts == 0 && Context->FaultRecoveryDone == 0);
    CHECK(Context->TxUsed == 0 && Context->TxOwned == 0 && Context->TxPackets == 17);
    CHECK(LastWireLength == 60);
    for (I = 0; I < 60; ++I) { CHECK(LastWire[I] == 5); }
    Stop(); /* An accepted chain is explicitly not a passing queue-fault test. */

    Start(); Context = Shared.Hardware; FaultBehavior = 2; VmxFaultArm = VMX_FAULT_ARM;
    Handle = AcquireTx(6);
    CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    Cycles += 100001; EmptyRx();
    CHECK(Context->FaultOutcome == 4 && Context->FaultEvents == 0 && Context->FaultTxError == 0);
    CHECK(Context->FaultRecoveryStage == VmxTxTimeout && Context->FaultRecoveryDone == 1);
    CHECK(Context->FaultRecoveryResult == STATUS_SUCCESS && Context->Restarts == 1);
    CHECK(Context->TxTimeouts == 1); Stop(); /* Timeout recovery also does not qualify. */

    Start(); Context = Shared.Hardware; VmxFaultArm = VMX_FAULT_ARM;
    Handle = AcquireTx(7);
    CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    ActivationError = 7; EmptyRx();
    CHECK(Context->FaultOutcome == 2 && Context->FaultRecoveryDone == 1);
    CHECK(!NT_SUCCESS(Context->FaultRecoveryResult) && !Context->Running && Context->Restarts == 0);
    CHECK(!Active && !(PciCommand & 4)); Stop();

    Start(); Context = Shared.Hardware; VmxFaultArm = VMX_FAULT_ARM;
    Handle = AcquireTx(8);
    CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    CleanupFailure = TRUE; EmptyRx();
    CHECK(Context->FaultOutcome == 2 && Context->FaultRecoveryDone == 1);
    CHECK(Context->FaultRecoveryResult == VmxStatus(VmxCleanup, VmxQueues));
    CHECK(!Context->Running && Context->Restarts == 0);
    CleanupFailure = FALSE; Stop();

    /* Accepted chains that cross the ring end must still reclaim all buffers
     * exactly once and reproduce both max-length and padded packet payloads. */
    for (J = 0; J < 3; ++J) {
        ULONG StartSlot = J == 0 ? 15 : (J == 1 ? 16 : 31);
        ULONG InputLength = J == 0 ? 1514 : 14;
        Start(); Context = Shared.Hardware; FaultBehavior = 1;
        for (I = 0; I < StartSlot; ++I) {
            Handle = AcquireTx(0); CHECK(Exports.KdSendTxPacket(&Shared, Handle, 60) == STATUS_SUCCESS);
        }
        Handle = AcquireTx(0xA5); Packet = Exports.KdGetPacketAddress(&Shared, Handle);
        for (I = 0; I < InputLength; ++I) { Packet[I] = (UCHAR)I; }
        VmxFaultArm = VMX_FAULT_ARM;
        CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, InputLength) == STATUS_SUCCESS);
        EmptyRx();
        CHECK(Context->FaultSlot == StartSlot && Context->FaultLastSlot == ((StartSlot + 16) & 31));
        CHECK(Context->FaultOutcome == 3 && Context->TxUsed == 0 && Context->TxOwned == 0);
        CHECK(Context->TxPackets == StartSlot + 17 && Context->TxTimeouts == 0);
        CHECK(LastWireLength == (InputLength < 60 ? 60 : InputLength));
        for (I = 0; I < LastWireLength; ++I) { CHECK(LastWire[I] == (I < InputLength ? (UCHAR)I : 0)); }
        Handle = AcquireTx(9); CHECK(Exports.KdSendTxPacket(&Shared, Handle, 60) == STATUS_SUCCESS);
        Stop();
    }
    /* Preserve 15 caller reservations: defer until 16 spare buffers exist. */
    Start(); Context = Shared.Hardware; FaultBehavior = 1;
    for (I = 0; I < 16; ++I) { Held[I] = AcquireTx((UCHAR)I); }
    VmxFaultArm = VMX_FAULT_ARM;
    CHECK(Exports.KdSendTxPacket(&Shared, Held[0] | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    EmptyRx(); CHECK(Context->FaultCount == 0 && VmxFaultArm == VMX_FAULT_ARM);
    CHECK(Exports.KdSendTxPacket(&Shared, Held[1] | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    EmptyRx(); CHECK(Context->FaultCount == 1 && Context->TxOwned == 14);
    for (I = 2; I < 16; ++I) {
        Packet = Exports.KdGetPacketAddress(&Shared, Held[I]);
        CHECK(Packet != NULL && Packet[59] == I);
        CHECK(Exports.KdSendTxPacket(&Shared, Held[I], 60) == STATUS_SUCCESS);
    }
    Stop();
    Start(); Context = Shared.Hardware; AutoTx = FALSE;
    Handle = AcquireTx(1); CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    VmxFaultArm = VMX_FAULT_ARM;
    Handle = AcquireTx(2); CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    CHECK(Context->FaultCount == 0 && VmxFaultArm == VMX_FAULT_ARM && Context->TxUsed == 2);
    DeviceTransmit(); EmptyRx(); AutoTx = TRUE;
    Handle = AcquireTx(3); CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    EmptyRx(); CHECK(Context->FaultOutcome == 2 && Context->Restarts == 1); Stop();
}
#endif

int main(VOID)
{
    ULONG I, Handle, Old, Held[31], Before;
    PVOID Packet;
    VMX_CONTEXT *Context;
    Start(); Context = Shared.Hardware;
    EmptyRx();
    CHECK(Exports.KdGetTxPacket(NULL, &Handle) == STATUS_INVALID_PARAMETER);
    CHECK(Exports.KdGetTxPacket(&Shared, NULL) == STATUS_INVALID_PARAMETER);
    CHECK(Exports.KdGetPacketAddress(&Shared, 0) == NULL);
    CHECK(Exports.KdSendTxPacket(&Shared, 0, 60) == STATUS_INVALID_PARAMETER);
    /* Repeated wraparound with independent completion generations. */
    for (I = 0; I < 257; ++I) {
        Handle = AcquireTx((UCHAR)I);
        CHECK(Exports.KdSendTxPacket(&Shared, Handle, 1514) == STATUS_SUCCESS);
        CHECK(LastWire[1513] == (UCHAR)I && LastWireLength == 1514);
        CHECK(Exports.KdGetPacketAddress(&Shared, Handle) == NULL);
        InjectRx(0, 0, 100, 0xC000, (UCHAR)I);
        Handle = Receive((UCHAR)I, 100);
        Exports.KdReleaseRxPacket(&Shared, Handle);
        CHECK(Exports.KdGetPacketAddress(&Shared, Handle) == NULL);
    }
    CHECK(WirePackets == 257 && Context->TxPackets == 257);
    Handle = AcquireTx(0xA5);
    CHECK(Exports.KdSendTxPacket(&Shared, Handle, 14) == STATUS_SUCCESS);
    CHECK(LastWireLength == 60 && LastWire[13] == 0xA5);
    for (I = 14; I < 60; ++I) { CHECK(LastWire[I] == 0); }
    Handle = AcquireTx(1);
    CHECK(Exports.KdSendTxPacket(&Shared, Handle, 2048) == STATUS_INVALID_PARAMETER);
    CHECK(Context->TxOwned == 0);
    /* All reservations are usable even when submitted in reverse order. */
    for (I = 0; I < 31; ++I) { Held[I] = AcquireTx((UCHAR)I); }
    Before = StallCalls;
    CHECK(Exports.KdGetTxPacket(&Shared, &Handle) == STATUS_IO_TIMEOUT);
    CHECK(StallCalls == Before);
    for (I = 31; I != 0; --I) {
        CHECK(Exports.KdSendTxPacket(&Shared, Held[I - 1], 60) == STATUS_SUCCESS);
        CHECK(LastWire[0] == I - 1);
    }
    /* RX buffers stay valid through out-of-order release and descriptor reuse. */
    for (I = 0; I < 31; ++I) { InjectRx(0, 0, 60, 0xC000, (UCHAR)I); Held[I] = Receive((UCHAR)I, 60); }
    EmptyRx(); Old = Held[15];
    Exports.KdReleaseRxPacket(&Shared, Old);
    InjectRx(0, 0, 60, 0xC000, 77); Held[15] = Receive(77, 60);
    Exports.KdReleaseRxPacket(&Shared, Old); /* stale token cannot free new RX */
    CHECK(Exports.KdGetPacketAddress(&Shared, Held[15]) != NULL);
    for (I = 31; I != 0; --I) {
        Packet = Exports.KdGetPacketAddress(&Shared, Held[I - 1]);
        CHECK(Packet != NULL && *(PUCHAR)Packet == (I == 16 ? 77 : I - 1));
        Exports.KdReleaseRxPacket(&Shared, Held[I - 1]);
    }
    InjectRx(0, 2, 60, 0xC000, 22); Handle = Receive(22, 60);
    Exports.KdReleaseRxPacket(&Shared, Handle);
    InjectRx(0, 0, 0x4000 | 60, 0xC000, 1); EmptyRx();
    InjectRx(0, 0, 60, 0x8000, 1); EmptyRx();
    InjectRx(0, 0, 10, 0xC000, 1); EmptyRx();
    InjectRx(0, 0, 1600, 0xC000, 1); EmptyRx();
    InjectRx(1, 0, 60, 0x4000, 1); EmptyRx();
    CHECK(Context->RxDropped == 7);
    Stop();

    Start(); Context = Shared.Hardware; AutoTx = FALSE;
    Handle = AcquireTx(1); Before = StallCalls;
    CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    CHECK(StallCalls == Before && Context->TxUsed == 1 && WirePackets == 0);
    StallTx = TRUE; Handle = AcquireTx(2);
    CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_LAST, 60) == STATUS_SUCCESS);
    CHECK(Context->TxUsed == 0 && WirePackets == 2 && StallCalls == Before + 1);
    StallTx = FALSE;
    for (I = 0; I < 31; ++I) {
        Handle = AcquireTx((UCHAR)I);
        CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    }
    Before = StallCalls;
    CHECK(Exports.KdGetTxPacket(&Shared, &Handle) == STATUS_IO_TIMEOUT);
    CHECK(StallCalls == Before && Context->TxUsed == 31);
    DeviceTransmit(); EmptyRx(); CHECK(Context->TxUsed == 0 && Context->TxOwned == 0);
    Handle = AcquireTx(3); FrozenClock = TRUE; Before = StallCalls;
    CHECK(Exports.KdSendTxPacket(&Shared, Handle, 60) == STATUS_IO_TIMEOUT);
    CHECK(StallCalls - Before == VMX_TX_POLLS && Context->Restarts == 1 && Context->Running);
    FrozenClock = FALSE; AutoTx = TRUE;
    Handle = AcquireTx(4); CHECK(Exports.KdSendTxPacket(&Shared, Handle, 60) == STATUS_SUCCESS);
    /* Recovery preserves both kinds of retained buffer and discards inflight TX. */
    InjectRx(0, 0, 60, 0xC000, 0xB6); Old = Receive(0xB6, 60);
    Held[0] = AcquireTx(0xD7); AutoTx = FALSE;
    Handle = AcquireTx(5); CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    Cycles += 100001; EmptyRx();
    CHECK(Context->Restarts == 2 && Context->TxOwned == 1);
    Packet = Exports.KdGetPacketAddress(&Shared, Old);
    CHECK(Packet != NULL && *(PUCHAR)Packet == 0xB6);
    Packet = Exports.KdGetPacketAddress(&Shared, Held[0]);
    CHECK(Packet != NULL && *(PUCHAR)Packet == 0xD7);
    Exports.KdReleaseRxPacket(&Shared, Old); AutoTx = TRUE;
    CHECK(Exports.KdSendTxPacket(&Shared, Held[0], 60) == STATUS_SUCCESS);
    InjectRx(0, 0, 60, 0xC000, 8); Handle = Receive(8, 60); Exports.KdReleaseRxPacket(&Shared, Handle);
    ((VMX_QUEUES *)Context->Region[1].Va)->Rx.Status.Error = 0x80000001;
    Bar1[VMX_REG_ECR / 4] = 2; Context->ServiceCalls = 63; EmptyRx();
    CHECK(Context->Restarts == 3 && Context->Running && Context->LastRxError == 0x80000001);
    /* Malformed DMA completion indices cannot index outside our arrays. */
    ((VMX_COMPLETION *)Context->Region[7].Va)[0].Word0 = 0xCFFF;
    ((VMX_COMPLETION *)Context->Region[7].Va)[0].Word3 = 0x83000000;
    EmptyRx(); CHECK(Context->Restarts == 4 && Context->LastStage == VmxRxCompletion);
    ((VMX_COMPLETION *)Context->Region[4].Va)[0].Word0 = 0xFFF;
    ((VMX_COMPLETION *)Context->Region[4].Va)[0].Word3 = 0x80000000;
    EmptyRx(); CHECK(Context->Restarts == 5 && Context->LastStage == VmxTxCompletion);
    LinkDown = TRUE; Bar1[VMX_REG_ECR / 4] = 4; Context->ServiceCalls = 63; EmptyRx();
    CHECK(LinkState == 0 && Bar1[VMX_REG_ECR / 4] == 0);
    CHECK(Exports.KdGetTxPacket(&Shared, &Handle) == STATUS_IO_TIMEOUT);
    LinkDown = FALSE; Context->ServiceCalls = 63; EmptyRx(); CHECK(LinkState == 1);
    Handle = AcquireTx(9); AutoTx = FALSE; StallTx = TRUE;
    CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    Stop(); CHECK(Context->TxUsed == 0);

    Start(); Context = Shared.Hardware; AutoTx = FALSE;
    Handle = AcquireTx(1); CHECK(Exports.KdSendTxPacket(&Shared, Handle | TRANSMIT_ASYNC, 60) == STATUS_SUCCESS);
    FrozenClock = TRUE; Before = StallCalls; Stop();
    CHECK(StallCalls - Before == VMX_TX_POLLS && !Context->Running);
    Start(); Context = Shared.Hardware; ActivationError = 7;
    Bar1[VMX_REG_ECR / 4] = 2; Context->ServiceCalls = 63; EmptyRx();
    CHECK(!Context->Running && !Active && !(PciCommand & 4)); Stop();
#if VMX_FAULT_INJECT
    FaultTests();
    ResetTests();
#endif
    printf("vmxnet3 packet tests (fault variant %u): %lu checks, %lu failures\n",
           (unsigned)VMX_FAULT_INJECT, Checks, Failures);
    return Failures ? 1 : 0;
}

/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2007 VMware, Inc. All rights reserved.
 *
 * Reduced/adapted from DPDK v24.11 vmxnet3_defs.h and upt1_defs.h.
 * See third_party/dpdk/README.md and LICENSE. Revision 1, little endian only.
 */
#pragma once

#define VMX_REG_VRRS       0x000
#define VMX_REG_UVRS       0x008
#define VMX_REG_DSAL       0x010
#define VMX_REG_DSAH       0x018
#define VMX_REG_CMD        0x020
#define VMX_REG_MACL       0x028
#define VMX_REG_MACH       0x030
#define VMX_REG_ECR        0x040
#define VMX_REG_IMR        0x000
#define VMX_REG_TXPROD     0x600
#define VMX_REG_RXPROD     0x800
#define VMX_REG_RXPROD2    0xA00
#define VMX_CMD_ACTIVATE   0xCAFE0000UL
#define VMX_CMD_QUIESCE    0xCAFE0001UL
#define VMX_CMD_RESET      0xCAFE0002UL
#define VMX_CMD_RX_MODE    0xCAFE0003UL
#define VMX_CMD_MAC_FILTER 0xCAFE0004UL
#define VMX_CMD_VLAN       0xCAFE0005UL
#define VMX_CMD_QUEUE      0xF00D0000UL
#define VMX_CMD_LINK       0xF00D0002UL
#define VMX_MAGIC          0xBABEFEE1UL
#define VMX_GOS_WIN64      (2UL | (2UL << 2))
#define VMX_INTR_DISABLE   1UL
#define VMX_RX_MODE        (0x01UL | 0x04UL | 0x08UL)
#define VMX_RX_BODY        (1UL << 14)
#define VMX_RX_GEN         (1UL << 31)
#define VMX_QUEUE_ERRORS   3UL
#define VMX_EVENT_LINK     4UL
#define VMX_TX_GEN         (1UL << 14)
#define VMX_TX_EOP_CQ      ((1UL << 12) | (1UL << 13))
#define VMX_COMP_GEN       (1UL << 31)
#define VMX_COMP_TYPE(v)   (((v) >> 24) & 0x7F)
#define VMX_RX_SOP_EOP     ((1UL << 15) | (1UL << 14))
#define VMX_RX_ERROR       (1UL << 14)
#define VMX_MAX_INTRS      25

#pragma pack(push, 1)
typedef struct _VMX_DRIVER_INFO {
    ULONG Version, Gos, Vmxnet3RevSpt, UptVerSpt;
} VMX_DRIVER_INFO;

typedef struct _VMX_MISC {
    VMX_DRIVER_INFO DriverInfo;
    ULONG64 UptFeatures, DdPA, QueueDescPA;
    ULONG DdLen, QueueDescLen, Mtu;
    USHORT MaxNumRxSG;
    UCHAR NumTxQueues, NumRxQueues;
    ULONG Reserved[4];
} VMX_MISC;

typedef struct _VMX_INTR {
    UCHAR AutoMask, NumIntrs, EventIntrIdx, ModLevels[VMX_MAX_INTRS];
    ULONG IntrCtrl, Reserved[2];
} VMX_INTR;

typedef struct _VMX_FILTER {
    ULONG RxMode;
    USHORT MfTableLen, Pad;
    ULONG64 MfTablePA;
    ULONG VfTable[128];
} VMX_FILTER;

typedef struct _VMX_VARIABLE_CONF {
    ULONG Version, Length;
    ULONG64 Address;
} VMX_VARIABLE_CONF;

typedef struct _VMX_SHARED {
    ULONG Magic, Reserved;
    VMX_MISC Misc;
    VMX_INTR Intr;
    VMX_FILTER Filter;
    VMX_VARIABLE_CONF Rss, Power, Plugin;
    ULONG Ecr, Reserved2[5];
} VMX_SHARED;

typedef struct _VMX_TX_CONF {
    ULONG64 RingPA, DataPA, CompPA, DdPA, Reserved;
    ULONG RingSize, DataSize, CompSize, DdLen;
    UCHAR IntrIdx, Pad[7];
} VMX_TX_CONF;

typedef struct _VMX_RX_CONF {
    ULONG64 RingPA[2], CompPA, DdPA, Reserved;
    ULONG RingSize[2], CompSize, DdLen;
    UCHAR IntrIdx, Pad[7];
} VMX_RX_CONF;

typedef struct _VMX_QUEUE_STATUS {
    UCHAR Stopped, Pad[3];
    ULONG Error;
} VMX_QUEUE_STATUS;

typedef struct _VMX_TX_QUEUE {
    ULONG NumDeferred, Threshold;
    ULONG64 Reserved;
    VMX_TX_CONF Conf;
    VMX_QUEUE_STATUS Status;
    ULONG64 Stats[10];
    UCHAR Pad[88];
} VMX_TX_QUEUE;

typedef struct _VMX_RX_QUEUE {
    UCHAR UpdateRxProd, Pad1[7];
    ULONG64 Reserved;
    VMX_RX_CONF Conf;
    VMX_QUEUE_STATUS Status;
    ULONG64 Stats[10];
    UCHAR Pad2[88];
} VMX_RX_QUEUE;

typedef struct _VMX_QUEUES {
    VMX_TX_QUEUE Tx;
    VMX_RX_QUEUE Rx;
} VMX_QUEUES;

typedef struct _VMX_DESC {
    ULONG64 Address;
    ULONG Word2, Word3;
} VMX_DESC;
typedef struct _VMX_COMPLETION {
    ULONG Word0, Word1, Word2, Word3;
} VMX_COMPLETION;
#pragma pack(pop)

/* Byte offsets are taken from the packed upstream wire layouts. */
C_ASSERT(sizeof(VMX_DRIVER_INFO) == 16);
C_ASSERT(sizeof(VMX_MISC) == 72);
C_ASSERT(FIELD_OFFSET(VMX_MISC, QueueDescPA) == 32);
C_ASSERT(FIELD_OFFSET(VMX_MISC, NumTxQueues) == 54);
C_ASSERT(sizeof(VMX_INTR) == 40);
C_ASSERT(FIELD_OFFSET(VMX_INTR, IntrCtrl) == 28);
C_ASSERT(sizeof(VMX_FILTER) == 528);
C_ASSERT(FIELD_OFFSET(VMX_FILTER, VfTable) == 16);
C_ASSERT(sizeof(VMX_VARIABLE_CONF) == 16);
C_ASSERT(sizeof(VMX_SHARED) == 720);
C_ASSERT(FIELD_OFFSET(VMX_SHARED, Misc) == 8);
C_ASSERT(FIELD_OFFSET(VMX_SHARED, Intr) == 80);
C_ASSERT(FIELD_OFFSET(VMX_SHARED, Filter) == 120);
C_ASSERT(FIELD_OFFSET(VMX_SHARED, Ecr) == 696);
C_ASSERT(sizeof(VMX_TX_CONF) == 64);
C_ASSERT(sizeof(VMX_RX_CONF) == 64);
C_ASSERT(FIELD_OFFSET(VMX_TX_CONF, RingSize) == 40);
C_ASSERT(FIELD_OFFSET(VMX_RX_CONF, RingSize) == 40);
C_ASSERT(FIELD_OFFSET(VMX_TX_CONF, IntrIdx) == 56);
C_ASSERT(FIELD_OFFSET(VMX_RX_CONF, IntrIdx) == 56);
C_ASSERT(sizeof(VMX_QUEUE_STATUS) == 8);
C_ASSERT(sizeof(VMX_TX_QUEUE) == 256);
C_ASSERT(sizeof(VMX_RX_QUEUE) == 256);
C_ASSERT(FIELD_OFFSET(VMX_TX_QUEUE, Conf) == 16);
C_ASSERT(FIELD_OFFSET(VMX_RX_QUEUE, Conf) == 16);
C_ASSERT(FIELD_OFFSET(VMX_TX_QUEUE, Status) == 80);
C_ASSERT(FIELD_OFFSET(VMX_RX_QUEUE, Status) == 80);
C_ASSERT(sizeof(VMX_QUEUES) == 512);
C_ASSERT(FIELD_OFFSET(VMX_QUEUES, Rx) == 256);
C_ASSERT(sizeof(VMX_DESC) == 16);
C_ASSERT(FIELD_OFFSET(VMX_DESC, Word2) == 8);
C_ASSERT(sizeof(VMX_COMPLETION) == 16);
C_ASSERT(FIELD_OFFSET(VMX_COMPLETION, Word3) == 12);

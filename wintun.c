/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018-2019 WireGuard LLC. All Rights Reserved.
 */

#include <ntifs.h>
#include <wdm.h>
#include <wdmsec.h>
#include <ndis.h>
#include <ntstrsafe.h>
#include "undocumented.h"

#pragma warning(disable : 4100) /* unreferenced formal parameter */
#pragma warning(disable : 4200) /* nonstandard: zero-sized array in struct/union */
#pragma warning(disable : 4204) /* nonstandard: non-constant aggregate initializer */
#pragma warning(disable : 4221) /* nonstandard: cannot be initialized using address of automatic variable */
#pragma warning(disable : 6320) /* exception-filter expression is the constant EXCEPTION_EXECUTE_HANDLER */

#define NDIS_MINIPORT_VERSION_MIN ((NDIS_MINIPORT_MINIMUM_MAJOR_VERSION << 16) | NDIS_MINIPORT_MINIMUM_MINOR_VERSION)
#define NDIS_MINIPORT_VERSION_MAX ((NDIS_MINIPORT_MAJOR_VERSION << 16) | NDIS_MINIPORT_MINOR_VERSION)

/* Data device name */
#define TUN_DEVICE_NAME L"WINTUN%u"

#define TUN_VENDOR_NAME "Wintun Tunnel"
#define TUN_VENDOR_ID 0xFFFFFF00
#define TUN_LINK_SPEED 100000000000ULL /* 100gbps */

/* Memory alignment of packets and rings */
#define TUN_ALIGNMENT sizeof(ULONG)
#define TUN_ALIGN(Size) (((ULONG)(Size) + (ULONG)(TUN_ALIGNMENT - 1)) & ~(ULONG)(TUN_ALIGNMENT - 1))
/* Maximum IP packet size */
#define TUN_MAX_IP_PACKET_SIZE 0xFFFF
/* Maximum packet size */
#define TUN_MAX_PACKET_SIZE TUN_ALIGN(sizeof(TUN_PACKET) + TUN_MAX_IP_PACKET_SIZE)
/* Minimum ring capacity. */
#define TUN_MIN_RING_CAPACITY 0x20000 /* 128kiB */
/* Maximum ring capacity. */
#define TUN_MAX_RING_CAPACITY 0x4000000 /* 64MiB */
/* Calculates ring capacity */
#define TUN_RING_CAPACITY(Size) ((Size) - sizeof(TUN_RING) - (TUN_MAX_PACKET_SIZE - TUN_ALIGNMENT))
/* Calculates ring offset modulo capacity */
#define TUN_RING_WRAP(Value, Capacity) ((Value) & (Capacity - 1))

#if REG_DWORD == REG_DWORD_BIG_ENDIAN
#    define TUN_HTONS(x) ((USHORT)(x))
#    define TUN_HTONL(x) ((ULONG)(x))
#elif REG_DWORD == REG_DWORD_LITTLE_ENDIAN
#    define TUN_HTONS(x) ((((USHORT)(x)&0x00ff) << 8) | (((USHORT)(x)&0xff00) >> 8))
#    define TUN_HTONL(x) \
        ((((ULONG)(x)&0x000000ff) << 24) | (((ULONG)(x)&0x0000ff00) << 8) | (((ULONG)(x)&0x00ff0000) >> 8) | \
         (((ULONG)(x)&0xff000000) >> 24))
#else
#    error "Unable to determine endianess"
#endif

#define TUN_MEMORY_TAG TUN_HTONL('wtun')

typedef struct _TUN_PACKET
{
    /* Size of packet data (TUN_MAX_IP_PACKET_SIZE max) */
    ULONG Size;

    /* Packet data */
    UCHAR _Field_size_bytes_(Size)
    Data[];
} TUN_PACKET;

typedef struct _TUN_RING
{
    /* Byte offset of the first packet in the ring. Its value must be a multiple of TUN_ALIGNMENT and less than ring
     * capacity. */
    volatile ULONG Head;

    /* Byte offset of the first free space in the ring. Its value must be multiple of TUN_ALIGNMENT and less than ring
     * capacity. */
    volatile ULONG Tail;

    /* Non-zero when consumer is in alertable state. */
    volatile LONG Alertable;

    /* Ring data. Its capacity must be a power of 2 + extra TUN_MAX_PACKET_SIZE-TUN_ALIGNMENT space to
     * eliminate need for wrapping. */
    UCHAR Data[];
} TUN_RING;

typedef struct _TUN_REGISTER_RINGS
{
    struct
    {
        /* Size of the ring */
        ULONG RingSize;

        /* Pointer to client allocated ring */
        TUN_RING *Ring;

        /* On send: An event created by the client the Wintun signals after it moves the Tail member of the send ring.
         * On receive: An event created by the client the client will signal when it moves the Tail member of
         * the receive ring if receive ring is alertable. */
        HANDLE TailMoved;
    } Send, Receive;
} TUN_REGISTER_RINGS;

/* Register rings hosted by the client
 * The lpInBuffer and nInBufferSize parameters of DeviceIoControl() must point to an TUN_REGISTER_RINGS struct.
 * Client must wait for this IOCTL to finish before adding packets to the ring. */
#define TUN_IOCTL_REGISTER_RINGS CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
/* TODO: Select and specify OEM-specific device type instead of FILE_DEVICE_UNKNOWN. */

typedef enum _TUN_FLAGS
{
    TUN_FLAGS_RUNNING = 1 << 0,   /* Toggles between paused and running state */
    TUN_FLAGS_PRESENT = 1 << 1,   /* Toggles between removal pending and being present */
    TUN_FLAGS_CONNECTED = 1 << 2, /* Is client connected? */
} TUN_FLAGS;

typedef struct _TUN_CTX
{
    volatile LONG Flags;

    /* Used like RCU. When we're making use of rings, we take a shared lock. When we want to register or release the
     * rings and toggle the state, we take an exclusive lock before toggling the atomic and then releasing. It's similar
     * to setting the atomic and then calling rcu_barrier(). */
    EX_SPIN_LOCK TransitionLock;

    NDIS_HANDLE MiniportAdapterHandle; /* This is actually a pointer to NDIS_MINIPORT_BLOCK struct. */
    NDIS_STATISTICS_INFO Statistics;

    struct
    {
        NDIS_HANDLE Handle;
        DEVICE_OBJECT *Object;
        IO_REMOVE_LOCK RemoveLock;
        FILE_OBJECT *volatile Owner;

        struct
        {
            MDL *Mdl;
            TUN_RING *Ring;
            ULONG Capacity;
            KEVENT *TailMoved;
            KSPIN_LOCK Lock;
        } Send;

        struct
        {
            MDL *Mdl;
            TUN_RING *Ring;
            ULONG Capacity;
            KEVENT *TailMoved;
            HANDLE Thread;
        } Receive;
    } Device;

    NDIS_HANDLE NblPool;
} TUN_CTX;

static UINT NdisVersion;
static NDIS_HANDLE NdisMiniportDriverHandle;
static DRIVER_DISPATCH *NdisDispatchPnP;
static volatile LONG64 TunAdapterCount;

static __forceinline ULONG
InterlockedExchangeU(_Inout_ _Interlocked_operand_ ULONG volatile *Target, _In_ ULONG Value)
{
    return (ULONG)InterlockedExchange((LONG volatile *)Target, Value);
}

static __forceinline LONG
InterlockedGet(_In_ _Interlocked_operand_ LONG volatile *Value)
{
    return *Value;
}

static __forceinline ULONG
InterlockedGetU(_In_ _Interlocked_operand_ ULONG volatile *Value)
{
    return *Value;
}

static __forceinline PVOID
InterlockedGetPointer(_In_ _Interlocked_operand_ PVOID volatile *Value)
{
    return *Value;
}

static __forceinline LONG64
InterlockedGet64(_In_ _Interlocked_operand_ LONG64 volatile *Value)
{
#ifdef _WIN64
    return *Value;
#else
    return InterlockedCompareExchange64(Value, 0, 0);
#endif
}

#define TunInitUnicodeString(str, buf) \
    { \
        (str)->Length = 0; \
        (str)->MaximumLength = sizeof(buf); \
        (str)->Buffer = buf; \
    }

_IRQL_requires_max_(DISPATCH_LEVEL)
static void
TunIndicateStatus(_In_ NDIS_HANDLE MiniportAdapterHandle, _In_ NDIS_MEDIA_CONNECT_STATE MediaConnectState)
{
    NDIS_LINK_STATE State = { .Header = { .Type = NDIS_OBJECT_TYPE_DEFAULT,
                                          .Revision = NDIS_LINK_STATE_REVISION_1,
                                          .Size = NDIS_SIZEOF_LINK_STATE_REVISION_1 },
                              .MediaConnectState = MediaConnectState,
                              .MediaDuplexState = MediaDuplexStateFull,
                              .XmitLinkSpeed = TUN_LINK_SPEED,
                              .RcvLinkSpeed = TUN_LINK_SPEED,
                              .PauseFunctions = NdisPauseFunctionsUnsupported };

    NDIS_STATUS_INDICATION Indication = { .Header = { .Type = NDIS_OBJECT_TYPE_STATUS_INDICATION,
                                                      .Revision = NDIS_STATUS_INDICATION_REVISION_1,
                                                      .Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1 },
                                          .SourceHandle = MiniportAdapterHandle,
                                          .StatusCode = NDIS_STATUS_LINK_STATE,
                                          .StatusBuffer = &State,
                                          .StatusBufferSize = sizeof(State) };

    NdisMIndicateStatusEx(MiniportAdapterHandle, &Indication);
}

static MINIPORT_SEND_NET_BUFFER_LISTS TunSendNetBufferLists;
_Use_decl_annotations_
static void
TunSendNetBufferLists(
    NDIS_HANDLE MiniportAdapterContext,
    NET_BUFFER_LIST *NetBufferLists,
    NDIS_PORT_NUMBER PortNumber,
    ULONG SendFlags)
{
    TUN_CTX *Ctx = (TUN_CTX *)MiniportAdapterContext;
    LONG64 SentPacketsCount = 0, SentPacketsSize = 0, DiscardedPacketsCount = 0;

    /* TODO: This handler is called by NDIS in parallel. Consider implementing a lock-less MPSC ring. */

    KIRQL Irql = ExAcquireSpinLockShared(&Ctx->TransitionLock);
    LONG Flags = InterlockedGet(&Ctx->Flags);

    for (NET_BUFFER_LIST *Nbl = NetBufferLists; Nbl; Nbl = NET_BUFFER_LIST_NEXT_NBL(Nbl))
    {
        for (NET_BUFFER *Nb = NET_BUFFER_LIST_FIRST_NB(Nbl); Nb; Nb = NET_BUFFER_NEXT_NB(Nb))
        {
            NDIS_STATUS Status;
            if ((Status = NDIS_STATUS_ADAPTER_REMOVED, !(Flags & TUN_FLAGS_PRESENT)) ||
                (Status = NDIS_STATUS_PAUSED, !(Flags & TUN_FLAGS_RUNNING)) ||
                (Status = NDIS_STATUS_MEDIA_DISCONNECTED, !(Flags & TUN_FLAGS_CONNECTED)))
                goto cleanupDiscardPacket;

            TUN_RING *Ring = Ctx->Device.Send.Ring;
            ULONG RingCapacity = Ctx->Device.Send.Capacity;
            ULONG PacketSize = NET_BUFFER_DATA_LENGTH(Nb);
            if (Status = NDIS_STATUS_INVALID_LENGTH, PacketSize > TUN_MAX_IP_PACKET_SIZE)
                goto cleanupDiscardPacket;
            ULONG AlignedPacketSize = TUN_ALIGN(sizeof(TUN_PACKET) + PacketSize);

            KLOCK_QUEUE_HANDLE LockHandle;
            KeAcquireInStackQueuedSpinLock(&Ctx->Device.Send.Lock, &LockHandle);

            ULONG RingHead = InterlockedGetU(&Ring->Head);
            if (Status = NDIS_STATUS_ADAPTER_NOT_READY, RingHead >= RingCapacity)
                goto cleanupReleaseSpinLock;

            ULONG RingTail = InterlockedGetU(&Ring->Tail);
            if (Status = NDIS_STATUS_ADAPTER_NOT_READY, RingTail >= RingCapacity)
                goto cleanupReleaseSpinLock;

            ULONG RingSpace = TUN_RING_WRAP(RingHead - RingTail - TUN_ALIGNMENT, RingCapacity);
            if (Status = NDIS_STATUS_BUFFER_OVERFLOW, AlignedPacketSize > RingSpace)
                goto cleanupReleaseSpinLock;

            TUN_PACKET *Packet = (TUN_PACKET *)(Ring->Data + RingTail);
            Packet->Size = PacketSize;
            void *NbData = NdisGetDataBuffer(Nb, PacketSize, Packet->Data, 1, 0);
            if (!NbData)
                goto cleanupReleaseSpinLock;
            if (NbData != Packet->Data)
                NdisMoveMemory(Packet->Data, NbData, PacketSize);
            InterlockedExchangeU(&Ring->Tail, TUN_RING_WRAP(RingTail + AlignedPacketSize, RingCapacity));
            KeSetEvent(Ctx->Device.Send.TailMoved, IO_NETWORK_INCREMENT, FALSE);

            KeReleaseInStackQueuedSpinLock(&LockHandle);
            SentPacketsCount++;
            SentPacketsSize += PacketSize;
            continue;

        cleanupReleaseSpinLock:
            KeReleaseInStackQueuedSpinLock(&LockHandle);
        cleanupDiscardPacket:
            DiscardedPacketsCount++;
            NET_BUFFER_LIST_STATUS(Nbl) = Status;
        }
    }

    NdisMSendNetBufferListsComplete(
        Ctx->MiniportAdapterHandle, NetBufferLists, NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL);

    ExReleaseSpinLockShared(&Ctx->TransitionLock, Irql);

    InterlockedAdd64((LONG64 *)&Ctx->Statistics.ifHCOutOctets, SentPacketsSize);
    InterlockedAdd64((LONG64 *)&Ctx->Statistics.ifHCOutUcastOctets, SentPacketsSize);
    InterlockedAdd64((LONG64 *)&Ctx->Statistics.ifHCOutUcastPkts, SentPacketsCount);
    InterlockedAdd64((LONG64 *)&Ctx->Statistics.ifOutDiscards, DiscardedPacketsCount);
}

static MINIPORT_CANCEL_SEND TunCancelSend;
_Use_decl_annotations_
static void
TunCancelSend(NDIS_HANDLE MiniportAdapterContext, PVOID CancelId)
{
}

static MINIPORT_RETURN_NET_BUFFER_LISTS TunReturnNetBufferLists;
_Use_decl_annotations_
static void
TunReturnNetBufferLists(NDIS_HANDLE MiniportAdapterContext, PNET_BUFFER_LIST NetBufferLists, ULONG ReturnFlags)
{
}

_IRQL_requires_max_(PASSIVE_LEVEL)
_Function_class_(KSTART_ROUTINE)
static void
TunProcessReceiveData(_Inout_ TUN_CTX *Ctx)
{
    KIRQL Irql = ExAcquireSpinLockShared(&Ctx->TransitionLock);
    TUN_RING *Ring = Ctx->Device.Receive.Ring;
    ULONG RingCapacity = Ctx->Device.Receive.Capacity;
    const ULONG SpinMax = 10000 * 50 / KeQueryTimeIncrement(); /* 50ms */

    for (;;)
    {
        LONG Flags = InterlockedGet(&Ctx->Flags);
        if (!(Flags & TUN_FLAGS_CONNECTED))
            break;

        /* Get next packet from the ring. */
        ULONG RingHead = InterlockedGetU(&Ring->Head);
        if (RingHead >= RingCapacity)
            break;

        ULONG RingTail = InterlockedGetU(&Ring->Tail);
        if (RingHead == RingTail)
        {
            ExReleaseSpinLockShared(&Ctx->TransitionLock, Irql);
            ULONG64 SpinStart;
            KeQueryTickCount(&SpinStart);
            for (;;)
            {
                RingTail = InterlockedGetU(&Ring->Tail);
                if (RingTail != RingHead)
                    break;
                if (!(InterlockedGet(&Ctx->Flags) & TUN_FLAGS_CONNECTED))
                    break;
                ULONG64 SpinNow;
                KeQueryTickCount(&SpinNow);
                if (SpinNow - SpinStart >= SpinMax)
                    break;

                /* This should really call KeYieldProcessorEx(&zero), so it does the Hyper-V paravirtualization call,
                 * but it's not exported. */
                YieldProcessor();
            }
            if (RingHead == RingTail)
            {
                InterlockedExchange(&Ring->Alertable, TRUE);
                RingTail = InterlockedGetU(&Ring->Tail);
                if (RingHead == RingTail)
                {
                    KeWaitForSingleObject(Ctx->Device.Receive.TailMoved, Executive, KernelMode, FALSE, NULL);
                    InterlockedExchange(&Ring->Alertable, FALSE);
                    Irql = ExAcquireSpinLockShared(&Ctx->TransitionLock);
                    continue;
                }
                InterlockedExchange(&Ring->Alertable, FALSE);
                KeClearEvent(Ctx->Device.Receive.TailMoved);
            }
            Irql = ExAcquireSpinLockShared(&Ctx->TransitionLock);
        }
        if (RingTail >= RingCapacity)
            break;

        ULONG RingContent = TUN_RING_WRAP(RingTail - RingHead, RingCapacity);
        if (RingContent < sizeof(TUN_PACKET))
            break;

        TUN_PACKET *Packet = (TUN_PACKET *)(Ring->Data + RingHead);
        ULONG PacketSize = Packet->Size;
        if (PacketSize > TUN_MAX_IP_PACKET_SIZE)
            break;

        ULONG AlignedPacketSize = TUN_ALIGN(sizeof(TUN_PACKET) + PacketSize);
        if (AlignedPacketSize > RingContent)
            break;

        ULONG NblFlags;
        USHORT NblProto;
        if (PacketSize >= 20 && Packet->Data[0] >> 4 == 4)
        {
            NblFlags = NDIS_NBL_FLAGS_IS_IPV4;
            NblProto = TUN_HTONS(NDIS_ETH_TYPE_IPV4);
        }
        else if (PacketSize >= 40 && Packet->Data[0] >> 4 == 6)
        {
            NblFlags = NDIS_NBL_FLAGS_IS_IPV6;
            NblProto = TUN_HTONS(NDIS_ETH_TYPE_IPV6);
        }
        else
            break;

        NET_BUFFER_LIST *Nbl = NdisAllocateNetBufferAndNetBufferList(
            Ctx->NblPool, 0, 0, Ctx->Device.Receive.Mdl, (ULONG)(Packet->Data - (UCHAR *)Ring), PacketSize);
        if (!Nbl)
            goto cleanupDiscardPacket;

        Nbl->SourceHandle = Ctx->MiniportAdapterHandle;
        NdisSetNblFlag(Nbl, NblFlags);
        NET_BUFFER_LIST_INFO(Nbl, NetBufferListFrameType) = (PVOID)NblProto;
        NET_BUFFER_LIST_STATUS(Nbl) = NDIS_STATUS_SUCCESS;

        /* Inform NDIS of the packet. */
        if ((Flags & (TUN_FLAGS_PRESENT | TUN_FLAGS_RUNNING)) != (TUN_FLAGS_PRESENT | TUN_FLAGS_RUNNING))
            goto cleanupFreeNbl;

        /* TODO: Consider making packet(s) copy rather than using NDIS_RECEIVE_FLAGS_RESOURCES. */
        NdisMIndicateReceiveNetBufferLists(
            Ctx->MiniportAdapterHandle,
            Nbl,
            NDIS_DEFAULT_PORT_NUMBER,
            1,
            NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL | NDIS_RECEIVE_FLAGS_RESOURCES | NDIS_RECEIVE_FLAGS_SINGLE_ETHER_TYPE);
        InterlockedAdd64((LONG64 *)&Ctx->Statistics.ifHCInOctets, PacketSize);
        InterlockedAdd64((LONG64 *)&Ctx->Statistics.ifHCInUcastOctets, PacketSize);
        InterlockedIncrement64((LONG64 *)&Ctx->Statistics.ifHCInUcastPkts);

        NdisFreeNetBufferList(Nbl);
        goto nextPacket;

    cleanupFreeNbl:
        NdisFreeNetBufferList(Nbl);
    cleanupDiscardPacket:
        InterlockedIncrement64((LONG64 *)&Ctx->Statistics.ifInDiscards);
    nextPacket:
        InterlockedExchangeU(&Ring->Head, TUN_RING_WRAP(RingHead + AlignedPacketSize, RingCapacity));
    }

    InterlockedExchangeU(&Ring->Head, MAXULONG);
    ExReleaseSpinLockShared(&Ctx->TransitionLock, Irql);
}

_IRQL_requires_max_(APC_LEVEL)
_Must_inspect_result_
static NTSTATUS
TunDispatchCreate(_Inout_ TUN_CTX *Ctx, _Inout_ IRP *Irp)
{
    NTSTATUS Status;

    KIRQL Irql = ExAcquireSpinLockShared(&Ctx->TransitionLock);
    LONG Flags = InterlockedGet(&Ctx->Flags);
    if (Status = STATUS_DELETE_PENDING, !(Flags & TUN_FLAGS_PRESENT))
        goto cleanupReleaseSpinLock;

    IO_STACK_LOCATION *Stack = IoGetCurrentIrpStackLocation(Irp);
    Status = IoAcquireRemoveLock(&Ctx->Device.RemoveLock, Stack->FileObject);

cleanupReleaseSpinLock:
    ExReleaseSpinLockShared(&Ctx->TransitionLock, Irql);
    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
static NTSTATUS
TunDispatchRegisterBuffers(_Inout_ TUN_CTX *Ctx, _Inout_ IRP *Irp)
{
    NTSTATUS Status;
    IO_STACK_LOCATION *Stack = IoGetCurrentIrpStackLocation(Irp);

    if (InterlockedCompareExchangePointer(&Ctx->Device.Owner, Stack->FileObject, NULL) != NULL)
        return STATUS_ALREADY_INITIALIZED;

    TUN_REGISTER_RINGS *Rrb = Irp->AssociatedIrp.SystemBuffer;
    if (Status = STATUS_INVALID_PARAMETER, Stack->Parameters.DeviceIoControl.InputBufferLength != sizeof(*Rrb))
        goto cleanupResetOwner;

    /* Analyze and lock send ring. */
    Ctx->Device.Send.Capacity = TUN_RING_CAPACITY(Rrb->Send.RingSize);
    if (Status = STATUS_INVALID_PARAMETER,
        (Ctx->Device.Send.Capacity < TUN_MIN_RING_CAPACITY || Ctx->Device.Send.Capacity > TUN_MAX_RING_CAPACITY ||
         PopulationCount64(Ctx->Device.Send.Capacity) != 1 || !Rrb->Send.TailMoved || !Rrb->Send.Ring))
        goto cleanupResetOwner;

    if (!NT_SUCCESS(
            Status = ObReferenceObjectByHandle(
                Rrb->Send.TailMoved,
                EVENT_MODIFY_STATE, /* We will not wait on send ring tail moved event. */
                *ExEventObjectType,
                UserMode,
                &Ctx->Device.Send.TailMoved,
                NULL)))
        goto cleanupResetOwner;

    Ctx->Device.Send.Mdl = IoAllocateMdl(Rrb->Send.Ring, Rrb->Send.RingSize, FALSE, FALSE, NULL);
    if (Status = STATUS_INSUFFICIENT_RESOURCES, !Ctx->Device.Send.Mdl)
        goto cleanupSendTailMoved;
    try
    {
        Status = STATUS_INVALID_USER_BUFFER;
        MmProbeAndLockPages(Ctx->Device.Send.Mdl, UserMode, IoWriteAccess);
    }
    except(EXCEPTION_EXECUTE_HANDLER) { goto cleanupSendMdl; }

    Ctx->Device.Send.Ring =
        MmGetSystemAddressForMdlSafe(Ctx->Device.Send.Mdl, NormalPagePriority | MdlMappingNoExecute);
    if (Status = STATUS_INSUFFICIENT_RESOURCES, !Ctx->Device.Send.Ring)
        goto cleanupSendUnlockPages;

    /* Analyze and lock receive ring. */
    Ctx->Device.Receive.Capacity = TUN_RING_CAPACITY(Rrb->Receive.RingSize);
    if (Status = STATUS_INVALID_PARAMETER,
        (Ctx->Device.Receive.Capacity < TUN_MIN_RING_CAPACITY || Ctx->Device.Receive.Capacity > TUN_MAX_RING_CAPACITY ||
         PopulationCount64(Ctx->Device.Receive.Capacity) != 1 || !Rrb->Receive.TailMoved || !Rrb->Receive.Ring))
        goto cleanupSendUnlockPages;

    if (!NT_SUCCESS(
            Status = ObReferenceObjectByHandle(
                Rrb->Receive.TailMoved,
                SYNCHRONIZE | EVENT_MODIFY_STATE, /* We need to set recv ring TailMoved event to unblock on close. */
                *ExEventObjectType,
                UserMode,
                &Ctx->Device.Receive.TailMoved,
                NULL)))
        goto cleanupSendUnlockPages;

    Ctx->Device.Receive.Mdl = IoAllocateMdl(Rrb->Receive.Ring, Rrb->Receive.RingSize, FALSE, FALSE, NULL);
    if (Status = STATUS_INSUFFICIENT_RESOURCES, !Ctx->Device.Receive.Mdl)
        goto cleanupReceiveTailMoved;
    try
    {
        Status = STATUS_INVALID_USER_BUFFER;
        MmProbeAndLockPages(Ctx->Device.Receive.Mdl, UserMode, IoWriteAccess);
    }
    except(EXCEPTION_EXECUTE_HANDLER) { goto cleanupReceiveMdl; }

    Ctx->Device.Receive.Ring =
        MmGetSystemAddressForMdlSafe(Ctx->Device.Receive.Mdl, NormalPagePriority | MdlMappingNoExecute);
    if (Status = STATUS_INSUFFICIENT_RESOURCES, !Ctx->Device.Receive.Ring)
        goto cleanupReceiveUnlockPages;

    InterlockedOr(&Ctx->Flags, TUN_FLAGS_CONNECTED);

    /* Spawn receiver thread. */
    OBJECT_ATTRIBUTES ObjectAttributes;
    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    if (Status = NDIS_STATUS_FAILURE,
        !NT_SUCCESS(PsCreateSystemThread(
            &Ctx->Device.Receive.Thread, THREAD_ALL_ACCESS, &ObjectAttributes, NULL, NULL, TunProcessReceiveData, Ctx)))
        goto cleanupFlagsConnected;

    TunIndicateStatus(Ctx->MiniportAdapterHandle, MediaConnectStateConnected);
    return STATUS_SUCCESS;

cleanupFlagsConnected:
    InterlockedAnd(&Ctx->Flags, ~TUN_FLAGS_CONNECTED);
    ExReleaseSpinLockExclusive(
        &Ctx->TransitionLock,
        ExAcquireSpinLockExclusive(&Ctx->TransitionLock)); /* Ensure above change is visible to all readers. */
cleanupReceiveUnlockPages:
    MmUnlockPages(Ctx->Device.Receive.Mdl);
cleanupReceiveMdl:
    IoFreeMdl(Ctx->Device.Receive.Mdl);
cleanupReceiveTailMoved:
    ObDereferenceObject(Ctx->Device.Receive.TailMoved);
cleanupSendUnlockPages:
    MmUnlockPages(Ctx->Device.Send.Mdl);
cleanupSendMdl:
    IoFreeMdl(Ctx->Device.Send.Mdl);
cleanupSendTailMoved:
    ObDereferenceObject(Ctx->Device.Send.TailMoved);
cleanupResetOwner:
    InterlockedExchangePointer(&Ctx->Device.Owner, NULL);
    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
static void
TunDispatchUnregisterBuffers(_Inout_ TUN_CTX *Ctx, _In_ FILE_OBJECT *Owner)
{
    if (InterlockedCompareExchangePointer(&Ctx->Device.Owner, NULL, Owner) != Owner)
        return;

    InterlockedAnd(&Ctx->Flags, ~TUN_FLAGS_CONNECTED);
    ExReleaseSpinLockExclusive(
        &Ctx->TransitionLock,
        ExAcquireSpinLockExclusive(&Ctx->TransitionLock)); /* Ensure Flags change is visible to all readers. */
    KeSetEvent(Ctx->Device.Receive.TailMoved, IO_NO_INCREMENT, FALSE);

    TunIndicateStatus(Ctx->MiniportAdapterHandle, MediaConnectStateDisconnected);

    PKTHREAD ThreadObject;
    if (NT_SUCCESS(
            ObReferenceObjectByHandle(Ctx->Device.Receive.Thread, SYNCHRONIZE, NULL, KernelMode, &ThreadObject, NULL)))
    {
        KeWaitForSingleObject(ThreadObject, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(ThreadObject);
    }
    ZwClose(Ctx->Device.Receive.Thread);

    InterlockedExchangeU(&Ctx->Device.Send.Ring->Tail, MAXULONG);
    KeSetEvent(Ctx->Device.Send.TailMoved, IO_NO_INCREMENT, FALSE);

    MmUnlockPages(Ctx->Device.Receive.Mdl);
    IoFreeMdl(Ctx->Device.Receive.Mdl);
    ObDereferenceObject(Ctx->Device.Receive.TailMoved);
    MmUnlockPages(Ctx->Device.Send.Mdl);
    IoFreeMdl(Ctx->Device.Send.Mdl);
    ObDereferenceObject(Ctx->Device.Send.TailMoved);
}

static DRIVER_DISPATCH_PAGED TunDispatch;
_Use_decl_annotations_
static NTSTATUS
TunDispatch(DEVICE_OBJECT *DeviceObject, IRP *Irp)
{
    NTSTATUS Status;

    Irp->IoStatus.Information = 0;
    TUN_CTX *Ctx = NdisGetDeviceReservedExtension(DeviceObject);
    if (Status = STATUS_INVALID_HANDLE, !Ctx)
        goto cleanupCompleteRequest;

    IO_STACK_LOCATION *Stack = IoGetCurrentIrpStackLocation(Irp);
    switch (Stack->MajorFunction)
    {
    case IRP_MJ_CREATE:
        if (!NT_SUCCESS(Status = IoAcquireRemoveLock(&Ctx->Device.RemoveLock, Irp)))
            goto cleanupCompleteRequest;
        Status = TunDispatchCreate(Ctx, Irp);
        IoReleaseRemoveLock(&Ctx->Device.RemoveLock, Irp);
        break;

    case IRP_MJ_DEVICE_CONTROL:
        if ((Status = STATUS_INVALID_PARAMETER,
             Stack->Parameters.DeviceIoControl.IoControlCode != TUN_IOCTL_REGISTER_RINGS) ||
            !NT_SUCCESS(Status = IoAcquireRemoveLock(&Ctx->Device.RemoveLock, Irp)))
            goto cleanupCompleteRequest;
        Status = TunDispatchRegisterBuffers(Ctx, Irp);
        IoReleaseRemoveLock(&Ctx->Device.RemoveLock, Irp);
        break;

    case IRP_MJ_CLOSE:
        Status = STATUS_SUCCESS;
        TunDispatchUnregisterBuffers(Ctx, Stack->FileObject);
        IoReleaseRemoveLock(&Ctx->Device.RemoveLock, Stack->FileObject);
        break;

    default:
        Status = STATUS_INVALID_PARAMETER;
        break;
    }

cleanupCompleteRequest:
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

_Dispatch_type_(IRP_MJ_PNP) static DRIVER_DISPATCH TunDispatchPnP;
_Use_decl_annotations_
static NTSTATUS
TunDispatchPnP(DEVICE_OBJECT *DeviceObject, IRP *Irp)
{
    IO_STACK_LOCATION *Stack = IoGetCurrentIrpStackLocation(Irp);
    if (Stack->MajorFunction == IRP_MJ_PNP)
    {
#pragma warning(suppress : 28175)
        TUN_CTX *Ctx = DeviceObject->Reserved;
        if (!Ctx)
            return NdisDispatchPnP(DeviceObject, Irp);

        switch (Stack->MinorFunction)
        {
        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
            InterlockedAnd(&Ctx->Flags, ~TUN_FLAGS_PRESENT);
            ExReleaseSpinLockExclusive(
                &Ctx->TransitionLock,
                ExAcquireSpinLockExclusive(&Ctx->TransitionLock)); /* Ensure above change is visible to all readers. */
            break;

        case IRP_MN_CANCEL_REMOVE_DEVICE:
            InterlockedOr(&Ctx->Flags, TUN_FLAGS_PRESENT);
            break;
        }
    }

    return NdisDispatchPnP(DeviceObject, Irp);
}

static MINIPORT_RESTART TunRestart;
_Use_decl_annotations_
static NDIS_STATUS
TunRestart(NDIS_HANDLE MiniportAdapterContext, PNDIS_MINIPORT_RESTART_PARAMETERS MiniportRestartParameters)
{
    TUN_CTX *Ctx = (TUN_CTX *)MiniportAdapterContext;

    InterlockedOr(&Ctx->Flags, TUN_FLAGS_RUNNING);

    return NDIS_STATUS_SUCCESS;
}

static MINIPORT_PAUSE TunPause;
_Use_decl_annotations_
static NDIS_STATUS
TunPause(NDIS_HANDLE MiniportAdapterContext, PNDIS_MINIPORT_PAUSE_PARAMETERS MiniportPauseParameters)
{
    TUN_CTX *Ctx = (TUN_CTX *)MiniportAdapterContext;

    InterlockedAnd(&Ctx->Flags, ~TUN_FLAGS_RUNNING);
    ExReleaseSpinLockExclusive(
        &Ctx->TransitionLock,
        ExAcquireSpinLockExclusive(&Ctx->TransitionLock)); /* Ensure above change is visible to all readers. */

    return NDIS_STATUS_SUCCESS;
}

static MINIPORT_DEVICE_PNP_EVENT_NOTIFY TunDevicePnPEventNotify;
_Use_decl_annotations_
static void
TunDevicePnPEventNotify(NDIS_HANDLE MiniportAdapterContext, PNET_DEVICE_PNP_EVENT NetDevicePnPEvent)
{
}

static MINIPORT_INITIALIZE TunInitializeEx;
_Use_decl_annotations_
static NDIS_STATUS
TunInitializeEx(
    NDIS_HANDLE MiniportAdapterHandle,
    NDIS_HANDLE MiniportDriverContext,
    PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters)
{
    NDIS_STATUS Status;

    if (!MiniportAdapterHandle)
        return NDIS_STATUS_FAILURE;

    /* Register data device first. Having only one device per adapter allows us to store adapter context inside device
     * extension. */
    WCHAR DeviceName[sizeof(L"\\Device\\" TUN_DEVICE_NAME L"4294967295") / sizeof(WCHAR) + 1] = { 0 };
    UNICODE_STRING UnicodeDeviceName;
    TunInitUnicodeString(&UnicodeDeviceName, DeviceName);
    RtlUnicodeStringPrintf(
        &UnicodeDeviceName, L"\\Device\\" TUN_DEVICE_NAME, (ULONG)MiniportInitParameters->NetLuid.Info.NetLuidIndex);

    WCHAR SymbolicName[sizeof(L"\\DosDevices\\" TUN_DEVICE_NAME L"4294967295") / sizeof(WCHAR) + 1] = { 0 };
    UNICODE_STRING UnicodeSymbolicName;
    TunInitUnicodeString(&UnicodeSymbolicName, SymbolicName);
    RtlUnicodeStringPrintf(
        &UnicodeSymbolicName,
        L"\\DosDevices\\" TUN_DEVICE_NAME,
        (ULONG)MiniportInitParameters->NetLuid.Info.NetLuidIndex);

    static PDRIVER_DISPATCH DispatchTable[IRP_MJ_MAXIMUM_FUNCTION + 1] = {
        TunDispatch, /* IRP_MJ_CREATE                   */
        NULL,        /* IRP_MJ_CREATE_NAMED_PIPE        */
        TunDispatch, /* IRP_MJ_CLOSE                    */
        NULL,        /* IRP_MJ_READ                     */
        NULL,        /* IRP_MJ_WRITE                    */
        NULL,        /* IRP_MJ_QUERY_INFORMATION        */
        NULL,        /* IRP_MJ_SET_INFORMATION          */
        NULL,        /* IRP_MJ_QUERY_EA                 */
        NULL,        /* IRP_MJ_SET_EA                   */
        NULL,        /* IRP_MJ_FLUSH_BUFFERS            */
        NULL,        /* IRP_MJ_QUERY_VOLUME_INFORMATION */
        NULL,        /* IRP_MJ_SET_VOLUME_INFORMATION   */
        NULL,        /* IRP_MJ_DIRECTORY_CONTROL        */
        NULL,        /* IRP_MJ_FILE_SYSTEM_CONTROL      */
        TunDispatch, /* IRP_MJ_DEVICE_CONTROL           */
    };
    NDIS_DEVICE_OBJECT_ATTRIBUTES DeviceObjectAttributes = {
        .Header = { .Type = NDIS_OBJECT_TYPE_DEVICE_OBJECT_ATTRIBUTES,
                    .Revision = NDIS_DEVICE_OBJECT_ATTRIBUTES_REVISION_1,
                    .Size = NDIS_SIZEOF_DEVICE_OBJECT_ATTRIBUTES_REVISION_1 },
        .DeviceName = &UnicodeDeviceName,
        .SymbolicName = &UnicodeSymbolicName,
        .MajorFunctions = DispatchTable,
        .ExtensionSize = sizeof(TUN_CTX),
        .DefaultSDDLString = &SDDL_DEVOBJ_SYS_ALL /* Kernel, and SYSTEM: full control. Others: none */
    };
    NDIS_HANDLE DeviceObjectHandle;
    DEVICE_OBJECT *DeviceObject;
    if (!NT_SUCCESS(
            Status = NdisRegisterDeviceEx(
                NdisMiniportDriverHandle, &DeviceObjectAttributes, &DeviceObject, &DeviceObjectHandle)))
        return NDIS_STATUS_FAILURE;

    TUN_CTX *Ctx = NdisGetDeviceReservedExtension(DeviceObject);
    if (Status = NDIS_STATUS_FAILURE, !Ctx)
        goto cleanupDeregisterDevice;
    DEVICE_OBJECT *FunctionalDeviceObject;
    NdisMGetDeviceProperty(MiniportAdapterHandle, NULL, &FunctionalDeviceObject, NULL, NULL, NULL);

    /* Reverse engineering indicates that we'd be better off calling
     * NdisWdfGetAdapterContextFromAdapterHandle(functional_device),
     * which points to our TUN_CTX object directly, but this isn't
     * available before Windows 10, so for now we just stick it into
     * this reserved field. Revisit this when we drop support for old
     * Windows versions. */
#pragma warning(suppress : 28175)
    ASSERT(!FunctionalDeviceObject->Reserved);
#pragma warning(suppress : 28175)
    FunctionalDeviceObject->Reserved = Ctx;

    NdisZeroMemory(Ctx, sizeof(*Ctx));
    Ctx->MiniportAdapterHandle = MiniportAdapterHandle;

    Ctx->Statistics.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    Ctx->Statistics.Header.Revision = NDIS_STATISTICS_INFO_REVISION_1;
    Ctx->Statistics.Header.Size = NDIS_SIZEOF_STATISTICS_INFO_REVISION_1;
    Ctx->Statistics.SupportedStatistics =
        NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_RCV | NDIS_STATISTICS_FLAGS_VALID_MULTICAST_FRAMES_RCV |
        NDIS_STATISTICS_FLAGS_VALID_BROADCAST_FRAMES_RCV | NDIS_STATISTICS_FLAGS_VALID_BYTES_RCV |
        NDIS_STATISTICS_FLAGS_VALID_RCV_DISCARDS | NDIS_STATISTICS_FLAGS_VALID_RCV_ERROR |
        NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_XMIT | NDIS_STATISTICS_FLAGS_VALID_MULTICAST_FRAMES_XMIT |
        NDIS_STATISTICS_FLAGS_VALID_BROADCAST_FRAMES_XMIT | NDIS_STATISTICS_FLAGS_VALID_BYTES_XMIT |
        NDIS_STATISTICS_FLAGS_VALID_XMIT_ERROR | NDIS_STATISTICS_FLAGS_VALID_XMIT_DISCARDS |
        NDIS_STATISTICS_FLAGS_VALID_DIRECTED_BYTES_RCV | NDIS_STATISTICS_FLAGS_VALID_MULTICAST_BYTES_RCV |
        NDIS_STATISTICS_FLAGS_VALID_BROADCAST_BYTES_RCV | NDIS_STATISTICS_FLAGS_VALID_DIRECTED_BYTES_XMIT |
        NDIS_STATISTICS_FLAGS_VALID_MULTICAST_BYTES_XMIT | NDIS_STATISTICS_FLAGS_VALID_BROADCAST_BYTES_XMIT;

    Ctx->Device.Handle = DeviceObjectHandle;
    Ctx->Device.Object = DeviceObject;
    IoInitializeRemoveLock(&Ctx->Device.RemoveLock, TUN_MEMORY_TAG, 0, 0);
    KeInitializeSpinLock(&Ctx->Device.Send.Lock);

    NET_BUFFER_LIST_POOL_PARAMETERS NblPoolParameters = {
        .Header = { .Type = NDIS_OBJECT_TYPE_DEFAULT,
                    .Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1,
                    .Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1 },
        .ProtocolId = NDIS_PROTOCOL_ID_DEFAULT,
        .fAllocateNetBuffer = TRUE,
        .PoolTag = TUN_MEMORY_TAG
    };
/* Leaking memory 'Ctx->NblPool'. Note: 'Ctx->NblPool' is freed in TunHaltEx; or on failure. */
#pragma warning(suppress : 6014)
    Ctx->NblPool = NdisAllocateNetBufferListPool(MiniportAdapterHandle, &NblPoolParameters);
    if (Status = NDIS_STATUS_FAILURE, !Ctx->NblPool)
        goto cleanupDeregisterDevice;

    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES AdapterRegistrationAttributes = {
        .Header = { .Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES,
                    .Revision = NdisVersion < NDIS_RUNTIME_VERSION_630
                                    ? NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1
                                    : NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_2,
                    .Size = NdisVersion < NDIS_RUNTIME_VERSION_630
                                ? NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1
                                : NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_2 },
        .AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_NO_HALT_ON_SUSPEND | NDIS_MINIPORT_ATTRIBUTES_SURPRISE_REMOVE_OK,
        .InterfaceType = NdisInterfaceInternal,
        .MiniportAdapterContext = Ctx
    };
    if (Status = NDIS_STATUS_FAILURE,
        !NT_SUCCESS(NdisMSetMiniportAttributes(
            MiniportAdapterHandle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&AdapterRegistrationAttributes)))
        goto cleanupFreeNblPool;

    NDIS_PM_CAPABILITIES PmCapabilities = {
        .Header = { .Type = NDIS_OBJECT_TYPE_DEFAULT,
                    .Revision = NdisVersion < NDIS_RUNTIME_VERSION_630 ? NDIS_PM_CAPABILITIES_REVISION_1
                                                                       : NDIS_PM_CAPABILITIES_REVISION_2,
                    .Size = NdisVersion < NDIS_RUNTIME_VERSION_630 ? NDIS_SIZEOF_NDIS_PM_CAPABILITIES_REVISION_1
                                                                   : NDIS_SIZEOF_NDIS_PM_CAPABILITIES_REVISION_2 },
        .MinMagicPacketWakeUp = NdisDeviceStateUnspecified,
        .MinPatternWakeUp = NdisDeviceStateUnspecified,
        .MinLinkChangeWakeUp = NdisDeviceStateUnspecified
    };
    static NDIS_OID SupportedOids[] = { OID_GEN_MAXIMUM_TOTAL_SIZE,
                                        OID_GEN_CURRENT_LOOKAHEAD,
                                        OID_GEN_TRANSMIT_BUFFER_SPACE,
                                        OID_GEN_RECEIVE_BUFFER_SPACE,
                                        OID_GEN_TRANSMIT_BLOCK_SIZE,
                                        OID_GEN_RECEIVE_BLOCK_SIZE,
                                        OID_GEN_VENDOR_DESCRIPTION,
                                        OID_GEN_VENDOR_ID,
                                        OID_GEN_VENDOR_DRIVER_VERSION,
                                        OID_GEN_XMIT_OK,
                                        OID_GEN_RCV_OK,
                                        OID_GEN_CURRENT_PACKET_FILTER,
                                        OID_GEN_STATISTICS,
                                        OID_GEN_INTERRUPT_MODERATION,
                                        OID_GEN_LINK_PARAMETERS,
                                        OID_PNP_SET_POWER,
                                        OID_PNP_QUERY_POWER };
    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES AdapterGeneralAttributes = {
        .Header = { .Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES,
                    .Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2,
                    .Size = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2 },
        .MediaType = NdisMediumIP,
        .PhysicalMediumType = NdisPhysicalMediumUnspecified,
        .MtuSize = TUN_MAX_IP_PACKET_SIZE,
        .MaxXmitLinkSpeed = TUN_LINK_SPEED,
        .MaxRcvLinkSpeed = TUN_LINK_SPEED,
        .RcvLinkSpeed = TUN_LINK_SPEED,
        .XmitLinkSpeed = TUN_LINK_SPEED,
        .MediaConnectState = MediaConnectStateDisconnected,
        .LookaheadSize = TUN_MAX_IP_PACKET_SIZE,
        .MacOptions =
            NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_NO_LOOPBACK,
        .SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_ALL_MULTICAST |
                                  NDIS_PACKET_TYPE_BROADCAST | NDIS_PACKET_TYPE_ALL_LOCAL |
                                  NDIS_PACKET_TYPE_ALL_FUNCTIONAL,
        .AccessType = NET_IF_ACCESS_BROADCAST,
        .DirectionType = NET_IF_DIRECTION_SENDRECEIVE,
        .ConnectionType = NET_IF_CONNECTION_DEDICATED,
        .IfType = IF_TYPE_PROP_VIRTUAL,
        .IfConnectorPresent = FALSE,
        .SupportedStatistics = Ctx->Statistics.SupportedStatistics,
        .SupportedPauseFunctions = NdisPauseFunctionsUnsupported,
        .AutoNegotiationFlags =
            NDIS_LINK_STATE_XMIT_LINK_SPEED_AUTO_NEGOTIATED | NDIS_LINK_STATE_RCV_LINK_SPEED_AUTO_NEGOTIATED |
            NDIS_LINK_STATE_DUPLEX_AUTO_NEGOTIATED | NDIS_LINK_STATE_PAUSE_FUNCTIONS_AUTO_NEGOTIATED,
        .SupportedOidList = SupportedOids,
        .SupportedOidListLength = sizeof(SupportedOids),
        .PowerManagementCapabilitiesEx = &PmCapabilities
    };
    if (Status = NDIS_STATUS_FAILURE,
        !NT_SUCCESS(NdisMSetMiniportAttributes(
            MiniportAdapterHandle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&AdapterGeneralAttributes)))
        goto cleanupFreeNblPool;

    /* A miniport driver can call NdisMIndicateStatusEx after setting its
     * registration attributes even if the driver is still in the context
     * of the MiniportInitializeEx function. */
    TunIndicateStatus(Ctx->MiniportAdapterHandle, MediaConnectStateDisconnected);
    InterlockedIncrement64(&TunAdapterCount);
    InterlockedOr(&Ctx->Flags, TUN_FLAGS_PRESENT);
    return NDIS_STATUS_SUCCESS;

cleanupFreeNblPool:
    NdisFreeNetBufferListPool(Ctx->NblPool);
cleanupDeregisterDevice:
    NdisDeregisterDeviceEx(DeviceObjectHandle);
    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
static NTSTATUS
TunDeviceSetDenyAllDacl(_In_ DEVICE_OBJECT *DeviceObject)
{
    NTSTATUS Status;
    SECURITY_DESCRIPTOR Sd;
    ACL Acl;
    HANDLE DeviceObjectHandle;

    if (!NT_SUCCESS(Status = RtlCreateSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION)) ||
        !NT_SUCCESS(Status = RtlCreateAcl(&Acl, sizeof(ACL), ACL_REVISION)) ||
        !NT_SUCCESS(Status = RtlSetDaclSecurityDescriptor(&Sd, TRUE, &Acl, FALSE)) ||
        !NT_SUCCESS(
            Status = ObOpenObjectByPointer(
                DeviceObject,
                OBJ_KERNEL_HANDLE,
                NULL,
                WRITE_DAC,
                *IoDeviceObjectType,
                KernelMode,
                &DeviceObjectHandle)))
        return Status;

    Status = ZwSetSecurityObject(DeviceObjectHandle, DACL_SECURITY_INFORMATION, &Sd);
    ZwClose(DeviceObjectHandle);
    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
static void
TunForceHandlesClosed(_Inout_ TUN_CTX *Ctx)
{
    NTSTATUS Status;
    PEPROCESS Process;
    KAPC_STATE ApcState;
    PVOID Object = NULL;
    ULONG VerifierFlags = 0;
    OBJECT_HANDLE_INFORMATION HandleInfo;
    SYSTEM_HANDLE_INFORMATION_EX *HandleTable = NULL;

    MmIsVerifierEnabled(&VerifierFlags);

    for (ULONG Size = 0, RequestedSize;
         (Status = ZwQuerySystemInformation(SystemExtendedHandleInformation, HandleTable, Size, &RequestedSize)) ==
         STATUS_INFO_LENGTH_MISMATCH;
         Size = RequestedSize)
    {
        if (HandleTable)
            ExFreePoolWithTag(HandleTable, TUN_MEMORY_TAG);
        HandleTable = ExAllocatePoolWithTag(PagedPool, RequestedSize, TUN_MEMORY_TAG);
        if (!HandleTable)
            return;
    }
    if (!NT_SUCCESS(Status) || !HandleTable)
        goto cleanup;

    for (ULONG_PTR Index = 0; Index < HandleTable->NumberOfHandles; ++Index)
    {
        /* XXX: We should perhaps first look at table->Handles[i].ObjectTypeIndex, but
         * the value changes lots between NT versions, and it should be implicit anyway. */
        FILE_OBJECT *FileObject = HandleTable->Handles[Index].Object;
        if (!FileObject || FileObject->Type != 5 || FileObject->DeviceObject != Ctx->Device.Object)
            continue;
        Status = PsLookupProcessByProcessId(HandleTable->Handles[Index].UniqueProcessId, &Process);
        if (!NT_SUCCESS(Status))
            continue;
        KeStackAttachProcess(Process, &ApcState);
        if (!VerifierFlags)
            Status = ObReferenceObjectByHandle(
                HandleTable->Handles[Index].HandleValue, 0, NULL, UserMode, &Object, &HandleInfo);
        if (NT_SUCCESS(Status))
        {
            if (VerifierFlags || Object == FileObject)
                ObCloseHandle(HandleTable->Handles[Index].HandleValue, UserMode);
            if (!VerifierFlags)
                ObfDereferenceObject(Object);
        }
        KeUnstackDetachProcess(&ApcState);
        ObfDereferenceObject(Process);
    }
cleanup:
    if (HandleTable)
        ExFreePoolWithTag(HandleTable, TUN_MEMORY_TAG);
}

_IRQL_requires_max_(APC_LEVEL)
static void
TunWaitForReferencesToDropToZero(_In_ DEVICE_OBJECT *DeviceObject)
{
    /* The sleep loop isn't pretty, but we don't have a choice. This is an NDIS bug we're working around. */
    enum
    {
        SleepTime = 50,
        TotalTime = 2 * 60 * 1000,
        MaxTries = TotalTime / SleepTime
    };
#pragma warning(suppress : 28175)
    for (INT Try = 0; Try < MaxTries && InterlockedGet(&DeviceObject->ReferenceCount); ++Try)
        NdisMSleep(SleepTime);
}

static MINIPORT_HALT TunHaltEx;
_Use_decl_annotations_
static void
TunHaltEx(NDIS_HANDLE MiniportAdapterContext, NDIS_HALT_ACTION HaltAction)
{
    TUN_CTX *Ctx = (TUN_CTX *)MiniportAdapterContext;

    InterlockedAnd(&Ctx->Flags, ~TUN_FLAGS_PRESENT);
    ExReleaseSpinLockExclusive(
        &Ctx->TransitionLock,
        ExAcquireSpinLockExclusive(&Ctx->TransitionLock)); /* Ensure above change is visible to all readers. */

    /* Setting a deny-all DACL we prevent userspace to open the data device by symlink after TunForceHandlesClosed(). */
    TunDeviceSetDenyAllDacl(Ctx->Device.Object);
    TunForceHandlesClosed(Ctx);

    /* Wait for processing IRP(s) to complete. */
    IoAcquireRemoveLock(&Ctx->Device.RemoveLock, NULL);
    IoReleaseRemoveLockAndWait(&Ctx->Device.RemoveLock, NULL);
    NdisFreeNetBufferListPool(Ctx->NblPool);

    /* MiniportAdapterHandle must not be used in TunDispatch(). After TunHaltEx() returns it is invalidated. */
    InterlockedExchangePointer(&Ctx->MiniportAdapterHandle, NULL);

    ASSERT(InterlockedGet64(&TunAdapterCount) > 0);
    if (InterlockedDecrement64(&TunAdapterCount) <= 0)
        TunWaitForReferencesToDropToZero(Ctx->Device.Object);

    /* Deregister data device _after_ we are done using Ctx not to risk an UaF. The Ctx is hosted by device extension.
     */
    NdisDeregisterDeviceEx(Ctx->Device.Handle);
}

static MINIPORT_SHUTDOWN TunShutdownEx;
_Use_decl_annotations_
static void
TunShutdownEx(NDIS_HANDLE MiniportAdapterContext, NDIS_SHUTDOWN_ACTION ShutdownAction)
{
}

_IRQL_requires_max_(APC_LEVEL)
_Must_inspect_result_
static NDIS_STATUS
TunOidQueryWrite(_Inout_ NDIS_OID_REQUEST *OidRequest, _In_ ULONG Value)
{
    if (OidRequest->DATA.QUERY_INFORMATION.InformationBufferLength < sizeof(ULONG))
    {
        OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = sizeof(ULONG);
        OidRequest->DATA.QUERY_INFORMATION.BytesWritten = 0;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }

    OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = OidRequest->DATA.QUERY_INFORMATION.BytesWritten = sizeof(ULONG);
    *(ULONG *)OidRequest->DATA.QUERY_INFORMATION.InformationBuffer = Value;
    return NDIS_STATUS_SUCCESS;
}

_IRQL_requires_max_(APC_LEVEL)
_Must_inspect_result_
static NDIS_STATUS
TunOidQueryWrite32or64(_Inout_ NDIS_OID_REQUEST *OidRequest, _In_ ULONG64 Value)
{
    if (OidRequest->DATA.QUERY_INFORMATION.InformationBufferLength < sizeof(ULONG))
    {
        OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = sizeof(ULONG64);
        OidRequest->DATA.QUERY_INFORMATION.BytesWritten = 0;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }

    if (OidRequest->DATA.QUERY_INFORMATION.InformationBufferLength < sizeof(ULONG64))
    {
        OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = sizeof(ULONG64);
        OidRequest->DATA.QUERY_INFORMATION.BytesWritten = sizeof(ULONG);
        *(ULONG *)OidRequest->DATA.QUERY_INFORMATION.InformationBuffer = (ULONG)(Value & 0xffffffff);
        return NDIS_STATUS_SUCCESS;
    }

    OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = OidRequest->DATA.QUERY_INFORMATION.BytesWritten = sizeof(ULONG64);
    *(ULONG64 *)OidRequest->DATA.QUERY_INFORMATION.InformationBuffer = Value;
    return NDIS_STATUS_SUCCESS;
}

_IRQL_requires_max_(APC_LEVEL)
_Must_inspect_result_
static NDIS_STATUS
TunOidQueryWriteBuf(_Inout_ NDIS_OID_REQUEST *OidRequest, _In_bytecount_(Size) const void *Buf, _In_ ULONG Size)
{
    if (OidRequest->DATA.QUERY_INFORMATION.InformationBufferLength < Size)
    {
        OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = Size;
        OidRequest->DATA.QUERY_INFORMATION.BytesWritten = 0;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }

    OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = OidRequest->DATA.QUERY_INFORMATION.BytesWritten = Size;
    NdisMoveMemory(OidRequest->DATA.QUERY_INFORMATION.InformationBuffer, Buf, Size);
    return NDIS_STATUS_SUCCESS;
}

_IRQL_requires_max_(APC_LEVEL)
_Must_inspect_result_
static NDIS_STATUS
TunOidQuery(_Inout_ TUN_CTX *Ctx, _Inout_ NDIS_OID_REQUEST *OidRequest)
{
    ASSERT(
        OidRequest->RequestType == NdisRequestQueryInformation ||
        OidRequest->RequestType == NdisRequestQueryStatistics);

    switch (OidRequest->DATA.QUERY_INFORMATION.Oid)
    {
    case OID_GEN_MAXIMUM_TOTAL_SIZE:
    case OID_GEN_TRANSMIT_BLOCK_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE:
        return TunOidQueryWrite(OidRequest, TUN_MAX_IP_PACKET_SIZE);

    case OID_GEN_TRANSMIT_BUFFER_SPACE:
        return TunOidQueryWrite(OidRequest, TUN_MAX_RING_CAPACITY);

    case OID_GEN_RECEIVE_BUFFER_SPACE:
        return TunOidQueryWrite(OidRequest, TUN_MAX_RING_CAPACITY);

    case OID_GEN_VENDOR_ID:
        return TunOidQueryWrite(OidRequest, TUN_HTONL(TUN_VENDOR_ID));

    case OID_GEN_VENDOR_DESCRIPTION:
        return TunOidQueryWriteBuf(OidRequest, TUN_VENDOR_NAME, (ULONG)sizeof(TUN_VENDOR_NAME));

    case OID_GEN_VENDOR_DRIVER_VERSION:
        return TunOidQueryWrite(OidRequest, (WINTUN_VERSION_MAJ << 16) | WINTUN_VERSION_MIN);

    case OID_GEN_XMIT_OK:
        return TunOidQueryWrite32or64(
            OidRequest,
            InterlockedGet64((LONG64 *)&Ctx->Statistics.ifHCOutUcastPkts) +
                InterlockedGet64((LONG64 *)&Ctx->Statistics.ifHCOutMulticastPkts) +
                InterlockedGet64((LONG64 *)&Ctx->Statistics.ifHCOutBroadcastPkts));

    case OID_GEN_RCV_OK:
        return TunOidQueryWrite32or64(
            OidRequest,
            InterlockedGet64((LONG64 *)&Ctx->Statistics.ifHCInUcastPkts) +
                InterlockedGet64((LONG64 *)&Ctx->Statistics.ifHCInMulticastPkts) +
                InterlockedGet64((LONG64 *)&Ctx->Statistics.ifHCInBroadcastPkts));

    case OID_GEN_STATISTICS:
        return TunOidQueryWriteBuf(OidRequest, &Ctx->Statistics, (ULONG)sizeof(Ctx->Statistics));

    case OID_GEN_INTERRUPT_MODERATION: {
        static const NDIS_INTERRUPT_MODERATION_PARAMETERS InterruptParameters = {
            .Header = { .Type = NDIS_OBJECT_TYPE_DEFAULT,
                        .Revision = NDIS_INTERRUPT_MODERATION_PARAMETERS_REVISION_1,
                        .Size = NDIS_SIZEOF_INTERRUPT_MODERATION_PARAMETERS_REVISION_1 },
            .InterruptModeration = NdisInterruptModerationNotSupported
        };
        return TunOidQueryWriteBuf(OidRequest, &InterruptParameters, (ULONG)sizeof(InterruptParameters));
    }

    case OID_PNP_QUERY_POWER:
        OidRequest->DATA.QUERY_INFORMATION.BytesNeeded = OidRequest->DATA.QUERY_INFORMATION.BytesWritten = 0;
        return NDIS_STATUS_SUCCESS;
    }

    OidRequest->DATA.QUERY_INFORMATION.BytesWritten = 0;
    return NDIS_STATUS_NOT_SUPPORTED;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
static NDIS_STATUS
TunOidSet(_Inout_ TUN_CTX *Ctx, _Inout_ NDIS_OID_REQUEST *OidRequest)
{
    ASSERT(OidRequest->RequestType == NdisRequestSetInformation);

    OidRequest->DATA.SET_INFORMATION.BytesNeeded = OidRequest->DATA.SET_INFORMATION.BytesRead = 0;

    switch (OidRequest->DATA.SET_INFORMATION.Oid)
    {
    case OID_GEN_CURRENT_PACKET_FILTER:
    case OID_GEN_CURRENT_LOOKAHEAD:
        if (OidRequest->DATA.SET_INFORMATION.InformationBufferLength != 4)
        {
            OidRequest->DATA.SET_INFORMATION.BytesNeeded = 4;
            return NDIS_STATUS_INVALID_LENGTH;
        }
        OidRequest->DATA.SET_INFORMATION.BytesRead = 4;
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_LINK_PARAMETERS:
        OidRequest->DATA.SET_INFORMATION.BytesRead = OidRequest->DATA.SET_INFORMATION.InformationBufferLength;
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_INTERRUPT_MODERATION:
        return NDIS_STATUS_INVALID_DATA;

    case OID_PNP_SET_POWER:
        if (OidRequest->DATA.SET_INFORMATION.InformationBufferLength != sizeof(NDIS_DEVICE_POWER_STATE))
        {
            OidRequest->DATA.SET_INFORMATION.BytesNeeded = sizeof(NDIS_DEVICE_POWER_STATE);
            return NDIS_STATUS_INVALID_LENGTH;
        }
        OidRequest->DATA.SET_INFORMATION.BytesRead = sizeof(NDIS_DEVICE_POWER_STATE);
        return NDIS_STATUS_SUCCESS;
    }

    return NDIS_STATUS_NOT_SUPPORTED;
}

static MINIPORT_OID_REQUEST TunOidRequest;
_Use_decl_annotations_
static NDIS_STATUS
TunOidRequest(NDIS_HANDLE MiniportAdapterContext, PNDIS_OID_REQUEST OidRequest)
{
    switch (OidRequest->RequestType)
    {
    case NdisRequestQueryInformation:
    case NdisRequestQueryStatistics:
        return TunOidQuery(MiniportAdapterContext, OidRequest);

    case NdisRequestSetInformation:
        return TunOidSet(MiniportAdapterContext, OidRequest);

    default:
        return NDIS_STATUS_INVALID_OID;
    }
}

static MINIPORT_CANCEL_OID_REQUEST TunCancelOidRequest;
_Use_decl_annotations_
static void
TunCancelOidRequest(NDIS_HANDLE MiniportAdapterContext, PVOID RequestId)
{
}

static MINIPORT_DIRECT_OID_REQUEST TunDirectOidRequest;
_Use_decl_annotations_
static NDIS_STATUS
TunDirectOidRequest(NDIS_HANDLE MiniportAdapterContext, PNDIS_OID_REQUEST OidRequest)
{
    switch (OidRequest->RequestType)
    {
    case NdisRequestQueryInformation:
    case NdisRequestQueryStatistics:
    case NdisRequestSetInformation:
        return NDIS_STATUS_NOT_SUPPORTED;

    default:
        return NDIS_STATUS_INVALID_OID;
    }
}

static MINIPORT_CANCEL_DIRECT_OID_REQUEST TunCancelDirectOidRequest;
_Use_decl_annotations_
static void
TunCancelDirectOidRequest(NDIS_HANDLE MiniportAdapterContext, PVOID RequestId)
{
}

static MINIPORT_SYNCHRONOUS_OID_REQUEST TunSynchronousOidRequest;
_Use_decl_annotations_
static NDIS_STATUS
TunSynchronousOidRequest(NDIS_HANDLE MiniportAdapterContext, PNDIS_OID_REQUEST OidRequest)
{
    switch (OidRequest->RequestType)
    {
    case NdisRequestQueryInformation:
    case NdisRequestQueryStatistics:
    case NdisRequestSetInformation:
        return NDIS_STATUS_NOT_SUPPORTED;

    default:
        return NDIS_STATUS_INVALID_OID;
    }
}

static MINIPORT_UNLOAD TunUnload;
_Use_decl_annotations_
static VOID
TunUnload(PDRIVER_OBJECT DriverObject)
{
    NdisMDeregisterMiniportDriver(NdisMiniportDriverHandle);
}

DRIVER_INITIALIZE DriverEntry;
_Use_decl_annotations_
NTSTATUS
DriverEntry(DRIVER_OBJECT *DriverObject, UNICODE_STRING *RegistryPath)
{
    NTSTATUS Status;

    NdisVersion = NdisGetVersion();
    if (NdisVersion < NDIS_MINIPORT_VERSION_MIN)
        return NDIS_STATUS_UNSUPPORTED_REVISION;
    if (NdisVersion > NDIS_MINIPORT_VERSION_MAX)
        NdisVersion = NDIS_MINIPORT_VERSION_MAX;

    NDIS_MINIPORT_DRIVER_CHARACTERISTICS miniport = {
        .Header = { .Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS,
                    .Revision = NdisVersion < NDIS_RUNTIME_VERSION_680
                                    ? NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2
                                    : NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_3,
                    .Size = NdisVersion < NDIS_RUNTIME_VERSION_680
                                ? NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2
                                : NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_3 },

        .MajorNdisVersion = (UCHAR)((NdisVersion & 0x00ff0000) >> 16),
        .MinorNdisVersion = (UCHAR)(NdisVersion & 0x000000ff),

        .MajorDriverVersion = WINTUN_VERSION_MAJ,
        .MinorDriverVersion = WINTUN_VERSION_MIN,

        .InitializeHandlerEx = TunInitializeEx,
        .HaltHandlerEx = TunHaltEx,
        .UnloadHandler = TunUnload,
        .PauseHandler = TunPause,
        .RestartHandler = TunRestart,
        .OidRequestHandler = TunOidRequest,
        .SendNetBufferListsHandler = TunSendNetBufferLists,
        .ReturnNetBufferListsHandler = TunReturnNetBufferLists,
        .CancelSendHandler = TunCancelSend,
        .DevicePnPEventNotifyHandler = TunDevicePnPEventNotify,
        .ShutdownHandlerEx = TunShutdownEx,
        .CancelOidRequestHandler = TunCancelOidRequest,
        .DirectOidRequestHandler = TunDirectOidRequest,
        .CancelDirectOidRequestHandler = TunCancelDirectOidRequest,
        .SynchronousOidRequestHandler = TunSynchronousOidRequest
    };
    Status = NdisMRegisterMiniportDriver(DriverObject, RegistryPath, NULL, &miniport, &NdisMiniportDriverHandle);
    if (!NT_SUCCESS(Status))
        return Status;

    NdisDispatchPnP = DriverObject->MajorFunction[IRP_MJ_PNP];
    DriverObject->MajorFunction[IRP_MJ_PNP] = TunDispatchPnP;

    return STATUS_SUCCESS;
}

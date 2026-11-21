/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     LPC: local procedure call ports
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * The classic NT 5 port model, which the boot media's ntdll, smss and
 * csrss speak:
 *  - a server creates a named connection port and waits on it;
 *  - a client connects by name: its thread queues a connection request
 *    (with connection data and optional shared-section views) and sleeps;
 *    the server accepts or refuses it, then completes the connection,
 *    which wakes the client with its end of a pair of communication ports;
 *  - messages sent on a client's port arrive in the server connection
 *    port's queue, tagged with the context the server gave that
 *    connection; requests carry a message ID, and the reply goes straight
 *    to the thread waiting for it. Messages sent the other way (server to
 *    client) queue on the client's port.
 *  - closing either end disconnects the pair; the server hears
 *    LPC_PORT_CLOSED, and threads still waiting get
 *    STATUS_PORT_DISCONNECTED.
 * Messages are copied into kernel memory, so a sender's buffer is free as
 * soon as the call returns.
 */

#include "qzio.h"
#include "qzps.h"

#define QZ_LPC_TAG                  'cpLQ'
#define QZ_LPC_MAX_MESSAGE          0x1000     /* header + data */
#define QZ_LPC_MAX_CONNECTION_INFO  0x1000

#define QZ_PORT_SERVER_CONNECTION   1
#define QZ_PORT_SERVER_COMMUNICATION 2
#define QZ_PORT_CLIENT_COMMUNICATION 3

typedef struct _QZ_PORT
{
    ULONG Kind;
    BOOLEAN Waitable;
    BOOLEAN Closed;                     /* the owner closed it, or its peer went */
    struct _QZ_PORT *ConnectionPort;    /* communication ports: the server's port (referenced) */
    struct _QZ_PORT *Peer;              /* the other end of a connection (not referenced) */
    PVOID PortContext;                  /* the server's, for messages from this connection */
    LIST_ENTRY Queue;                   /* messages to receive (connection & client ports) */
    KSEMAPHORE QueueSemaphore;
    KEVENT WaitEvent;                   /* waitable ports: signaled while messages wait */
    ULONG MaxMessageLength;
    ULONG MaxConnectionInfoLength;
    PQZ_PROCESS Owner;
} QZ_PORT, *PQZ_PORT;

typedef struct _QZ_LPC_MESSAGE
{
    LIST_ENTRY Link;
    PQZ_PORT SenderPort;                /* the communication port it came through (referenced) */
    PQZ_PS_THREAD Sender;               /* requests: the thread waiting for the reply (referenced) */
    PORT_MESSAGE Header;                /* then the data */
} QZ_LPC_MESSAGE, *PQZ_LPC_MESSAGE;

/* What a thread waiting in NtRequestWaitReplyPort / NtConnectPort gets back */
typedef struct _QZ_LPC_WAIT
{
    LIST_ENTRY Link;                    /* QzLpcWaits */
    PQZ_PS_THREAD Thread;
    ULONG MessageId;
    PQZ_PORT Port;                      /* the port the request went to */
    KEVENT Done;
    NTSTATUS Status;
    PQZ_LPC_MESSAGE Reply;
    /* Connections */
    PQZ_PORT ClientPort;                /* the client's end (referenced) */
    PQZ_SECTION ClientSection;          /* the client's view section (referenced) */
    LARGE_INTEGER ClientSectionOffset;
    SIZE_T ClientViewSize;
    PQZ_PORT ServerPort;
    PQZ_SECTION ServerSection;          /* the server's view section, to map in the client */
    LARGE_INTEGER ServerSectionOffset;
    SIZE_T ServerViewSize;
    PVOID ServerViewInServer;
    PVOID ClientViewInServer;
    BOOLEAN Accepted;
} QZ_LPC_WAIT, *PQZ_LPC_WAIT;

POBJECT_TYPE LpcPortObjectType;
POBJECT_TYPE LpcWaitablePortObjectType;
static LIST_ENTRY QzLpcWaits = { &QzLpcWaits, &QzLpcWaits };
static FAST_MUTEX QzLpcLock;
static LONG QzLpcNextMessageId;

/* ---------------------------------------------------------------------- */
/* Messages                                                                */
/* ---------------------------------------------------------------------- */

static PQZ_LPC_MESSAGE
QzLpcAllocateMessage(ULONG DataLength)
{
    PQZ_LPC_MESSAGE Message = ExAllocatePoolWithTag(NonPagedPool, sizeof(QZ_LPC_MESSAGE) + DataLength,
                                                    QZ_LPC_TAG);
    if (Message)
        RtlZeroMemory(Message, sizeof(QZ_LPC_MESSAGE));
    return Message;
}

static VOID
QzLpcFreeMessage(PQZ_LPC_MESSAGE Message)
{
    if (Message->SenderPort)
        ObDereferenceObject(Message->SenderPort);
    if (Message->Sender)
        ObDereferenceObject(Message->Sender);
    ExFreePoolWithTag(Message, QZ_LPC_TAG);
}

/* Copy a caller's PORT_MESSAGE (header + data) into a new kernel message */
static NTSTATUS
QzLpcCaptureMessage(KPROCESSOR_MODE Mode, PPORT_MESSAGE From, ULONG MaxLength, PQZ_LPC_MESSAGE *Result)
{
    PORT_MESSAGE Header;
    PQZ_LPC_MESSAGE Message;
    ULONG DataLength;

    if (!QzReadArg(Mode, &Header, From, sizeof(Header)))
        return STATUS_ACCESS_VIOLATION;
    DataLength = (USHORT)Header.u1.s1.DataLength;
    if ((ULONG)(USHORT)Header.u1.s1.TotalLength < sizeof(PORT_MESSAGE) + DataLength ||
        sizeof(PORT_MESSAGE) + DataLength > MaxLength)
        return STATUS_PORT_MESSAGE_TOO_LONG;
    Message = QzLpcAllocateMessage(DataLength);
    if (!Message)
        return STATUS_NO_MEMORY;
    Message->Header = Header;
    Message->Header.u1.s1.TotalLength = (CSHORT)(sizeof(PORT_MESSAGE) + DataLength);
    if (DataLength && !QzReadArg(Mode, &Message->Header + 1, From + 1, DataLength))
    {
        QzLpcFreeMessage(Message);
        return STATUS_ACCESS_VIOLATION;
    }
    *Result = Message;
    return STATUS_SUCCESS;
}

static BOOLEAN
QzLpcWriteMessage(KPROCESSOR_MODE Mode, PPORT_MESSAGE To, PQZ_LPC_MESSAGE Message)
{
    return QzWriteArg(Mode, To, &Message->Header, Message->Header.u1.s1.TotalLength);
}

static VOID
QzLpcSetSender(PPORT_MESSAGE Header)
{
    PQZ_PS_THREAD Thread = QzCurrentThread->PsThread;
    Header->ClientId.UniqueProcess = PsGetCurrentProcessId();
    Header->ClientId.UniqueThread = Thread ? (HANDLE)(ULONG_PTR)Thread->UniqueId : PsGetCurrentThreadId();
}

/* Queue a message for whoever receives on Port */
static VOID
QzLpcQueue(PQZ_PORT Port, PQZ_LPC_MESSAGE Message)
{
    ExAcquireFastMutex(&QzLpcLock);
    InsertTailList(&Port->Queue, &Message->Link);
    if (Port->Waitable)
        KeSetEvent(&Port->WaitEvent, IO_NO_INCREMENT, FALSE);
    ExReleaseFastMutex(&QzLpcLock);
    KeReleaseSemaphore(&Port->QueueSemaphore, IO_NO_INCREMENT, 1, FALSE);
}

/* Where messages for the peer of Port go */
static PQZ_PORT
QzLpcTargetQueue(PQZ_PORT Port)
{
    if (Port->Kind == QZ_PORT_CLIENT_COMMUNICATION)
        return Port->ConnectionPort;     /* to the server's connection port */
    if (Port->Kind == QZ_PORT_SERVER_COMMUNICATION)
        return Port->Peer;               /* to the client's own port */
    return Port;
}

/* ---------------------------------------------------------------------- */
/* Port objects                                                            */
/* ---------------------------------------------------------------------- */

/* Wake every thread waiting for a reply through Port */
static VOID
QzLpcFailWaiters(PQZ_PORT Port)
{
    PLIST_ENTRY Entry, Next;

    ExAcquireFastMutex(&QzLpcLock);
    for (Entry = QzLpcWaits.Flink; Entry != &QzLpcWaits; Entry = Next)
    {
        PQZ_LPC_WAIT Wait = CONTAINING_RECORD(Entry, QZ_LPC_WAIT, Link);
        Next = Entry->Flink;
        if (Wait->Port == Port || (Port->Peer && Wait->Port == Port->Peer))
        {
            RemoveEntryList(&Wait->Link);
            InitializeListHead(&Wait->Link);
            Wait->Status = STATUS_PORT_DISCONNECTED;
            KeSetEvent(&Wait->Done, IO_NO_INCREMENT, FALSE);
        }
    }
    ExReleaseFastMutex(&QzLpcLock);
}

/* The last handle to a port went: disconnect it */
static VOID NTAPI
QzLpcClosePort(PEPROCESS Process, PVOID Object, ACCESS_MASK GrantedAccess, ULONG ProcessHandleCount,
               ULONG SystemHandleCount)
{
    PQZ_PORT Port = Object, Peer;

    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(GrantedAccess);
    UNREFERENCED_PARAMETER(ProcessHandleCount);
    if (SystemHandleCount != 1)
        return;

    ExAcquireFastMutex(&QzLpcLock);
    Port->Closed = TRUE;
    Peer = Port->Peer;
    if (Peer)
    {
        Peer->Peer = NULL;
        Peer->Closed = TRUE;
        ObReferenceObject(Peer);
    }
    Port->Peer = NULL;
    ExReleaseFastMutex(&QzLpcLock);

    QzLpcFailWaiters(Port);
    if (Peer)
    {
        /* Tell the server its client went (or the client, its server) */
        PQZ_PORT Server = (Port->Kind == QZ_PORT_CLIENT_COMMUNICATION) ? Port->ConnectionPort : NULL;
        if (Server && !Server->Closed)
        {
            PQZ_LPC_MESSAGE Message = QzLpcAllocateMessage(sizeof(LARGE_INTEGER));
            if (Message)
            {
                Message->Header.u1.s1.DataLength = sizeof(LARGE_INTEGER);
                Message->Header.u1.s1.TotalLength = sizeof(PORT_MESSAGE) + sizeof(LARGE_INTEGER);
                Message->Header.u2.s2.Type = LPC_PORT_CLOSED;
                Message->Header.MessageId = (ULONG)InterlockedIncrement(&QzLpcNextMessageId);
                Message->Header.ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)(Port->Owner ? Port->Owner->UniqueId : 4);
                KeQuerySystemTime((PLARGE_INTEGER)(&Message->Header + 1));
                Message->SenderPort = Peer;   /* the reference goes with the message */
                QzLpcQueue(Server, Message);
                Peer = NULL;
            }
        }
        if (Peer)
        {
            QzLpcFailWaiters(Peer);
            ObDereferenceObject(Peer);
        }
    }
}

static VOID NTAPI
QzLpcDeletePort(PVOID Object)
{
    PQZ_PORT Port = Object;

    while (!IsListEmpty(&Port->Queue))
        QzLpcFreeMessage(CONTAINING_RECORD(RemoveHeadList(&Port->Queue), QZ_LPC_MESSAGE, Link));
    if (Port->ConnectionPort)
        ObDereferenceObject(Port->ConnectionPort);
}

static NTSTATUS
QzLpcCreatePortObject(KPROCESSOR_MODE Mode, POBJECT_ATTRIBUTES Attributes, ULONG Kind, BOOLEAN Waitable,
                      PQZ_PORT *Result)
{
    PQZ_PORT Port;
    NTSTATUS Status = ObCreateObject(Mode, Waitable ? LpcWaitablePortObjectType : LpcPortObjectType, Attributes,
                                     Mode, NULL, sizeof(QZ_PORT), 0, 0, (PVOID *)&Port);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlZeroMemory(Port, sizeof(*Port));
    Port->Kind = Kind;
    Port->Waitable = Waitable;
    InitializeListHead(&Port->Queue);
    KeInitializeSemaphore(&Port->QueueSemaphore, 0, MAXLONG);
    KeInitializeEvent(&Port->WaitEvent, NotificationEvent, FALSE);
    Port->Owner = QzCurrentThread->Process;
    *Result = Port;
    return STATUS_SUCCESS;
}

VOID
QzLpcInit(VOID)
{
    static GENERIC_MAPPING Mapping =
    {
        READ_CONTROL | PORT_CONNECT, DELETE | PORT_CONNECT, 0, PORT_ALL_ACCESS
    };
    UNICODE_STRING Name;
    OBJECT_TYPE_INITIALIZER Init;

    ExInitializeFastMutex(&QzLpcLock);
    RtlZeroMemory(&Init, sizeof(Init));
    Init.Length = sizeof(Init);
    Init.GenericMapping = Mapping;
    Init.ValidAccessMask = PORT_ALL_ACCESS;
    Init.PoolType = NonPagedPool;
    Init.MaintainHandleCount = TRUE;
    Init.CloseProcedure = QzLpcClosePort;
    Init.DeleteProcedure = QzLpcDeletePort;
    Init.UseDefaultObject = TRUE;
    RtlInitUnicodeString(&Name, L"Port");
    if (!NT_SUCCESS(ObCreateObjectType(&Name, &Init, NULL, &LpcPortObjectType)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, 4, 0, 0, 0);
    /* Waitable ports: waits use their event */
    Init.UseDefaultObject = FALSE;
    RtlInitUnicodeString(&Name, L"WaitablePort");
    if (!NT_SUCCESS(ObCreateObjectType(&Name, &Init, NULL, &LpcWaitablePortObjectType)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, 5, 0, 0, 0);
    LpcWaitablePortObjectType->DefaultObject = (PVOID)FIELD_OFFSET(QZ_PORT, WaitEvent);
}

static NTSTATUS
QzLpcReferencePort(HANDLE Handle, ACCESS_MASK Access, PQZ_PORT *Port)
{
    NTSTATUS Status = ObReferenceObjectByHandle(Handle, Access, LpcPortObjectType, ExGetPreviousMode(),
                                                (PVOID *)Port, NULL);
    if (Status == STATUS_OBJECT_TYPE_MISMATCH)
        Status = ObReferenceObjectByHandle(Handle, Access, LpcWaitablePortObjectType, ExGetPreviousMode(),
                                           (PVOID *)Port, NULL);
    return Status;
}

/* ---------------------------------------------------------------------- */
/* Creating and connecting                                                 */
/* ---------------------------------------------------------------------- */

static NTSTATUS
QzLpcCreatePort(PHANDLE PortHandle, POBJECT_ATTRIBUTES ObjectAttributes, ULONG MaxConnectionInfoLength,
                ULONG MaxMessageLength, BOOLEAN Waitable)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PQZ_PORT Port;
    HANDLE Handle;
    NTSTATUS Status;

    if (MaxConnectionInfoLength > QZ_LPC_MAX_CONNECTION_INFO || MaxMessageLength > QZ_LPC_MAX_MESSAGE)
        return STATUS_INVALID_PARAMETER_3;
    Status = QzLpcCreatePortObject(Mode, ObjectAttributes, QZ_PORT_SERVER_CONNECTION, Waitable, &Port);
    if (!NT_SUCCESS(Status))
        return Status;
    Port->MaxConnectionInfoLength = MaxConnectionInfoLength;
    Port->MaxMessageLength = max(MaxMessageLength, sizeof(PORT_MESSAGE) + 0x100);
    Status = ObInsertObject(Port, NULL, PORT_ALL_ACCESS, 0, NULL, &Handle);
    if (NT_SUCCESS(Status) && !QzWriteArg(Mode, PortHandle, &Handle, sizeof(Handle)))
        Status = STATUS_ACCESS_VIOLATION;
    return Status;
}

NTSTATUS
NTAPI
NtCreatePort(OUT PHANDLE PortHandle, IN POBJECT_ATTRIBUTES ObjectAttributes, IN ULONG MaxConnectInfoLength,
             IN ULONG MaxDataLength, IN ULONG MaxPoolUsage)
{
    UNREFERENCED_PARAMETER(MaxPoolUsage);
    return QzLpcCreatePort(PortHandle, ObjectAttributes, MaxConnectInfoLength, MaxDataLength, FALSE);
}

NTSTATUS
NTAPI
NtCreateWaitablePort(OUT PHANDLE PortHandle, IN POBJECT_ATTRIBUTES ObjectAttributes,
                     IN ULONG MaxConnectInfoLength, IN ULONG MaxDataLength, IN ULONG MaxPoolUsage)
{
    UNREFERENCED_PARAMETER(MaxPoolUsage);
    return QzLpcCreatePort(PortHandle, ObjectAttributes, MaxConnectInfoLength, MaxDataLength, TRUE);
}

/* Register Wait as the current thread's pending request */
static VOID
QzLpcPrepareWait(PQZ_LPC_WAIT Wait, PQZ_PORT Port, ULONG MessageId)
{
    RtlZeroMemory(Wait, sizeof(*Wait));
    Wait->Thread = QzCurrentThread->PsThread;
    Wait->MessageId = MessageId;
    Wait->Port = Port;
    KeInitializeEvent(&Wait->Done, NotificationEvent, FALSE);
    ExAcquireFastMutex(&QzLpcLock);
    InsertTailList(&QzLpcWaits, &Wait->Link);
    ExReleaseFastMutex(&QzLpcLock);
}

static NTSTATUS
QzLpcWaitForReply(PQZ_LPC_WAIT Wait)
{
    KeWaitForSingleObject(&Wait->Done, WrLpcReply, KernelMode, FALSE, NULL);
    ExAcquireFastMutex(&QzLpcLock);
    if (!IsListEmpty(&Wait->Link))
        RemoveEntryList(&Wait->Link);
    ExReleaseFastMutex(&QzLpcLock);
    return Wait->Status;
}

/* The pending request a reply or an accept is for */
static PQZ_LPC_WAIT
QzLpcFindWait(ULONG MessageId, HANDLE UniqueThread, BOOLEAN Remove)
{
    PLIST_ENTRY Entry;

    for (Entry = QzLpcWaits.Flink; Entry != &QzLpcWaits; Entry = Entry->Flink)
    {
        PQZ_LPC_WAIT Wait = CONTAINING_RECORD(Entry, QZ_LPC_WAIT, Link);
        if (Wait->MessageId == MessageId &&
            (!UniqueThread || !Wait->Thread || (HANDLE)(ULONG_PTR)Wait->Thread->UniqueId == UniqueThread))
        {
            if (Remove)
            {
                RemoveEntryList(&Wait->Link);
                InitializeListHead(&Wait->Link);
            }
            return Wait;
        }
    }
    return NULL;
}

/* Map a view of Section in Process (port views): anywhere, read-write */
static NTSTATUS
QzLpcMapView(PQZ_SECTION Section, PQZ_PROCESS Process, LARGE_INTEGER Offset, SIZE_T Size, PVOID *Base)
{
    *Base = NULL;
    return QzMmMapView(Section, Process, Base, 0, &Offset, &Size, 0, PAGE_READWRITE);
}

NTSTATUS
NTAPI
NtSecureConnectPort(OUT PHANDLE PortHandle,
                    IN PUNICODE_STRING PortName,
                    IN PSECURITY_QUALITY_OF_SERVICE Qos,
                    IN OUT PPORT_VIEW ClientView OPTIONAL,
                    IN PSID ServerSid OPTIONAL,
                    IN OUT PREMOTE_PORT_VIEW ServerView OPTIONAL,
                    OUT PULONG MaxMessageLength OPTIONAL,
                    IN OUT PVOID ConnectionInformation OPTIONAL,
                    IN OUT PULONG ConnectionInformationLength OPTIONAL)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    UNICODE_STRING Name;
    WCHAR NameBuffer[128];
    PQZ_PORT Server = NULL, Client = NULL;
    PQZ_LPC_MESSAGE Message = NULL;
    PORT_VIEW View;
    REMOTE_PORT_VIEW Remote;
    QZ_LPC_WAIT Wait;
    PVOID ClientViewBase = NULL;
    ULONG InfoLength = 0, MessageId;
    HANDLE Handle;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Qos);
    UNREFERENCED_PARAMETER(ServerSid);
    RtlZeroMemory(&Wait, sizeof(Wait));

    /* The name, captured */
    if (!QzReadArg(Mode, &Name, PortName, sizeof(Name)) || Name.Length > sizeof(NameBuffer) ||
        !QzReadArg(Mode, NameBuffer, Name.Buffer, Name.Length))
        return STATUS_ACCESS_VIOLATION;
    Name.Buffer = NameBuffer;
    Name.MaximumLength = Name.Length;
    if (ConnectionInformationLength && !QzReadArg(Mode, &InfoLength, ConnectionInformationLength, sizeof(ULONG)))
        return STATUS_ACCESS_VIOLATION;
    if (!ConnectionInformation)
        InfoLength = 0;
    if (ClientView && !QzReadArg(Mode, &View, ClientView, sizeof(View)))
        return STATUS_ACCESS_VIOLATION;
    if (ServerView && !QzReadArg(Mode, &Remote, ServerView, sizeof(Remote)))
        return STATUS_ACCESS_VIOLATION;

    Status = ObReferenceObjectByName(&Name, OBJ_CASE_INSENSITIVE, NULL, PORT_CONNECT, LpcPortObjectType,
                                     KernelMode, NULL, (PVOID *)&Server);
    if (Status == STATUS_OBJECT_TYPE_MISMATCH)
        Status = ObReferenceObjectByName(&Name, OBJ_CASE_INSENSITIVE, NULL, PORT_CONNECT,
                                         LpcWaitablePortObjectType, KernelMode, NULL, (PVOID *)&Server);
    if (!NT_SUCCESS(Status))
        return Status == STATUS_OBJECT_TYPE_MISMATCH ? STATUS_OBJECT_NAME_NOT_FOUND : Status;
    if (Server->Kind != QZ_PORT_SERVER_CONNECTION || Server->Closed)
    {
        ObDereferenceObject(Server);
        return STATUS_PORT_CONNECTION_REFUSED;
    }
    if (InfoLength > Server->MaxConnectionInfoLength)
        InfoLength = Server->MaxConnectionInfoLength;

    /* The client's end */
    Status = QzLpcCreatePortObject(KernelMode, NULL, QZ_PORT_CLIENT_COMMUNICATION, FALSE, &Client);
    if (!NT_SUCCESS(Status))
        goto Out;
    Client->ConnectionPort = Server;
    ObReferenceObject(Server);
    Client->MaxMessageLength = Server->MaxMessageLength;

    /* The request the server will see: the connection data */
    Message = QzLpcAllocateMessage(InfoLength);
    if (!Message)
    {
        Status = STATUS_NO_MEMORY;
        goto Out;
    }
    if (InfoLength && !QzReadArg(Mode, &Message->Header + 1, ConnectionInformation, InfoLength))
    {
        Status = STATUS_ACCESS_VIOLATION;
        goto Out;
    }

    /* Our shared section: mapped here now, in the server when it accepts */
    if (ClientView && View.SectionHandle)
    {
        Status = ObReferenceObjectByHandle(View.SectionHandle, SECTION_MAP_READ | SECTION_MAP_WRITE,
                                           MmSectionObjectType, Mode, (PVOID *)&Wait.ClientSection, NULL);
        if (!NT_SUCCESS(Status))
            goto Out;
        Wait.ClientSectionOffset.QuadPart = View.SectionOffset;
        Wait.ClientViewSize = View.ViewSize;
        Status = QzLpcMapView(Wait.ClientSection, QzCurrentThread->ApcProcess, Wait.ClientSectionOffset,
                              View.ViewSize, &ClientViewBase);
        if (!NT_SUCCESS(Status))
            goto Out;
    }

    MessageId = (ULONG)InterlockedIncrement(&QzLpcNextMessageId);
    Message->Header.u1.s1.DataLength = (CSHORT)InfoLength;
    Message->Header.u1.s1.TotalLength = (CSHORT)(sizeof(PORT_MESSAGE) + InfoLength);
    Message->Header.u2.s2.Type = LPC_CONNECTION_REQUEST;
    Message->Header.MessageId = MessageId;
    Message->Header.ClientViewSize = Wait.ClientViewSize;
    QzLpcSetSender(&Message->Header);

    /* Ask, and sleep until the server has accepted and completed (or refused) */
    {
        PQZ_SECTION Section = Wait.ClientSection;
        LARGE_INTEGER Offset = Wait.ClientSectionOffset;
        SIZE_T Size = Wait.ClientViewSize;
        QzLpcPrepareWait(&Wait, Server, MessageId);
        Wait.ClientPort = Client;
        Wait.ClientSection = Section;
        Wait.ClientSectionOffset = Offset;
        Wait.ClientViewSize = Size;
    }
    QzLpcQueue(Server, Message);
    Message = NULL;
    Status = QzLpcWaitForReply(&Wait);
    if (NT_SUCCESS(Status) && !Wait.Accepted)
        Status = STATUS_PORT_CONNECTION_REFUSED;

    if (NT_SUCCESS(Status))
    {
        /* Connected: the server's data, the views, our handle */
        if (Wait.Reply && ConnectionInformation)
        {
            ULONG Back = min((ULONG)(USHORT)Wait.Reply->Header.u1.s1.DataLength, Server->MaxConnectionInfoLength);
            QzWriteArg(Mode, ConnectionInformation, &Wait.Reply->Header + 1, Back);
            if (ConnectionInformationLength)
                QzWriteArg(Mode, ConnectionInformationLength, &Back, sizeof(Back));
        }
        if (ClientView)
        {
            View.ViewBase = ClientViewBase;
            View.ViewRemoteBase = Wait.ClientViewInServer;
            QzWriteArg(Mode, ClientView, &View, sizeof(View));
        }
        if (ServerView && Wait.ServerSection)
        {
            PVOID Base;
            if (NT_SUCCESS(QzLpcMapView(Wait.ServerSection, QzCurrentThread->ApcProcess, Wait.ServerSectionOffset,
                                        Wait.ServerViewSize, &Base)))
            {
                Remote.Length = sizeof(Remote);
                Remote.ViewBase = Base;
                Remote.ViewSize = Wait.ServerViewSize;
                QzWriteArg(Mode, ServerView, &Remote, sizeof(Remote));
            }
        }
        if (MaxMessageLength)
            QzWriteArg(Mode, MaxMessageLength, &Client->MaxMessageLength, sizeof(ULONG));
        ObReferenceObject(Client);   /* the insert consumes one */
        Status = ObInsertObject(Client, NULL, PORT_ALL_ACCESS, 0, NULL, &Handle);
        if (NT_SUCCESS(Status) && !QzWriteArg(Mode, PortHandle, &Handle, sizeof(Handle)))
            Status = STATUS_ACCESS_VIOLATION;
    }
    if (Wait.Reply)
        QzLpcFreeMessage(Wait.Reply);
    if (Wait.ServerSection)
        ObDereferenceObject(Wait.ServerSection);

Out:
    if (!NT_SUCCESS(Status) && ClientViewBase)
        QzMmUnmapView(QzCurrentThread->ApcProcess, ClientViewBase);
    if (Wait.ClientSection)
        ObDereferenceObject(Wait.ClientSection);
    if (Message)
        QzLpcFreeMessage(Message);
    if (Client)
        ObDereferenceObject(Client);
    ObDereferenceObject(Server);
    return Status;
}

NTSTATUS
NTAPI
NtConnectPort(OUT PHANDLE PortHandle,
              IN PUNICODE_STRING PortName,
              IN PSECURITY_QUALITY_OF_SERVICE Qos,
              IN OUT PPORT_VIEW ClientView OPTIONAL,
              IN OUT PREMOTE_PORT_VIEW ServerView OPTIONAL,
              OUT PULONG MaxMessageLength OPTIONAL,
              IN OUT PVOID ConnectionInformation OPTIONAL,
              IN OUT PULONG ConnectionInformationLength OPTIONAL)
{
    return NtSecureConnectPort(PortHandle, PortName, Qos, ClientView, NULL, ServerView, MaxMessageLength,
                               ConnectionInformation, ConnectionInformationLength);
}

NTSTATUS
NTAPI
NtAcceptConnectPort(OUT PHANDLE PortHandle,
                    IN PVOID PortContext OPTIONAL,
                    IN PPORT_MESSAGE ReplyMessage,
                    IN BOOLEAN AcceptConnection,
                    IN OUT PPORT_VIEW ServerView OPTIONAL,
                    OUT PREMOTE_PORT_VIEW ClientView OPTIONAL)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PQZ_LPC_MESSAGE Reply = NULL;
    PQZ_LPC_WAIT Wait;
    PQZ_PORT ServerPort = NULL, Connection;
    PORT_VIEW View;
    HANDLE Handle = NULL;
    NTSTATUS Status;

    Status = QzLpcCaptureMessage(Mode, ReplyMessage, sizeof(PORT_MESSAGE) + QZ_LPC_MAX_CONNECTION_INFO, &Reply);
    if (!NT_SUCCESS(Status))
        return Status;
    if (ServerView && !QzReadArg(Mode, &View, ServerView, sizeof(View)))
    {
        QzLpcFreeMessage(Reply);
        return STATUS_ACCESS_VIOLATION;
    }

    /* The waiting client this answers */
    ExAcquireFastMutex(&QzLpcLock);
    Wait = QzLpcFindWait(Reply->Header.MessageId, Reply->Header.ClientId.UniqueThread, !AcceptConnection);
    ExReleaseFastMutex(&QzLpcLock);
    if (!Wait || !Wait->ClientPort)
    {
        QzLpcFreeMessage(Reply);
        return STATUS_INVALID_CID;
    }
    if (!AcceptConnection)
    {
        Wait->Status = STATUS_PORT_CONNECTION_REFUSED;
        KeSetEvent(&Wait->Done, IO_NO_INCREMENT, FALSE);
        QzLpcFreeMessage(Reply);
        return STATUS_SUCCESS;
    }

    /* The server's end */
    Connection = Wait->Port;
    Status = QzLpcCreatePortObject(Mode, NULL, QZ_PORT_SERVER_COMMUNICATION, FALSE, &ServerPort);
    if (!NT_SUCCESS(Status))
    {
        QzLpcFreeMessage(Reply);
        return Status;
    }
    ServerPort->ConnectionPort = Connection;
    ObReferenceObject(Connection);
    ServerPort->PortContext = PortContext;
    ServerPort->MaxMessageLength = Connection->MaxMessageLength;

    /* The client's section, mapped here; ours, mapped here and later in the client */
    if (Wait->ClientSection)
    {
        Status = QzLpcMapView(Wait->ClientSection, QzCurrentThread->ApcProcess, Wait->ClientSectionOffset,
                              Wait->ClientViewSize, &Wait->ClientViewInServer);
        if (NT_SUCCESS(Status) && ClientView)
        {
            REMOTE_PORT_VIEW Remote;
            Remote.Length = sizeof(Remote);
            Remote.ViewSize = Wait->ClientViewSize;
            Remote.ViewBase = Wait->ClientViewInServer;
            QzWriteArg(Mode, ClientView, &Remote, sizeof(Remote));
        }
    }
    if (NT_SUCCESS(Status) && ServerView && View.SectionHandle)
    {
        Status = ObReferenceObjectByHandle(View.SectionHandle, SECTION_MAP_READ | SECTION_MAP_WRITE,
                                           MmSectionObjectType, Mode, (PVOID *)&Wait->ServerSection, NULL);
        if (NT_SUCCESS(Status))
        {
            PVOID Base;
            Wait->ServerSectionOffset.QuadPart = View.SectionOffset;
            Wait->ServerViewSize = View.ViewSize;
            Status = QzLpcMapView(Wait->ServerSection, QzCurrentThread->ApcProcess, Wait->ServerSectionOffset,
                                  View.ViewSize, &Base);
            if (NT_SUCCESS(Status))
            {
                View.ViewBase = Base;
                QzWriteArg(Mode, ServerView, &View, sizeof(View));
            }
        }
    }
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(ServerPort);
        QzLpcFreeMessage(Reply);
        return Status;
    }

    /* Pair the two ends; the client wakes at NtCompleteConnectPort */
    ExAcquireFastMutex(&QzLpcLock);
    ServerPort->Peer = Wait->ClientPort;
    Wait->ClientPort->Peer = ServerPort;
    Wait->Reply = Reply;
    Wait->ServerPort = ServerPort;
    Wait->Accepted = TRUE;
    ExReleaseFastMutex(&QzLpcLock);

    Status = ObInsertObject(ServerPort, NULL, PORT_ALL_ACCESS, 0, NULL, &Handle);
    if (NT_SUCCESS(Status) && !QzWriteArg(Mode, PortHandle, &Handle, sizeof(Handle)))
        Status = STATUS_ACCESS_VIOLATION;
    return Status;
}

/* Wake the client whose connection was accepted on this port */
NTSTATUS
NTAPI
NtCompleteConnectPort(IN HANDLE PortHandle)
{
    PQZ_PORT ServerPort;
    PQZ_LPC_WAIT Wait = NULL;
    PLIST_ENTRY Entry;
    NTSTATUS Status = QzLpcReferencePort(PortHandle, 0, &ServerPort);

    if (!NT_SUCCESS(Status))
        return Status;
    ExAcquireFastMutex(&QzLpcLock);
    for (Entry = QzLpcWaits.Flink; Entry != &QzLpcWaits; Entry = Entry->Flink)
    {
        PQZ_LPC_WAIT Candidate = CONTAINING_RECORD(Entry, QZ_LPC_WAIT, Link);
        if (Candidate->ServerPort == ServerPort && Candidate->Accepted)
        {
            Wait = Candidate;
            RemoveEntryList(&Wait->Link);
            InitializeListHead(&Wait->Link);
            break;
        }
    }
    ExReleaseFastMutex(&QzLpcLock);
    if (Wait)
    {
        Wait->Status = STATUS_SUCCESS;
        KeSetEvent(&Wait->Done, IO_NO_INCREMENT, FALSE);
    }
    else
    {
        Status = STATUS_INVALID_PARAMETER;
    }
    ObDereferenceObject(ServerPort);
    return Status;
}

/* ---------------------------------------------------------------------- */
/* Sending                                                                 */
/* ---------------------------------------------------------------------- */

/* A communication port's message, tagged so the receiver knows the connection */
static NTSTATUS
QzLpcSendOn(PQZ_PORT Port, PQZ_LPC_MESSAGE Message, CSHORT Type)
{
    PQZ_PORT Target, Tag;

    ExAcquireFastMutex(&QzLpcLock);
    if (Port->Closed || (Port->Kind != QZ_PORT_SERVER_CONNECTION && !Port->Peer))
    {
        ExReleaseFastMutex(&QzLpcLock);
        return STATUS_PORT_DISCONNECTED;
    }
    if (Port->Kind == QZ_PORT_SERVER_CONNECTION)
    {
        /* Straight to the server's own queue, outside any connection (win32k
           to csrss's API port): it receives the message with no port context */
        Target = Port;
        Tag = NULL;
    }
    else
    {
        Target = QzLpcTargetQueue(Port);
        /* The receiver learns the connection from the server's end of it */
        Tag = (Port->Kind == QZ_PORT_CLIENT_COMMUNICATION) ? Port->Peer : Port;
        ObReferenceObject(Tag);
    }
    ObReferenceObject(Target);
    ExReleaseFastMutex(&QzLpcLock);

    Message->SenderPort = Tag;
    if (Type)
        Message->Header.u2.s2.Type = Type;
    if (!Message->Header.MessageId)
        Message->Header.MessageId = (ULONG)InterlockedIncrement(&QzLpcNextMessageId);
    QzLpcSetSender(&Message->Header);
    QzLpcQueue(Target, Message);
    ObDereferenceObject(Target);
    return STATUS_SUCCESS;
}

static NTSTATUS
QzLpcRequest(PQZ_PORT Port, KPROCESSOR_MODE Mode, PPORT_MESSAGE RequestMessage)
{
    PQZ_LPC_MESSAGE Message;
    NTSTATUS Status = QzLpcCaptureMessage(Mode, RequestMessage, Port->MaxMessageLength, &Message);
    if (!NT_SUCCESS(Status))
        return Status;
    Message->Header.MessageId = 0;
    Status = QzLpcSendOn(Port, Message, LPC_DATAGRAM);
    if (!NT_SUCCESS(Status))
        QzLpcFreeMessage(Message);
    return Status;
}

static NTSTATUS
QzLpcRequestWaitReply(PQZ_PORT Port, KPROCESSOR_MODE Mode, PPORT_MESSAGE RequestMessage,
                      PPORT_MESSAGE ReplyMessage)
{
    PQZ_LPC_MESSAGE Message;
    QZ_LPC_WAIT Wait;
    CSHORT Type;
    NTSTATUS Status = QzLpcCaptureMessage(Mode, RequestMessage, Port->MaxMessageLength, &Message);

    if (!NT_SUCCESS(Status))
        return Status;
    /* Kernel callers may send the special types (exceptions, errors, ...) */
    Type = Message->Header.u2.s2.Type;
    if (Mode != KernelMode || !Type)
        Type = LPC_REQUEST;
    Message->Header.u2.s2.Type = Type;

    /* Registered, with the message's ID, before it can be answered */
    Message->Header.MessageId = (ULONG)InterlockedIncrement(&QzLpcNextMessageId);
    QzLpcPrepareWait(&Wait, Port, Message->Header.MessageId);
    Status = QzLpcSendOn(Port, Message, 0);
    if (!NT_SUCCESS(Status))
    {
        ExAcquireFastMutex(&QzLpcLock);
        RemoveEntryList(&Wait.Link);
        ExReleaseFastMutex(&QzLpcLock);
        QzLpcFreeMessage(Message);
        return Status;
    }
    Status = QzLpcWaitForReply(&Wait);
    if (NT_SUCCESS(Status) && Wait.Reply)
    {
        if (!QzLpcWriteMessage(Mode, ReplyMessage, Wait.Reply))
            Status = STATUS_ACCESS_VIOLATION;
    }
    if (Wait.Reply)
        QzLpcFreeMessage(Wait.Reply);
    return Status;
}

NTSTATUS
NTAPI
NtRequestPort(IN HANDLE PortHandle, IN PPORT_MESSAGE LpcMessage)
{
    PQZ_PORT Port;
    NTSTATUS Status = QzLpcReferencePort(PortHandle, 0, &Port);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = QzLpcRequest(Port, ExGetPreviousMode(), LpcMessage);
    ObDereferenceObject(Port);
    return Status;
}

NTSTATUS
NTAPI
NtRequestWaitReplyPort(IN HANDLE PortHandle, IN PPORT_MESSAGE LpcRequest, IN OUT PPORT_MESSAGE LpcReply)
{
    PQZ_PORT Port;
    NTSTATUS Status = QzLpcReferencePort(PortHandle, 0, &Port);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = QzLpcRequestWaitReply(Port, ExGetPreviousMode(), LpcRequest, LpcReply);
    ObDereferenceObject(Port);
    return Status;
}

/* Kernel-side senders (the hard error path, the debugger, ...) */
NTSTATUS
NTAPI
LpcRequestPort(IN PVOID PortObject, IN PPORT_MESSAGE LpcMessage)
{
    return QzLpcRequest(PortObject, KernelMode, LpcMessage);
}

NTSTATUS
NTAPI
LpcRequestWaitReplyPort(IN PVOID PortObject, IN PPORT_MESSAGE LpcRequest, OUT PPORT_MESSAGE LpcReply)
{
    return QzLpcRequestWaitReply(PortObject, KernelMode, LpcRequest, LpcReply);
}

/* Answer a request: straight to the thread waiting for it */
static NTSTATUS
QzLpcReply(KPROCESSOR_MODE Mode, PPORT_MESSAGE ReplyMessage)
{
    PQZ_LPC_MESSAGE Reply;
    PQZ_LPC_WAIT Wait;
    NTSTATUS Status = QzLpcCaptureMessage(Mode, ReplyMessage, QZ_LPC_MAX_MESSAGE, &Reply);

    if (!NT_SUCCESS(Status))
        return Status;
    Reply->Header.u2.s2.Type = LPC_REPLY;
    ExAcquireFastMutex(&QzLpcLock);
    Wait = QzLpcFindWait(Reply->Header.MessageId, Reply->Header.ClientId.UniqueThread, TRUE);
    if (Wait)
    {
        Wait->Reply = Reply;
        Wait->Status = STATUS_SUCCESS;
        KeSetEvent(&Wait->Done, IO_NO_INCREMENT, FALSE);
    }
    ExReleaseFastMutex(&QzLpcLock);
    if (!Wait)
    {
        QzLpcFreeMessage(Reply);
        return STATUS_REPLY_MESSAGE_MISMATCH;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtReplyPort(IN HANDLE PortHandle, IN PPORT_MESSAGE LpcReply)
{
    PQZ_PORT Port;
    NTSTATUS Status = QzLpcReferencePort(PortHandle, 0, &Port);
    if (!NT_SUCCESS(Status))
        return Status;
    ObDereferenceObject(Port);
    return QzLpcReply(ExGetPreviousMode(), LpcReply);
}

/* ---------------------------------------------------------------------- */
/* Receiving                                                               */
/* ---------------------------------------------------------------------- */

/* The queue a port's owner receives from */
static PQZ_PORT
QzLpcReceiveQueue(PQZ_PORT Port)
{
    if (Port->Kind == QZ_PORT_SERVER_COMMUNICATION)
        return Port->ConnectionPort;
    return Port;
}

static NTSTATUS
QzLpcReceive(PQZ_PORT Port, PLARGE_INTEGER Timeout, PQZ_LPC_MESSAGE *Result)
{
    PQZ_PORT Queue = QzLpcReceiveQueue(Port);
    NTSTATUS Status;

    *Result = NULL;
    Status = KeWaitForSingleObject(&Queue->QueueSemaphore, WrLpcReceive, KernelMode, FALSE, Timeout);
    if (Status != STATUS_SUCCESS)
        return Status == STATUS_TIMEOUT ? STATUS_TIMEOUT : Status;
    ExAcquireFastMutex(&QzLpcLock);
    if (!IsListEmpty(&Queue->Queue))
        *Result = CONTAINING_RECORD(RemoveHeadList(&Queue->Queue), QZ_LPC_MESSAGE, Link);
    if (Queue->Waitable && IsListEmpty(&Queue->Queue))
        KeClearEvent(&Queue->WaitEvent);
    ExReleaseFastMutex(&QzLpcLock);
    return *Result ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

NTSTATUS
NTAPI
NtReplyWaitReceivePortEx(IN HANDLE PortHandle,
                         OUT PVOID *PortContext OPTIONAL,
                         IN PPORT_MESSAGE ReplyMessage OPTIONAL,
                         OUT PPORT_MESSAGE ReceiveMessage,
                         IN PLARGE_INTEGER Timeout OPTIONAL)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    PQZ_LPC_MESSAGE Message;
    LARGE_INTEGER LocalTimeout;
    PQZ_PORT Port;
    PVOID Context;
    NTSTATUS Status;

    if (Timeout && !QzReadArg(Mode, &LocalTimeout, Timeout, sizeof(LocalTimeout)))
        return STATUS_ACCESS_VIOLATION;
    Status = QzLpcReferencePort(PortHandle, 0, &Port);
    if (!NT_SUCCESS(Status))
        return Status;
    if (ReplyMessage)
    {
        Status = QzLpcReply(Mode, ReplyMessage);
        if (!NT_SUCCESS(Status) && Status != STATUS_REPLY_MESSAGE_MISMATCH)
        {
            ObDereferenceObject(Port);
            return Status;
        }
    }
    Status = QzLpcReceive(Port, Timeout ? &LocalTimeout : NULL, &Message);
    if (Status == STATUS_SUCCESS)
    {
        Context = Message->SenderPort ? Message->SenderPort->PortContext : NULL;
        if (!QzLpcWriteMessage(Mode, ReceiveMessage, Message) ||
            (PortContext && !QzWriteArg(Mode, PortContext, &Context, sizeof(Context))))
            Status = STATUS_ACCESS_VIOLATION;
        QzLpcFreeMessage(Message);
    }
    ObDereferenceObject(Port);
    return Status;
}

NTSTATUS
NTAPI
NtReplyWaitReceivePort(IN HANDLE PortHandle,
                       OUT PVOID *PortContext OPTIONAL,
                       IN PPORT_MESSAGE ReplyMessage OPTIONAL,
                       OUT PPORT_MESSAGE ReceiveMessage)
{
    return NtReplyWaitReceivePortEx(PortHandle, PortContext, ReplyMessage, ReceiveMessage, NULL);
}

/* Wait for a connection request (anything else stays queued) */
NTSTATUS
NTAPI
NtListenPort(IN HANDLE PortHandle, OUT PPORT_MESSAGE ConnectMessage)
{
    KPROCESSOR_MODE Mode = ExGetPreviousMode();
    LIST_ENTRY Others;
    PQZ_LPC_MESSAGE Message;
    PQZ_PORT Port, Queue;
    NTSTATUS Status;

    Status = QzLpcReferencePort(PortHandle, 0, &Port);
    if (!NT_SUCCESS(Status))
        return Status;
    Queue = QzLpcReceiveQueue(Port);
    InitializeListHead(&Others);
    for (;;)
    {
        Status = QzLpcReceive(Port, NULL, &Message);
        if (Status != STATUS_SUCCESS)
            break;
        if (Message->Header.u2.s2.Type == LPC_CONNECTION_REQUEST)
        {
            if (!QzLpcWriteMessage(Mode, ConnectMessage, Message))
                Status = STATUS_ACCESS_VIOLATION;
            QzLpcFreeMessage(Message);
            break;
        }
        InsertTailList(&Others, &Message->Link);
    }
    while (!IsListEmpty(&Others))
        QzLpcQueue(Queue, CONTAINING_RECORD(RemoveHeadList(&Others), QZ_LPC_MESSAGE, Link));
    ObDereferenceObject(Port);
    return Status;
}

/* Reply, then wait for the answer to the message being replied to (callbacks) */
NTSTATUS
NTAPI
NtReplyWaitReplyPort(IN HANDLE PortHandle, IN OUT PPORT_MESSAGE ReplyMessage)
{
    return NtReplyPort(PortHandle, ReplyMessage);
}

/* ---------------------------------------------------------------------- */
/* The rest                                                                */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
NtImpersonateClientOfPort(IN HANDLE PortHandle, IN PPORT_MESSAGE ClientMessage)
{
    UNREFERENCED_PARAMETER(PortHandle);
    UNREFERENCED_PARAMETER(ClientMessage);
    return STATUS_SUCCESS;   /* everyone is the system so far */
}

NTSTATUS
NTAPI
NtQueryInformationPort(IN HANDLE PortHandle, IN PORT_INFORMATION_CLASS PortInformationClass,
                       OUT PVOID PortInformation, IN ULONG PortInformationLength, OUT PULONG ReturnLength)
{
    PQZ_PORT Port;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(PortInformationClass);
    UNREFERENCED_PARAMETER(PortInformation);
    UNREFERENCED_PARAMETER(PortInformationLength);
    Status = QzLpcReferencePort(PortHandle, 0, &Port);
    if (!NT_SUCCESS(Status))
        return Status;
    ObDereferenceObject(Port);
    if (ReturnLength)
    {
        ULONG Zero = 0;
        QzWriteArg(ExGetPreviousMode(), ReturnLength, &Zero, sizeof(Zero));
    }
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtRegisterThreadTerminatePort(IN HANDLE PortHandle)
{
    PQZ_PORT Port;
    NTSTATUS Status = QzLpcReferencePort(PortHandle, 0, &Port);
    if (NT_SUCCESS(Status))
        ObDereferenceObject(Port);
    return Status;   /* LPC_CLIENT_DIED on thread exit: later */
}

/* ---------------------------------------------------------------------- */
/* Kernel-mode callers                                                     */
/* ---------------------------------------------------------------------- */

NTSTATUS
NTAPI
ZwSecureConnectPort(OUT PHANDLE PortHandle, IN PUNICODE_STRING PortName, IN PSECURITY_QUALITY_OF_SERVICE Qos,
                    IN OUT PPORT_VIEW ClientView OPTIONAL, IN PSID ServerSid OPTIONAL,
                    IN OUT PREMOTE_PORT_VIEW ServerView OPTIONAL, OUT PULONG MaxMessageLength OPTIONAL,
                    IN OUT PVOID ConnectionInformation OPTIONAL, IN OUT PULONG ConnectionInformationLength OPTIONAL)
{
    return QZ_AS_KERNEL(NtSecureConnectPort(PortHandle, PortName, Qos, ClientView, ServerSid, ServerView,
                                            MaxMessageLength, ConnectionInformation, ConnectionInformationLength));
}

NTSTATUS
NTAPI
ZwConnectPort(OUT PHANDLE PortHandle, IN PUNICODE_STRING PortName, IN PSECURITY_QUALITY_OF_SERVICE Qos,
              IN OUT PPORT_VIEW ClientView OPTIONAL, IN OUT PREMOTE_PORT_VIEW ServerView OPTIONAL,
              OUT PULONG MaxMessageLength OPTIONAL, IN OUT PVOID ConnectionInformation OPTIONAL,
              IN OUT PULONG ConnectionInformationLength OPTIONAL)
{
    return QZ_AS_KERNEL(NtSecureConnectPort(PortHandle, PortName, Qos, ClientView, NULL, ServerView,
                                            MaxMessageLength, ConnectionInformation, ConnectionInformationLength));
}

NTSTATUS
NTAPI
ZwRequestWaitReplyPort(IN HANDLE PortHandle, IN PPORT_MESSAGE LpcRequest, IN OUT PPORT_MESSAGE LpcReply)
{
    return QZ_AS_KERNEL(NtRequestWaitReplyPort(PortHandle, LpcRequest, LpcReply));
}

/* Kernel-mode servers (the reference monitor) */
NTSTATUS
NTAPI
ZwCreatePort(OUT PHANDLE PortHandle, IN POBJECT_ATTRIBUTES ObjectAttributes, IN ULONG MaxConnectInfoLength,
             IN ULONG MaxDataLength, IN ULONG MaxPoolUsage)
{
    return QZ_AS_KERNEL(NtCreatePort(PortHandle, ObjectAttributes, MaxConnectInfoLength, MaxDataLength,
                                     MaxPoolUsage));
}

NTSTATUS
NTAPI
ZwListenPort(IN HANDLE PortHandle, OUT PPORT_MESSAGE ConnectMessage)
{
    return QZ_AS_KERNEL(NtListenPort(PortHandle, ConnectMessage));
}

NTSTATUS
NTAPI
ZwAcceptConnectPort(OUT PHANDLE PortHandle, IN PVOID PortContext OPTIONAL, IN PPORT_MESSAGE ReplyMessage,
                    IN BOOLEAN AcceptConnection, IN OUT PPORT_VIEW ServerView OPTIONAL,
                    OUT PREMOTE_PORT_VIEW ClientView OPTIONAL)
{
    return QZ_AS_KERNEL(NtAcceptConnectPort(PortHandle, PortContext, ReplyMessage, AcceptConnection, ServerView,
                                            ClientView));
}

NTSTATUS
NTAPI
ZwCompleteConnectPort(IN HANDLE PortHandle)
{
    return QZ_AS_KERNEL(NtCompleteConnectPort(PortHandle));
}

NTSTATUS
NTAPI
ZwReplyWaitReceivePort(IN HANDLE PortHandle, OUT PVOID *PortContext OPTIONAL, IN PPORT_MESSAGE ReplyMessage OPTIONAL,
                       OUT PPORT_MESSAGE ReceiveMessage)
{
    return QZ_AS_KERNEL(NtReplyWaitReceivePort(PortHandle, PortContext, ReplyMessage, ReceiveMessage));
}

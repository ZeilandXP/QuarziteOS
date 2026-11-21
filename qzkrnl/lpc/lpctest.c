/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     LPC, keyed event and token self-test (shell command "lpctest")
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * A server thread listens on a named port, accepts a connection with a
 * port context, and answers requests until the client goes away; the
 * client connects with connection data, makes requests that need a
 * reply, sends a datagram and closes. Then keyed events (a rendezvous in
 * both orders, timeouts, the global event) and the system token.
 */

#include "qzio.h"
#include "qzps.h"
#include <ndk/lpcfuncs.h>

/* ex/keyed.c (the NDK declares these only for newer targets) */
NTSTATUS NTAPI NtCreateKeyedEvent(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG);
NTSTATUS NTAPI NtOpenKeyedEvent(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
NTSTATUS NTAPI NtReleaseKeyedEvent(HANDLE, PVOID, BOOLEAN, PLARGE_INTEGER);
NTSTATUS NTAPI NtWaitForKeyedEvent(HANDLE, PVOID, BOOLEAN, PLARGE_INTEGER);

static ULONG QzPassed, QzFailed;

static VOID
QzCheck(PCSTR Name, BOOLEAN Condition)
{
    if (Condition)
        QzPassed++;
    else
        QzFailed++;
    QzPrint("  [%s] %s\n", Condition ? "PASS" : "FAIL", Name);
}

typedef struct _QZ_TEST_MESSAGE
{
    PORT_MESSAGE Header;
    ULONG Data[4];
} QZ_TEST_MESSAGE, *PQZ_TEST_MESSAGE;

#define QZ_TEST_CONTEXT ((PVOID)0x5A5A1234)

typedef struct _QZ_SERVER_STATE
{
    HANDLE Port;
    ULONG ConnectData;          /* what the connection request carried */
    ULONG Requests, Datagrams;
    BOOLEAN ContextSeen;        /* every message came with our context */
    BOOLEAN SawClose;
    NTSTATUS Status;
} QZ_SERVER_STATE, *PQZ_SERVER_STATE;

static VOID
QzInitMessage(PQZ_TEST_MESSAGE Message, ULONG Value)
{
    RtlZeroMemory(Message, sizeof(*Message));
    Message->Header.u1.s1.DataLength = sizeof(Message->Data);
    Message->Header.u1.s1.TotalLength = sizeof(*Message);
    Message->Data[0] = Value;
}

static VOID
NTAPI
QzLpcServerThread(PVOID Context)
{
    PQZ_SERVER_STATE State = Context;
    struct { PORT_MESSAGE Header; UCHAR Data[0x100]; } Connect;
    QZ_TEST_MESSAGE Message, Reply;
    PQZ_TEST_MESSAGE ReplyPtr = NULL;
    HANDLE Communication = NULL;
    PVOID PortContext;
    NTSTATUS Status;

    State->ContextSeen = TRUE;
    Status = NtListenPort(State->Port, &Connect.Header);
    if (NT_SUCCESS(Status))
    {
        State->ConnectData = *(PULONG)Connect.Data;
        Status = NtAcceptConnectPort(&Communication, QZ_TEST_CONTEXT, &Connect.Header, TRUE, NULL, NULL);
        if (NT_SUCCESS(Status))
            Status = NtCompleteConnectPort(Communication);
    }
    while (NT_SUCCESS(Status))
    {
        Status = NtReplyWaitReceivePort(State->Port, &PortContext, &ReplyPtr->Header, &Message.Header);
        ReplyPtr = NULL;
        if (!NT_SUCCESS(Status))
            break;
        if (PortContext != QZ_TEST_CONTEXT)
            State->ContextSeen = FALSE;
        if (Message.Header.u2.s2.Type == LPC_REQUEST)
        {
            State->Requests++;
            Reply = Message;            /* same MessageId and ClientId */
            Reply.Data[0] = Message.Data[0] + 1;
            ReplyPtr = &Reply;
        }
        else if (Message.Header.u2.s2.Type == LPC_DATAGRAM)
            State->Datagrams++;
        else if (Message.Header.u2.s2.Type == LPC_PORT_CLOSED)
        {
            State->SawClose = TRUE;
            break;
        }
    }
    State->Status = Status;
    if (Communication)
        ZwClose(Communication);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS
QzStartThread(PKSTART_ROUTINE Routine, PVOID Context, PHANDLE Thread)
{
    return PsCreateSystemThread(Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, Routine, Context);
}

static BOOLEAN
QzJoin(HANDLE Thread, ULONG Milliseconds)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status;
    Timeout.QuadPart = -(LONGLONG)Milliseconds * 10000;
    Status = ZwWaitForSingleObject(Thread, FALSE, &Timeout);
    ZwClose(Thread);
    return Status == STATUS_SUCCESS;
}

static VOID
QzLpcPortTests(VOID)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\QzLpcTest");
    UNICODE_STRING Missing = RTL_CONSTANT_STRING(L"\\QzNoSuchPort");
    SECURITY_QUALITY_OF_SERVICE Qos = { sizeof(Qos), SecurityImpersonation, SECURITY_DYNAMIC_TRACKING, TRUE };
    OBJECT_ATTRIBUTES Attributes;
    QZ_SERVER_STATE State;
    QZ_TEST_MESSAGE Message, Reply;
    HANDLE Client = NULL, Server;
    ULONG ConnectData = 0xC0FFEE, ConnectLength = sizeof(ConnectData), MaxMessage = 0;
    NTSTATUS Status;
    BOOLEAN Ok;
    ULONG i;

    RtlZeroMemory(&State, sizeof(State));
    InitializeObjectAttributes(&Attributes, &Name, 0, NULL, NULL);
    Status = NtCreatePort(&State.Port, &Attributes, 0x100, sizeof(QZ_TEST_MESSAGE), 0);
    QzCheck("create a named connection port", NT_SUCCESS(Status));
    if (!NT_SUCCESS(Status))
        return;
    QzCheck("connecting to a port that is not there fails",
            NtConnectPort(&Client, &Missing, &Qos, NULL, NULL, NULL, NULL, NULL) == STATUS_OBJECT_NAME_NOT_FOUND);

    Status = QzStartThread(QzLpcServerThread, &State, &Server);
    QzCheck("start the server thread", NT_SUCCESS(Status));
    if (!NT_SUCCESS(Status))
    {
        ZwClose(State.Port);
        return;
    }

    Status = NtConnectPort(&Client, &Name, &Qos, NULL, NULL, &MaxMessage, &ConnectData, &ConnectLength);
    QzCheck("connect (listen, accept, complete)", NT_SUCCESS(Status) && Client);
    QzCheck("the server got the connection data", State.ConnectData == 0xC0FFEE);
    QzCheck("the client learns the maximum message length", MaxMessage >= sizeof(QZ_TEST_MESSAGE));

    if (NT_SUCCESS(Status))
    {
        Ok = TRUE;
        for (i = 0; i < 20; i++)
        {
            QzInitMessage(&Message, 100 + i);
            RtlZeroMemory(&Reply, sizeof(Reply));
            if (!NT_SUCCESS(NtRequestWaitReplyPort(Client, &Message.Header, &Reply.Header)) ||
                Reply.Data[0] != 101 + i || Reply.Header.u2.s2.Type != LPC_REPLY)
                Ok = FALSE;
        }
        QzCheck("20 requests get the right replies", Ok);

        QzInitMessage(&Message, 7);
        QzCheck("send a datagram", NT_SUCCESS(NtRequestPort(Client, &Message.Header)));

        /* One more request so the datagram was surely seen first */
        QzInitMessage(&Message, 1);
        NtRequestWaitReplyPort(Client, &Message.Header, &Reply.Header);
        QzCheck("the server saw it", State.Datagrams == 1);

        QzInitMessage(&Message, 1);
        Message.Header.u1.s1.DataLength = 0x2000;
        Message.Header.u1.s1.TotalLength = 0x2000 + sizeof(PORT_MESSAGE);
        QzCheck("an oversized message is refused",
                !NT_SUCCESS(NtRequestWaitReplyPort(Client, &Message.Header, &Reply.Header)));
        ZwClose(Client);
    }

    Ok = QzJoin(Server, 5000);
    QzCheck("closing the client ends the server (LPC_PORT_CLOSED)", Ok && State.SawClose);
    QzCheck("every message came with the accepted port context", State.ContextSeen && State.Requests == 21);
    ZwClose(State.Port);
}

/* ---------------------------------------------------------------------- */

typedef struct _QZ_KEYED_STATE
{
    HANDLE Event;
    PVOID Key;
    BOOLEAN Release;            /* release instead of wait */
    NTSTATUS Status;
} QZ_KEYED_STATE, *PQZ_KEYED_STATE;

static VOID
NTAPI
QzKeyedThread(PVOID Context)
{
    PQZ_KEYED_STATE State = Context;
    LARGE_INTEGER Timeout;
    Timeout.QuadPart = -50000000LL;     /* 5 s: never in a passing run */
    State->Status = State->Release ? NtReleaseKeyedEvent(State->Event, State->Key, FALSE, &Timeout)
                                   : NtWaitForKeyedEvent(State->Event, State->Key, FALSE, &Timeout);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID
QzKeyedEventTests(VOID)
{
    UNICODE_STRING Global = RTL_CONSTANT_STRING(L"\\KernelObjects\\CritSecOutOfMemoryEvent");
    OBJECT_ATTRIBUTES Attributes;
    QZ_KEYED_STATE State;
    LARGE_INTEGER Short, Pause;
    HANDLE Event, Opened, Thread;
    NTSTATUS Status;

    Short.QuadPart = -500000LL;         /* 50 ms */
    Pause.QuadPart = -1000000LL;        /* 100 ms */
    Status = NtCreateKeyedEvent(&Event, EVENT_ALL_ACCESS, NULL, 0);
    QzCheck("create a keyed event", NT_SUCCESS(Status));
    if (!NT_SUCCESS(Status))
        return;

    /* A waiter first, then the releaser */
    RtlZeroMemory(&State, sizeof(State));
    State.Event = Event;
    State.Key = (PVOID)0x1000;
    State.Status = STATUS_PENDING;
    if (NT_SUCCESS(QzStartThread(QzKeyedThread, &State, &Thread)))
    {
        KeDelayExecutionThread(KernelMode, FALSE, &Pause);
        QzCheck("a release meets a waiting thread", NT_SUCCESS(NtReleaseKeyedEvent(Event, State.Key, FALSE, &Short)));
        QzCheck("and the waiter wakes", QzJoin(Thread, 2000) && State.Status == STATUS_SUCCESS);
    }

    /* A releaser first, then the waiter */
    State.Release = TRUE;
    State.Key = (PVOID)0x2000;
    State.Status = STATUS_PENDING;
    if (NT_SUCCESS(QzStartThread(QzKeyedThread, &State, &Thread)))
    {
        KeDelayExecutionThread(KernelMode, FALSE, &Pause);
        QzCheck("a wait meets a waiting releaser", NT_SUCCESS(NtWaitForKeyedEvent(Event, State.Key, FALSE, &Short)));
        QzCheck("and the releaser goes on", QzJoin(Thread, 2000) && State.Status == STATUS_SUCCESS);
    }

    QzCheck("a wait nobody releases times out", NtWaitForKeyedEvent(Event, (PVOID)0x3000, FALSE, &Short) ==
                                                STATUS_TIMEOUT);
    QzCheck("a release on another key does not match",
            NtReleaseKeyedEvent(Event, (PVOID)0x4000, FALSE, &Short) == STATUS_TIMEOUT);
    QzCheck("odd keys are refused", !NT_SUCCESS(NtReleaseKeyedEvent(Event, (PVOID)0x5001, FALSE, &Short)));
    QzCheck("the global event (NULL handle) works", NtWaitForKeyedEvent(NULL, (PVOID)0x6000, FALSE, &Short) ==
                                                    STATUS_TIMEOUT);
    InitializeObjectAttributes(&Attributes, &Global, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenKeyedEvent(&Opened, EVENT_ALL_ACCESS, &Attributes);
    QzCheck("open \\KernelObjects\\CritSecOutOfMemoryEvent", NT_SUCCESS(Status));
    if (NT_SUCCESS(Status))
        ZwClose(Opened);
    ZwClose(Event);
}

/* ---------------------------------------------------------------------- */

static VOID
QzTokenTests(VOID)
{
    UCHAR Buffer[512];
    HANDLE Token, Copy;
    ULONG Length = 0;
    NTSTATUS Status;

    Status = NtOpenProcessToken(NtCurrentProcess(), TOKEN_ALL_ACCESS, &Token);
    QzCheck("open the process token", NT_SUCCESS(Status));
    if (!NT_SUCCESS(Status))
        return;

    Status = NtQueryInformationToken(Token, TokenUser, Buffer, 4, &Length);
    QzCheck("a short buffer reports the length", Status == STATUS_BUFFER_TOO_SMALL && Length > sizeof(TOKEN_USER));
    Status = NtQueryInformationToken(Token, TokenUser, Buffer, sizeof(Buffer), &Length);
    QzCheck("the user is LocalSystem", NT_SUCCESS(Status) &&
                                       RtlEqualSid(((PTOKEN_USER)Buffer)->User.Sid, SeExports->SeLocalSystemSid));
    Status = NtQueryInformationToken(Token, TokenGroups, Buffer, sizeof(Buffer), &Length);
    QzCheck("in the Administrators group", NT_SUCCESS(Status) &&
                                           RtlEqualSid(((PTOKEN_GROUPS)Buffer)->Groups[0].Sid,
                                                       SeExports->SeAliasAdminsSid));
    Status = NtQueryInformationToken(Token, TokenPrivileges, Buffer, sizeof(Buffer), &Length);
    QzCheck("with privileges", NT_SUCCESS(Status) && ((PTOKEN_PRIVILEGES)Buffer)->PrivilegeCount > 20);
    {
        TOKEN_PRIVILEGES New, Old;
        New.PrivilegeCount = 1;
        New.Privileges[0].Luid.LowPart = SE_SHUTDOWN_PRIVILEGE;
        New.Privileges[0].Luid.HighPart = 0;
        New.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        Status = NtAdjustPrivilegesToken(Token, FALSE, &New, sizeof(Old), &Old, &Length);
        QzCheck("adjust a privilege", NT_SUCCESS(Status) && Old.PrivilegeCount == 1);
    }
    Status = NtDuplicateToken(Token, TOKEN_QUERY, NULL, FALSE, TokenImpersonation, &Copy);
    QzCheck("duplicate the token", NT_SUCCESS(Status));
    if (NT_SUCCESS(Status))
        ZwClose(Copy);
    ZwClose(Token);
    QzCheck("SIDs as text", NT_SUCCESS(RtlConvertSidToUnicodeString((PUNICODE_STRING)Buffer,
                                                                     SeExports->SeLocalSystemSid, TRUE)) &&
                            RtlEqualUnicodeString((PUNICODE_STRING)Buffer, &(UNICODE_STRING)RTL_CONSTANT_STRING(L"S-1-5-18"), FALSE));
    RtlFreeUnicodeString((PUNICODE_STRING)Buffer);
}

VOID
QzRunLpcTests(VOID)
{
    QzPassed = QzFailed = 0;
    QzPrint("LPC:\n");
    QzLpcPortTests();
    QzPrint("Keyed events:\n");
    QzKeyedEventTests();
    QzPrint("Tokens:\n");
    QzTokenTests();
    QzPrint("  lpctest: %lu passed, %lu failed\n", QzPassed, QzFailed);
}

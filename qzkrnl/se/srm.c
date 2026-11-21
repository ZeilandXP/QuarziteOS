/*
 * PROJECT:     QuarziteOS Kernel (qzkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The security reference monitor's command server (lsass talks to it)
 * COPYRIGHT:   Copyright 2026 QuarziteOS contributors
 *
 * lsass starts by creating \SeLsaCommandPort, signalling \SeLsaInitEvent
 * and connecting to \SeRmCommandPort. The kernel side: the event and the
 * port exist from boot; a system thread waits for the event, accepts
 * lsass's connection, connects back to lsass's port (with a one-page
 * shared view for messages), then serves lsass's commands. There is one
 * token and no auditing, so creating and deleting logon sessions and
 * setting audit options all succeed without changing anything.
 */

#include "qz.h"
#include <reactos/srmp.h>

NTSTATUS NTAPI ZwCreatePort(PHANDLE PortHandle, POBJECT_ATTRIBUTES ObjectAttributes, ULONG MaxConnectionInfoLength,
                            ULONG MaxMessageLength, ULONG MaxPoolUsage);
NTSTATUS NTAPI ZwListenPort(HANDLE PortHandle, PPORT_MESSAGE ConnectionRequest);
NTSTATUS NTAPI ZwAcceptConnectPort(PHANDLE PortHandle, PVOID PortContext, PPORT_MESSAGE ConnectionRequest,
                                   BOOLEAN AcceptConnection, PPORT_VIEW ServerView, PREMOTE_PORT_VIEW ClientView);
NTSTATUS NTAPI ZwCompleteConnectPort(HANDLE PortHandle);
NTSTATUS NTAPI ZwConnectPort(PHANDLE PortHandle, PUNICODE_STRING PortName, PSECURITY_QUALITY_OF_SERVICE Qos,
                             PPORT_VIEW ClientView, PREMOTE_PORT_VIEW ServerView, PULONG MaxMessageLength,
                             PVOID ConnectionInformation, PULONG ConnectionInformationLength);
NTSTATUS NTAPI ZwReplyWaitReceivePort(HANDLE PortHandle, PVOID *PortContext, PPORT_MESSAGE ReplyMessage,
                                      PPORT_MESSAGE ReceiveMessage);

static HANDLE QzRmCommandPort, QzRmLsaInitEvent;
static HANDLE QzRmLsaPort;      /* our connection to \SeLsaCommandPort */

/* Wait for lsass, take its connection, connect back */
static NTSTATUS
QzRmConnect(PHANDLE MessagePort)
{
    SEP_RM_API_MESSAGE Message;
    REMOTE_PORT_VIEW RemoteView;
    PORT_VIEW View;
    SECURITY_QUALITY_OF_SERVICE Qos;
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\SeLsaCommandPort");
    LARGE_INTEGER SectionSize;
    HANDLE Section = NULL;
    NTSTATUS Status;

    Status = ZwWaitForSingleObject(QzRmLsaInitEvent, FALSE, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(&Message, sizeof(Message));
    Status = ZwListenPort(QzRmCommandPort, &Message.Header);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlZeroMemory(&RemoteView, sizeof(RemoteView));
    RemoteView.Length = sizeof(RemoteView);
    Status = ZwAcceptConnectPort(MessagePort, NULL, &Message.Header, TRUE, NULL, &RemoteView);
    if (NT_SUCCESS(Status))
        Status = ZwCompleteConnectPort(*MessagePort);
    if (!NT_SUCCESS(Status))
        return Status;

    SectionSize.QuadPart = PAGE_SIZE;
    Status = ZwCreateSection(&Section, SECTION_ALL_ACCESS, NULL, &SectionSize, PAGE_READWRITE, SEC_COMMIT, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlZeroMemory(&View, sizeof(View));
    View.Length = sizeof(View);
    View.SectionHandle = Section;
    View.ViewSize = PAGE_SIZE;
    Qos.Length = sizeof(Qos);
    Qos.ImpersonationLevel = SecurityImpersonation;
    Qos.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
    Qos.EffectiveOnly = TRUE;
    Status = ZwConnectPort(&QzRmLsaPort, &Name, &Qos, &View, NULL, NULL, NULL, NULL);
    ZwClose(Section);
    return Status;
}

static VOID NTAPI
QzRmCommandServer(PVOID Context)
{
    SEP_RM_API_MESSAGE Message;
    PPORT_MESSAGE Reply = NULL;
    HANDLE MessagePort = NULL;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Context);
    Status = QzRmConnect(&MessagePort);
    ZwClose(QzRmLsaInitEvent);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(QZ_KERNEL_NAME ": security: no connection with lsass (0x%08lx)\n", Status);
        PsTerminateSystemThread(Status);
    }
    DbgPrint(QZ_KERNEL_NAME ": security: lsass connected to the reference monitor\n");

    for (;;)
    {
        Status = ZwReplyWaitReceivePort(MessagePort, NULL, Reply, &Message.Header);
        Reply = NULL;
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(QZ_KERNEL_NAME ": security: command port: 0x%08lx\n", Status);
            break;
        }
        switch (Message.Header.u2.s2.Type)
        {
            case LPC_PORT_CLOSED:
            case LPC_CLIENT_DIED:
                DbgPrint(QZ_KERNEL_NAME ": security: lsass went away\n");
                PsTerminateSystemThread(STATUS_SUCCESS);
            case LPC_REQUEST:
                switch (Message.ApiNumber)
                {
                    case RmAuditSetCommand:
                    case RmCreateLogonSession:
                    case RmDeleteLogonSession:
                        Message.u.ResultStatus = STATUS_SUCCESS;
                        break;
                    default:
                        Message.u.ResultStatus = STATUS_INVALID_SYSTEM_SERVICE;
                        break;
                }
                Reply = &Message.Header;
                break;
            default:
                break;
        }
    }
    PsTerminateSystemThread(Status);
}

VOID
QzSeRmInit(VOID)
{
    UNICODE_STRING PortName = RTL_CONSTANT_STRING(L"\\SeRmCommandPort");
    UNICODE_STRING EventName = RTL_CONSTANT_STRING(L"\\SeLsaInitEvent");
    UNICODE_STRING SecurityName = RTL_CONSTANT_STRING(L"\\Security");
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Thread, Directory;
    NTSTATUS Status;

    /* lsass keeps its LSA_AUTHENTICATION_INITIALIZED event in \Security */
    InitializeObjectAttributes(&Attributes, &SecurityName, OBJ_KERNEL_HANDLE | OBJ_PERMANENT, NULL, NULL);
    if (NT_SUCCESS(ZwCreateDirectoryObject(&Directory, DIRECTORY_ALL_ACCESS, &Attributes)))
        ZwClose(Directory);

    InitializeObjectAttributes(&Attributes, &PortName, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreatePort(&QzRmCommandPort, &Attributes, sizeof(ULONG), PORT_MAXIMUM_MESSAGE_LENGTH,
                          2 * PAGE_SIZE);
    if (NT_SUCCESS(Status))
    {
        InitializeObjectAttributes(&Attributes, &EventName, OBJ_KERNEL_HANDLE | OBJ_PERMANENT, NULL, NULL);
        Status = ZwCreateEvent(&QzRmLsaInitEvent, EVENT_ALL_ACCESS, &Attributes, NotificationEvent, FALSE);
    }
    if (NT_SUCCESS(Status))
    {
        InitializeObjectAttributes(&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, &Attributes, NULL, NULL, QzRmCommandServer, NULL);
        if (NT_SUCCESS(Status))
            ZwClose(Thread);
    }
    if (!NT_SUCCESS(Status))
        DbgPrint(QZ_KERNEL_NAME ": security: reference monitor not started (0x%08lx)\n", Status);
}

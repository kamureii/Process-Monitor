#include <ntifs.h>
#include "Shared.h"

#define DEVICE_NAME L"\\Device\\ProcessMonitor"
#define SYM_LINK_NAME L"\\??\\ProcessMonitor"

DRIVER_UNLOAD DriverUnload;

typedef struct _QUEUE_NODE {
	LIST_ENTRY ListEntry;
	PROCESS_EVENT Event;
} QUEUE_NODE, * PQUEUE_NODE;

LIST_ENTRY g_EventQueueHead;
KSPIN_LOCK g_QueueLock;

NTSTATUS DriverDispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

NTSTATUS DriverDispatchIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	UNREFERENCED_PARAMETER(DeviceObject);

	PIO_STACK_LOCATION pStackLocation = IoGetCurrentIrpStackLocation(Irp);
	ULONG controlCode = pStackLocation->Parameters.DeviceIoControl.IoControlCode;
	ULONG outLen = pStackLocation->Parameters.DeviceIoControl.OutputBufferLength;

	NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
	ULONG_PTR info = 0; 

	switch (controlCode) {
	case IOCTL_GET_PROCESS_EVENT:
		if (outLen < sizeof(PROCESS_EVENT)) {
			status = STATUS_BUFFER_TOO_SMALL;
			info = 0;
			break;
		}

		PPROCESS_EVENT userBuffer = (PPROCESS_EVENT)Irp->AssociatedIrp.SystemBuffer;
		KLOCK_QUEUE_HANDLE lockHandle;
		KeAcquireInStackQueuedSpinLock(&g_QueueLock, &lockHandle);

		if (!IsListEmpty(&g_EventQueueHead)) {
			PLIST_ENTRY entry = RemoveHeadList(&g_EventQueueHead);
			KeReleaseInStackQueuedSpinLock(&lockHandle);

			PQUEUE_NODE node = CONTAINING_RECORD(entry, QUEUE_NODE, ListEntry);

			RtlCopyMemory(userBuffer, &node->Event, sizeof(PROCESS_EVENT));
			info = sizeof(PROCESS_EVENT);

			ExFreePoolWithTag(node, 'proc');
			status = STATUS_SUCCESS;
		}
		else {
			KeReleaseInStackQueuedSpinLock(&lockHandle);
			status = STATUS_NO_MORE_ENTRIES; 
			info = 0;
		}
		break;

	case IOCTL_KILL_PROCESS:
		if (outLen < sizeof(HANDLE)) {
			status = STATUS_BUFFER_TOO_SMALL;
			info = 0;
			break;
		}
		HANDLE targetPid = *(PHANDLE)Irp->AssociatedIrp.SystemBuffer;

		OBJECT_ATTRIBUTES objAttr;
		InitializeObjectAttributes(&objAttr, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

		CLIENT_ID clientId;
		clientId.UniqueProcess = targetPid;
		clientId.UniqueThread = NULL;

		HANDLE hProcess = NULL;

		status = ZwOpenProcess(&hProcess, PROCESS_TERMINATE, &objAttr, &clientId);

		if (NT_SUCCESS(status)) {
			status = ZwTerminateProcess(hProcess, STATUS_SUCCESS);
			ZwClose(hProcess);
		}
		else {
			KdPrint(("Failed to open process with PID %d, status: 0x%X\n", (ULONG)(ULONG_PTR)targetPid, status));
		}
		info = 0;
		break;

	default:
		status = STATUS_INVALID_DEVICE_REQUEST;
		info = 0;
		break;
	}

	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = info;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return status; 
}


void ProcessNotifyCallbackEx(PEPROCESS Process, HANDLE ProcessId, PPS_CREATE_NOTIFY_INFO CreateInfo) {
	UNREFERENCED_PARAMETER(Process);

	PQUEUE_NODE node = (PQUEUE_NODE)ExAllocatePoolWithTag(NonPagedPool, sizeof(QUEUE_NODE), 'proc');
	if (node == NULL) {
		return;
	}
	RtlZeroMemory(node, sizeof(QUEUE_NODE));

	KeQuerySystemTime(&node->Event.Timestamp);
	node->Event.ProcessId = ProcessId;

	if (CreateInfo != NULL) {
		node->Event.IsCreate = TRUE;
		node->Event.ParentProcessId = CreateInfo->ParentProcessId;

		if (CreateInfo->CommandLine) {
			ULONG bytesToCopy = min(CreateInfo->CommandLine->Length, (MAX_CMD_LEN - 1) * sizeof(WCHAR));
			RtlCopyMemory(node->Event.CommandLine, CreateInfo->CommandLine->Buffer, bytesToCopy);
			node->Event.CommandLine[bytesToCopy / sizeof(WCHAR)] = L'\0';
		}
	}
	else {
		node->Event.IsCreate = FALSE;
	}
	KLOCK_QUEUE_HANDLE lockHandle;
	KeAcquireInStackQueuedSpinLock(&g_QueueLock, &lockHandle);
	InsertTailList(&g_EventQueueHead, &node->ListEntry);
	KeReleaseInStackQueuedSpinLock(&lockHandle);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
	UNREFERENCED_PARAMETER(RegistryPath);
	NTSTATUS status;
	PDEVICE_OBJECT DeviceObject = NULL;
	UNICODE_STRING devName;
	UNICODE_STRING symLink;

	InitializeListHead(&g_EventQueueHead);
	KeInitializeSpinLock(&g_QueueLock);

	DriverObject->MajorFunction[IRP_MJ_CREATE] = DriverDispatchCreateClose;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = DriverDispatchCreateClose;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DriverDispatchIoctl;
	DriverObject->DriverUnload = DriverUnload;

	RtlInitUnicodeString(&devName, DEVICE_NAME);
	RtlInitUnicodeString(&symLink, SYM_LINK_NAME);

	status = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, 0, FALSE, &DeviceObject);
	if (!NT_SUCCESS(status)) {
		KdPrint(("Failed to create device\n"));
		return status;
	}

	// 4. Tạo Symbolic Link
	status = IoCreateSymbolicLink(&symLink, &devName);
	if (!NT_SUCCESS(status)) {
		KdPrint(("Failed to create symbolic link\n"));
		IoDeleteDevice(DeviceObject);
		return status;
	}

	status = PsSetCreateProcessNotifyRoutineEx(ProcessNotifyCallbackEx, FALSE);
	if (!NT_SUCCESS(status)) {
		KdPrint(("Failed to register callback\n"));
		IoDeleteSymbolicLink(&symLink);
		IoDeleteDevice(DeviceObject);
		return status;
	}

	KdPrint(("Driver loaded successfully\n"));
	return STATUS_SUCCESS;
}

void DriverUnload(PDRIVER_OBJECT DriverObject) {
	PsSetCreateProcessNotifyRoutineEx(ProcessNotifyCallbackEx, TRUE);

	UNICODE_STRING symLink;
	RtlInitUnicodeString(&symLink, SYM_LINK_NAME);
	IoDeleteSymbolicLink(&symLink);

	if (DriverObject->DeviceObject) {
		IoDeleteDevice(DriverObject->DeviceObject);
	}

	KLOCK_QUEUE_HANDLE lockHandle;
	KeAcquireInStackQueuedSpinLock(&g_QueueLock, &lockHandle);
	while (!IsListEmpty(&g_EventQueueHead)) {
		PLIST_ENTRY entry = RemoveHeadList(&g_EventQueueHead);
		PQUEUE_NODE node = CONTAINING_RECORD(entry, QUEUE_NODE, ListEntry);
		ExFreePoolWithTag(node, 'proc');
	}
	KeReleaseInStackQueuedSpinLock(&lockHandle);

	KdPrint(("Driver unloaded\n"));
}
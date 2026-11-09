#include <ntifs.h>
#include <wdmsec.h>
#include "../../Common/Shared.h"

#ifndef PROCESS_TERMINATE
#define PROCESS_TERMINATE (0x0001)
#endif

#define POOL_TAG 'proc'
#define MAX_QUEUED_EVENTS 4096

// {861CD76D-DF21-4C33-9B08-A929F887E58E}
static const GUID PROCESS_MONITOR_CLASS_GUID =
	{ 0x861cd76d, 0xdf21, 0x4c33, { 0x9b, 0x08, 0xa9, 0x29, 0xf8, 0x87, 0xe5, 0x8e } };

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD DriverUnload;
_Dispatch_type_(IRP_MJ_CREATE) _Dispatch_type_(IRP_MJ_CLOSE) DRIVER_DISPATCH DriverDispatchCreateClose;
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL) DRIVER_DISPATCH DriverDispatchIoctl;

typedef struct _QUEUE_NODE {
	LIST_ENTRY ListEntry;
	PROCESS_EVENT Event;
} QUEUE_NODE, * PQUEUE_NODE;

// Event queue, filled by the process callback and drained by IOCTL_GET_PROCESS_EVENT.
LIST_ENTRY g_EventQueueHead;
ULONG g_EventCount;
KSPIN_LOCK g_QueueLock;

// Block rules. A fast mutex (not a spin lock) because image names may live in paged memory.
FAST_MUTEX g_RuleLock;
WCHAR g_BlockRules[MAX_BLOCK_RULES][MAX_IMAGE_NAME_LEN];
ULONG g_BlockRuleCount;

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
	ULONG inLen = pStackLocation->Parameters.DeviceIoControl.InputBufferLength;
	ULONG outLen = pStackLocation->Parameters.DeviceIoControl.OutputBufferLength;
	PVOID buffer = Irp->AssociatedIrp.SystemBuffer;

	NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
	ULONG_PTR info = 0;

	switch (controlCode) {
	case IOCTL_GET_PROCESS_EVENT: {
		if (outLen < sizeof(PROCESS_EVENT)) {
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		PQUEUE_NODE node = NULL;
		KLOCK_QUEUE_HANDLE lockHandle;
		KeAcquireInStackQueuedSpinLock(&g_QueueLock, &lockHandle);
		if (!IsListEmpty(&g_EventQueueHead)) {
			node = CONTAINING_RECORD(RemoveHeadList(&g_EventQueueHead), QUEUE_NODE, ListEntry);
			g_EventCount--;
		}
		KeReleaseInStackQueuedSpinLock(&lockHandle);

		// An empty queue is not an error: succeed with 0 bytes so the client can tell it apart from a failure.
		if (node != NULL) {
			RtlCopyMemory(buffer, &node->Event, sizeof(PROCESS_EVENT));
			ExFreePoolWithTag(node, POOL_TAG);
			info = sizeof(PROCESS_EVENT);
		}
		status = STATUS_SUCCESS;
		break;
	}

	case IOCTL_KILL_PROCESS: {
		// The PID arrives in the input buffer.
		if (inLen < sizeof(KILL_REQUEST)) {
			status = STATUS_INVALID_PARAMETER;
			break;
		}
		ULONG targetPid = ((PKILL_REQUEST)buffer)->ProcessId;

		// Refuse the Idle (0) and System (4) processes.
		if (targetPid == 0 || targetPid == 4) {
			status = STATUS_ACCESS_DENIED;
			break;
		}
		// Terminating the caller from inside its own IOCTL would leave this IRP uncompleted.
		if (targetPid == (ULONG)(ULONG_PTR)PsGetCurrentProcessId()) {
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		OBJECT_ATTRIBUTES objAttr;
		InitializeObjectAttributes(&objAttr, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

		CLIENT_ID clientId;
		clientId.UniqueProcess = (HANDLE)(ULONG_PTR)targetPid;
		clientId.UniqueThread = NULL;

		HANDLE hProcess = NULL;
		status = ZwOpenProcess(&hProcess, PROCESS_TERMINATE, &objAttr, &clientId);
		if (NT_SUCCESS(status)) {
			status = ZwTerminateProcess(hProcess, STATUS_SUCCESS);
			ZwClose(hProcess);
		}
		else {
			KdPrint(("Failed to open process with PID %lu, status: 0x%X\n", targetPid, status));
		}
		break;
	}

	case IOCTL_ADD_BLOCK_RULE: {
		if (inLen < sizeof(BLOCK_RULE)) {
			status = STATUS_INVALID_PARAMETER;
			break;
		}
		PBLOCK_RULE rule = (PBLOCK_RULE)buffer;

		// Never trust the caller to null-terminate.
		SIZE_T len = 0;
		while (len < MAX_IMAGE_NAME_LEN && rule->ImageName[len] != L'\0') {
			len++;
		}
		if (len == 0 || len == MAX_IMAGE_NAME_LEN) {
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		ExAcquireFastMutex(&g_RuleLock);
		if (g_BlockRuleCount < MAX_BLOCK_RULES) {
			RtlCopyMemory(g_BlockRules[g_BlockRuleCount], rule->ImageName, (len + 1) * sizeof(WCHAR));
			g_BlockRuleCount++;
			status = STATUS_SUCCESS;
		}
		else {
			status = STATUS_INSUFFICIENT_RESOURCES;
		}
		ExReleaseFastMutex(&g_RuleLock);
		break;
	}

	case IOCTL_CLEAR_BLOCK_RULES:
		ExAcquireFastMutex(&g_RuleLock);
		g_BlockRuleCount = 0;
		ExReleaseFastMutex(&g_RuleLock);
		status = STATUS_SUCCESS;
		break;

	default:
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}

	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = info;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return status;
}

// Returns TRUE if the file-name part of ImageFileName matches a block rule (case-insensitive).
BOOLEAN IsImageBlocked(PCUNICODE_STRING ImageFileName) {
	if (ImageFileName == NULL || ImageFileName->Buffer == NULL) {
		return FALSE;
	}

	// "\??\C:\Windows\System32\notepad.exe" -> "notepad.exe"
	UNICODE_STRING fileName = *ImageFileName;
	USHORT charCount = ImageFileName->Length / sizeof(WCHAR);
	for (USHORT i = charCount; i > 0; i--) {
		if (ImageFileName->Buffer[i - 1] == L'\\') {
			fileName.Buffer = &ImageFileName->Buffer[i];
			fileName.Length = (USHORT)((charCount - i) * sizeof(WCHAR));
			fileName.MaximumLength = fileName.Length;
			break;
		}
	}

	BOOLEAN blocked = FALSE;
	ExAcquireFastMutex(&g_RuleLock);
	for (ULONG i = 0; i < g_BlockRuleCount && !blocked; i++) {
		UNICODE_STRING ruleName;
		RtlInitUnicodeString(&ruleName, g_BlockRules[i]);
		blocked = RtlEqualUnicodeString(&fileName, &ruleName, TRUE);
	}
	ExReleaseFastMutex(&g_RuleLock);

	return blocked;
}

void ProcessNotifyCallbackEx(PEPROCESS Process, HANDLE ProcessId, PPS_CREATE_NOTIFY_INFO CreateInfo) {
	UNREFERENCED_PARAMETER(Process);

	BOOLEAN blocked = FALSE;
	if (CreateInfo != NULL && IsImageBlocked(CreateInfo->ImageFileName)) {
		CreateInfo->CreationStatus = STATUS_ACCESS_DENIED;
		blocked = TRUE;
	}

	PQUEUE_NODE node = (PQUEUE_NODE)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(QUEUE_NODE), POOL_TAG);
	if (node == NULL) {
		return;
	}
	RtlZeroMemory(node, sizeof(QUEUE_NODE));

	KeQuerySystemTime(&node->Event.Timestamp);
	node->Event.ProcessId = (ULONGLONG)(ULONG_PTR)ProcessId;

	if (CreateInfo != NULL) {
		node->Event.IsCreate = TRUE;
		node->Event.IsBlocked = blocked;
		node->Event.ParentProcessId = (ULONGLONG)(ULONG_PTR)CreateInfo->ParentProcessId;

		if (CreateInfo->CommandLine) {
			ULONG bytesToCopy = min(CreateInfo->CommandLine->Length, (MAX_CMD_LEN - 1) * sizeof(WCHAR));
			RtlCopyMemory(node->Event.CommandLine, CreateInfo->CommandLine->Buffer, bytesToCopy);
			node->Event.CommandLine[bytesToCopy / sizeof(WCHAR)] = L'\0';
		}
	}
	else {
		node->Event.IsCreate = FALSE;
	}

	PQUEUE_NODE dropped = NULL;
	KLOCK_QUEUE_HANDLE lockHandle;
	KeAcquireInStackQueuedSpinLock(&g_QueueLock, &lockHandle);
	if (g_EventCount >= MAX_QUEUED_EVENTS) {
		// No client is draining the queue: drop the oldest event so non-paged pool usage stays bounded.
		dropped = CONTAINING_RECORD(RemoveHeadList(&g_EventQueueHead), QUEUE_NODE, ListEntry);
		g_EventCount--;
	}
	InsertTailList(&g_EventQueueHead, &node->ListEntry);
	g_EventCount++;
	KeReleaseInStackQueuedSpinLock(&lockHandle);

	if (dropped != NULL) {
		ExFreePoolWithTag(dropped, POOL_TAG);
	}
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
	UNREFERENCED_PARAMETER(RegistryPath);
	NTSTATUS status;
	PDEVICE_OBJECT DeviceObject = NULL;
	UNICODE_STRING devName;
	UNICODE_STRING symLink;

	InitializeListHead(&g_EventQueueHead);
	KeInitializeSpinLock(&g_QueueLock);
	ExInitializeFastMutex(&g_RuleLock);

	DriverObject->MajorFunction[IRP_MJ_CREATE] = DriverDispatchCreateClose;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = DriverDispatchCreateClose;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DriverDispatchIoctl;
	DriverObject->DriverUnload = DriverUnload;

	RtlInitUnicodeString(&devName, PROCESS_MONITOR_DEVICE_NAME);
	RtlInitUnicodeString(&symLink, PROCESS_MONITOR_SYMLINK_NAME);

	// Only SYSTEM and Administrators may open the device: it can terminate any process.
	status = IoCreateDeviceSecure(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE,
		&SDDL_DEVOBJ_SYS_ALL_ADM_ALL, &PROCESS_MONITOR_CLASS_GUID, &DeviceObject);
	if (!NT_SUCCESS(status)) {
		KdPrint(("Failed to create device\n"));
		return status;
	}

	status = IoCreateSymbolicLink(&symLink, &devName);
	if (!NT_SUCCESS(status)) {
		KdPrint(("Failed to create symbolic link\n"));
		IoDeleteDevice(DeviceObject);
		return status;
	}

	// Requires the image to be linked with /INTEGRITYCHECK, otherwise returns STATUS_ACCESS_DENIED.
	status = PsSetCreateProcessNotifyRoutineEx(ProcessNotifyCallbackEx, FALSE);
	if (!NT_SUCCESS(status)) {
		KdPrint(("Failed to register callback, status: 0x%X\n", status));
		IoDeleteSymbolicLink(&symLink);
		IoDeleteDevice(DeviceObject);
		return status;
	}

	KdPrint(("Driver loaded successfully\n"));
	return STATUS_SUCCESS;
}

void DriverUnload(PDRIVER_OBJECT DriverObject) {
	// Unregistering waits for in-flight callbacks, so the queue is stable afterwards.
	PsSetCreateProcessNotifyRoutineEx(ProcessNotifyCallbackEx, TRUE);

	UNICODE_STRING symLink;
	RtlInitUnicodeString(&symLink, PROCESS_MONITOR_SYMLINK_NAME);
	IoDeleteSymbolicLink(&symLink);

	if (DriverObject->DeviceObject) {
		IoDeleteDevice(DriverObject->DeviceObject);
	}

	KLOCK_QUEUE_HANDLE lockHandle;
	KeAcquireInStackQueuedSpinLock(&g_QueueLock, &lockHandle);
	while (!IsListEmpty(&g_EventQueueHead)) {
		PLIST_ENTRY entry = RemoveHeadList(&g_EventQueueHead);
		PQUEUE_NODE node = CONTAINING_RECORD(entry, QUEUE_NODE, ListEntry);
		ExFreePoolWithTag(node, POOL_TAG);
	}
	g_EventCount = 0;
	KeReleaseInStackQueuedSpinLock(&lockHandle);

	KdPrint(("Driver unloaded\n"));
}

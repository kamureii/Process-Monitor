#pragma once

// Interface shared by the kernel driver and the user-mode client.
// All fields are fixed-width so the layout is identical for 32-bit and 64-bit callers.

#ifdef _KERNEL_MODE
#include <ntdef.h>
#include <devioctl.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif

#define PROCESS_MONITOR_DEVICE_NAME  L"\\Device\\ProcessMonitor"
#define PROCESS_MONITOR_SYMLINK_NAME L"\\??\\ProcessMonitor"
#define PROCESS_MONITOR_USER_PATH    L"\\\\.\\ProcessMonitor"

// Output: PROCESS_EVENT. Returns 0 bytes when the queue is empty.
#define IOCTL_GET_PROCESS_EVENT   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
// Input: KILL_REQUEST.
#define IOCTL_KILL_PROCESS        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
// Input: BLOCK_RULE.
#define IOCTL_ADD_BLOCK_RULE      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
// No input or output.
#define IOCTL_CLEAR_BLOCK_RULES   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define MAX_CMD_LEN        256
#define MAX_IMAGE_NAME_LEN 64
#define MAX_BLOCK_RULES    32

typedef struct _PROCESS_EVENT {
	LARGE_INTEGER Timestamp;       // UTC, FILETIME format
	ULONGLONG ProcessId;
	ULONGLONG ParentProcessId;     // only valid when IsCreate is TRUE
	BOOLEAN IsCreate;              // TRUE = process created, FALSE = process exited
	BOOLEAN IsBlocked;             // TRUE = creation was denied by a block rule
	WCHAR CommandLine[MAX_CMD_LEN];
} PROCESS_EVENT, * PPROCESS_EVENT;

typedef struct _KILL_REQUEST {
	ULONG ProcessId;
} KILL_REQUEST, * PKILL_REQUEST;

typedef struct _BLOCK_RULE {
	WCHAR ImageName[MAX_IMAGE_NAME_LEN]; // file name only, null-terminated, e.g. L"notepad.exe"
} BLOCK_RULE, * PBLOCK_RULE;

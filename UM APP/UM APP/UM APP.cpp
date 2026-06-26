#include <iostream>
#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include "Shared.h"
#include <io.h>     
#include <fcntl.h>  

std::atomic<bool> g_running(true);  

std::wstring FormatTimeStamp(LARGE_INTEGER systemTime) {
	FILETIME ftUtc, ftLocal;
	SYSTEMTIME stUtc, stLocal;

	ftUtc.dwLowDateTime = systemTime.LowPart;
	ftUtc.dwHighDateTime = systemTime.HighPart;

	FileTimeToSystemTime(&ftUtc, &stUtc);
	SystemTimeToTzSpecificLocalTime(NULL, &stUtc, &stLocal);

	wchar_t buffer[256];
	swprintf(buffer, 256, L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
		stLocal.wYear, stLocal.wMonth, stLocal.wDay,
		stLocal.wHour, stLocal.wMinute, stLocal.wSecond);

	return std::wstring(buffer);
}

void ProcessMonitorThread(HANDLE hDevice) {
	if (hDevice == INVALID_HANDLE_VALUE) {
		std::wcerr << L"Failed to open device: " << GetLastError() << std::endl;
		return;
	}
	PROCESS_EVENT event;
	DWORD bytesReturned;
	while (g_running) {
		BOOL result = DeviceIoControl(hDevice, IOCTL_GET_PROCESS_EVENT, NULL, 0, &event, sizeof(event), &bytesReturned, NULL);

		if (result && bytesReturned == sizeof(event)) {
			std::wstring timestamp = FormatTimeStamp(event.Timestamp);

			ULONG_PTR processId = reinterpret_cast<ULONG_PTR>(event.ProcessId);
			ULONG_PTR parentProcessId = reinterpret_cast<ULONG_PTR>(event.ParentProcessId);

			if (event.IsCreate) {
				std::wcout << L"[CREATE] " << timestamp << L" | PID: " << processId << L" | PPID: " << parentProcessId << L" | CommandLine: " << event.CommandLine << std::endl;
			}
			else {
				std::wcout << L"[DELETE] " << timestamp << L" | PID: " << processId << std::endl;
			}
		}
		else {
			Sleep(100);
		}
	}
}

void KillProcess(HANDLE hDevice, HANDLE processId) {
	DWORD bytesReturned;
	BOOL result = DeviceIoControl(hDevice, IOCTL_KILL_PROCESS, &processId, sizeof(processId), NULL, 0, &bytesReturned, NULL);
	if (!result) {
		std::wcerr << L"Failed to kill process " << reinterpret_cast<ULONG_PTR>(processId) << L": " << GetLastError() << std::endl;
	}
	else {
		std::wcout << L"Successfully killed process " << reinterpret_cast<ULONG_PTR>(processId) << std::endl;
	}
}

int main()
{
	_setmode(_fileno(stdout), _O_U16TEXT);
	_setmode(_fileno(stdin), _O_U16TEXT);

	std::wcout << L"=== PROCESS MONITOR ===\n";

	HANDLE hDevice = CreateFile(L"\\\\.\\ProcessMonitor",
		GENERIC_READ | GENERIC_WRITE,
		0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hDevice == INVALID_HANDLE_VALUE) {
		std::wcout << L"Error! Cant connect to Driver.\n";
		system("pause");
		return 1;
	}

	std::wcout << L"Connected success!";
	std::wcout << L"Type PID then Enter to KILL a process. Press '0' to stop.\n\n";

	std::thread monitorThread(ProcessMonitorThread, hDevice);

	HANDLE targetPid;
	while (true) {
		std::wcin >> targetPid;

		if (targetPid == 0) {
			std::wcout << L"Disconnecting...\n";
			break;
		}

		KillProcess(hDevice, targetPid);
	}

	g_running = false;          
	monitorThread.join();       
	CloseHandle(hDevice);         

	return 0;
}
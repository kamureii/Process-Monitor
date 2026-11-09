#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <io.h>
#include <fcntl.h>
#include "../../Common/Shared.h"

std::atomic<bool> g_running(true);
std::mutex g_consoleMutex; // keeps output from the monitor thread and the command loop from interleaving

std::wstring FormatTimeStamp(LARGE_INTEGER systemTime) {
	FILETIME ftUtc;
	SYSTEMTIME stUtc, stLocal;

	ftUtc.dwLowDateTime = systemTime.LowPart;
	ftUtc.dwHighDateTime = systemTime.HighPart;

	FileTimeToSystemTime(&ftUtc, &stUtc);
	SystemTimeToTzSpecificLocalTime(NULL, &stUtc, &stLocal);

	wchar_t buffer[64];
	swprintf(buffer, 64, L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
		stLocal.wYear, stLocal.wMonth, stLocal.wDay,
		stLocal.wHour, stLocal.wMinute, stLocal.wSecond, stLocal.wMilliseconds);

	return std::wstring(buffer);
}

void ProcessMonitorThread(HANDLE hDevice) {
	PROCESS_EVENT event;
	DWORD bytesReturned;
	while (g_running) {
		BOOL result = DeviceIoControl(hDevice, IOCTL_GET_PROCESS_EVENT, NULL, 0, &event, sizeof(event), &bytesReturned, NULL);
		if (!result) {
			DWORD error = GetLastError();
			std::lock_guard<std::mutex> lock(g_consoleMutex);
			std::wcerr << L"Failed to read events from driver: " << error << std::endl;
			return;
		}

		if (bytesReturned != sizeof(event)) {
			// Queue is empty.
			Sleep(100);
			continue;
		}

		std::wstring timestamp = FormatTimeStamp(event.Timestamp);
		std::lock_guard<std::mutex> lock(g_consoleMutex);
		if (event.IsCreate) {
			std::wcout << (event.IsBlocked ? L"[BLOCK ] " : L"[CREATE] ") << timestamp
				<< L" | PID: " << event.ProcessId << L" | PPID: " << event.ParentProcessId
				<< L" | CommandLine: " << event.CommandLine << std::endl;
		}
		else {
			std::wcout << L"[EXIT  ] " << timestamp << L" | PID: " << event.ProcessId << std::endl;
		}
	}
}

void SendRequest(HANDLE hDevice, DWORD controlCode, LPVOID input, DWORD inputSize, const std::wstring& description) {
	DWORD bytesReturned;
	BOOL result = DeviceIoControl(hDevice, controlCode, input, inputSize, NULL, 0, &bytesReturned, NULL);
	DWORD error = GetLastError();

	std::lock_guard<std::mutex> lock(g_consoleMutex);
	if (result) {
		std::wcout << L"OK: " << description << std::endl;
	}
	else {
		std::wcerr << L"FAILED: " << description << L" (error " << error << L")" << std::endl;
	}
}

void KillProcess(HANDLE hDevice, ULONG processId) {
	KILL_REQUEST request = { processId };
	SendRequest(hDevice, IOCTL_KILL_PROCESS, &request, sizeof(request), L"kill process " + std::to_wstring(processId));
}

void AddBlockRule(HANDLE hDevice, const std::wstring& imageName) {
	BLOCK_RULE rule = {};
	wcscpy_s(rule.ImageName, imageName.c_str());
	SendRequest(hDevice, IOCTL_ADD_BLOCK_RULE, &rule, sizeof(rule), L"block " + imageName);
}

void ClearBlockRules(HANDLE hDevice) {
	SendRequest(hDevice, IOCTL_CLEAR_BLOCK_RULES, NULL, 0, L"clear all block rules");
}

void PrintHelp() {
	std::lock_guard<std::mutex> lock(g_consoleMutex);
	std::wcout << L"Commands:\n"
		<< L"  kill <pid>      terminate a process\n"
		<< L"  block <name>    block new launches of an image, e.g. block notepad.exe\n"
		<< L"  clear           remove all block rules\n"
		<< L"  help            show this list\n"
		<< L"  quit            exit\n\n";
}

int main()
{
	_setmode(_fileno(stdout), _O_U16TEXT);
	_setmode(_fileno(stderr), _O_U16TEXT);
	_setmode(_fileno(stdin), _O_U16TEXT);

	std::wcout << L"=== PROCESS MONITOR ===\n";

	HANDLE hDevice = CreateFile(PROCESS_MONITOR_USER_PATH,
		GENERIC_READ | GENERIC_WRITE,
		0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hDevice == INVALID_HANDLE_VALUE) {
		DWORD error = GetLastError();
		std::wcout << L"Error! Can't connect to driver (error " << error << L").\n";
		if (error == ERROR_FILE_NOT_FOUND) {
			std::wcout << L"Is the driver loaded? (sc start ProcessMonitor)\n";
		}
		else if (error == ERROR_ACCESS_DENIED) {
			std::wcout << L"Run this program as Administrator.\n";
		}
		system("pause");
		return 1;
	}

	std::wcout << L"Connected successfully!\n";
	PrintHelp();

	std::thread monitorThread(ProcessMonitorThread, hDevice);

	std::wstring line;
	while (std::getline(std::wcin, line)) {
		std::wistringstream input(line);
		std::wstring command;
		if (!(input >> command)) {
			continue;
		}

		if (command == L"quit" || command == L"exit") {
			break;
		}
		else if (command == L"help") {
			PrintHelp();
		}
		else if (command == L"kill") {
			ULONG pid;
			wchar_t extra;
			if (!(input >> pid) || (input >> extra)) {
				std::lock_guard<std::mutex> lock(g_consoleMutex);
				std::wcerr << L"Usage: kill <pid>   (decimal process ID)" << std::endl;
				continue;
			}
			KillProcess(hDevice, pid);
		}
		else if (command == L"block") {
			std::wstring name;
			std::getline(input >> std::ws, name);
			name.erase(name.find_last_not_of(L" \t") + 1);
			if (name.empty() || name.size() >= MAX_IMAGE_NAME_LEN || name.find(L'\\') != std::wstring::npos) {
				std::lock_guard<std::mutex> lock(g_consoleMutex);
				std::wcerr << L"Usage: block <file name>   (e.g. notepad.exe, max "
					<< MAX_IMAGE_NAME_LEN - 1 << L" characters, no path)" << std::endl;
				continue;
			}
			AddBlockRule(hDevice, name);
		}
		else if (command == L"clear") {
			ClearBlockRules(hDevice);
		}
		else {
			std::lock_guard<std::mutex> lock(g_consoleMutex);
			std::wcerr << L"Unknown command. Type 'help'." << std::endl;
		}
	}

	g_running = false;
	monitorThread.join();
	CloseHandle(hDevice);
	std::wcout << L"Disconnected.\n";

	return 0;
}

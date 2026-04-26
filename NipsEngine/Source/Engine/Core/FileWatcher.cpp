#include "FileWatcher.h"

#include "Editor/UI/EditorConsoleWidget.h"

#include <filesystem>

FFileWatcher::~FFileWatcher()
{
	Stop();
}

bool FFileWatcher::Start(const std::wstring& InDirectory, bool bInRecursive)
{
	Stop();

	WatchedDirectory = InDirectory;
	bRecursive = bInRecursive;
	DirectoryHandle = CreateFileW(
		WatchedDirectory.c_str(),
		FILE_LIST_DIRECTORY,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS,
		nullptr);

	if (DirectoryHandle == INVALID_HANDLE_VALUE)
	{
		UE_LOG("[FileWatcher] Failed to watch directory");
		return false;
	}

	bStopRequested = false;
	WatcherThread = std::thread(&FFileWatcher::WatchLoop, this);
	return true;
}

void FFileWatcher::Stop()
{
	bStopRequested = true;

	if (WatcherThread.joinable())
	{
		CancelSynchronousIo(WatcherThread.native_handle());
		WatcherThread.join();
	}

	if (DirectoryHandle != INVALID_HANDLE_VALUE)
	{
		CloseHandle(DirectoryHandle);
		DirectoryHandle = INVALID_HANDLE_VALUE;
	}

	std::lock_guard<std::mutex> Lock(ChangedFilesMutex);
	ChangedFiles.clear();
}

std::vector<std::wstring> FFileWatcher::DequeueChangedFiles()
{
	std::lock_guard<std::mutex> Lock(ChangedFilesMutex);
	std::vector<std::wstring> Result(ChangedFiles.begin(), ChangedFiles.end());
	ChangedFiles.clear();
	return Result;
}

void FFileWatcher::WatchLoop()
{
	constexpr DWORD BufferSize = 16 * 1024;
	BYTE Buffer[BufferSize] = {};

	while (!bStopRequested)
	{
		DWORD BytesReturned = 0;
		const BOOL bReadSucceeded = ReadDirectoryChangesW(
			DirectoryHandle,
			Buffer,
			BufferSize,
			bRecursive,
			FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
			&BytesReturned,
			nullptr,
			nullptr);

		if (!bReadSucceeded)
		{
			const DWORD ErrorCode = GetLastError();
			if (bStopRequested || ErrorCode == ERROR_OPERATION_ABORTED)
			{
				break;
			}

			UE_LOG("[FileWatcher] ReadDirectoryChangesW failed: %lu", static_cast<unsigned long>(ErrorCode));
			Sleep(50);
			continue;
		}

		BYTE* Current = Buffer;
		while (Current != nullptr)
		{
			FILE_NOTIFY_INFORMATION* Notification = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(Current);
			std::wstring RelativePath(Notification->FileName, Notification->FileNameLength / sizeof(WCHAR));
			const std::wstring FullPath =
				(std::filesystem::path(WatchedDirectory) / std::filesystem::path(RelativePath)).lexically_normal().generic_wstring();
			EnqueueChangedFile(FullPath);

			if (Notification->NextEntryOffset == 0)
			{
				break;
			}

			Current += Notification->NextEntryOffset;
		}
	}
}

void FFileWatcher::EnqueueChangedFile(const std::wstring& InFilePath)
{
	std::lock_guard<std::mutex> Lock(ChangedFilesMutex);
	ChangedFiles.push_back(InFilePath);
}

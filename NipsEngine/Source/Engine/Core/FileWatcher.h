#pragma once

#include <Windows.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class FFileWatcher
{
public:
	FFileWatcher() = default;
	~FFileWatcher();

	bool Start(const std::wstring& InDirectory, bool bInRecursive = true);
	void Stop();

	std::vector<std::wstring> DequeueChangedFiles();

private:
	void WatchLoop();
	void EnqueueChangedFile(const std::wstring& InFilePath);

private:
	std::wstring WatchedDirectory;
	bool bRecursive = true;
	std::thread WatcherThread;
	std::atomic<bool> bStopRequested = false;
	HANDLE DirectoryHandle = INVALID_HANDLE_VALUE;
	std::mutex ChangedFilesMutex;
	std::deque<std::wstring> ChangedFiles;
};

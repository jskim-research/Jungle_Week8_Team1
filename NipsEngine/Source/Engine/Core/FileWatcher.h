#pragma once


#include <atomic>
#include <mutex>
#include <thread>

class FFileWatcher
{
public:
	FFileWatcher() = default;
	~FFileWatcher();

	bool Start(const FWString& InDirectory, bool bInRecursive = true);
	void Stop();

	TArray<FWString> DequeueChangedFiles();

private:
	void WatchLoop();
	void EnqueueChangedFile(const FWString& InFilePath);

private:
	FWString WatchedDirectory;
	bool bRecursive = true;
	std::thread WatcherThread;
	std::atomic<bool> bStopRequested = false;
	HANDLE DirectoryHandle = INVALID_HANDLE_VALUE;
	std::mutex ChangedFilesMutex;
	std::deque<FWString> ChangedFiles;
};

#pragma once

#include "Core/Containers/Array.h"
#include "Core/Containers/Map.h"
#include "Core/CoreTypes.h"
#include <Windows.h>
#include <cfloat>
#include "Core/Singleton.h"


// --- 빌드 설정 ---
#ifndef STATS
#if defined(_DEBUG) || (defined(DEBUG) && DEBUG)
#define STATS 1
#else
#define STATS 0
#endif
#endif

// --- Stat Entry ---
struct FStatEntry
{
	const char* Name = nullptr;
	uint32 CallCount = 0;
	double TotalTime = 0.0;		// seconds
	double MaxTime   = 0.0;
	double MinTime   = DBL_MAX;
	double LastTime  = 0.0;

	double GetAvgTime() const { return CallCount > 0 ? TotalTime / CallCount : 0.0; }
};

// --- Stat Manager (싱글턴) ---
class FStatManager : public TSingleton<FStatManager>
{
	friend class TSingleton<FStatManager>;

public:
	void RecordTime(const char* Name, double ElapsedSeconds);
	void TakeSnapshot();
	const TArray<FStatEntry>& GetSnapshot() const { return Snapshot; }
	LARGE_INTEGER GetFrequency() const { return Frequency; }

private:
	FStatManager();
	~FStatManager() = default;

	TMap<const char*, FStatEntry> Stats;
	TArray<FStatEntry> Snapshot;
	LARGE_INTEGER Frequency;
};

class FFrameSpikeProfiler : public TSingleton<FFrameSpikeProfiler>
{
	friend class TSingleton<FFrameSpikeProfiler>;

public:
	void BeginFrame();
	void RecordSection(const char* Name, double ElapsedSeconds);
	void AddCounter(const char* Name, uint64 Amount = 1);
	void EndFrame();

private:
	FFrameSpikeProfiler() = default;
	~FFrameSpikeProfiler() = default;

	TMap<const char*, double> SectionTimesMs;
	TMap<const char*, uint64> CounterValues;
	LARGE_INTEGER FrameStartTime = {};
	double RollingAverageMs = 0.0;
	uint64 FrameIndex = 0;
};

// --- Scoped Timer (RAII) ---
class FScopedTimer
{
public:
	FScopedTimer(const char* InName) : Name(InName)
	{
		QueryPerformanceCounter(&StartTime);
	}

	~FScopedTimer()
	{
		LARGE_INTEGER EndTime;
		QueryPerformanceCounter(&EndTime);
		double Elapsed = static_cast<double>(EndTime.QuadPart - StartTime.QuadPart)
			/ static_cast<double>(FStatManager::Get().GetFrequency().QuadPart);
		FStatManager::Get().RecordTime(Name, Elapsed);
	}

private:
	const char* Name;
	LARGE_INTEGER StartTime;
};

class FFrameSectionTimer
{
public:
	FFrameSectionTimer(const char* InName)
		: Name(InName)
	{
		QueryPerformanceCounter(&StartTime);
	}

	~FFrameSectionTimer()
	{
		LARGE_INTEGER EndTime;
		QueryPerformanceCounter(&EndTime);

		const double Elapsed =
			static_cast<double>(EndTime.QuadPart - StartTime.QuadPart) /
			static_cast<double>(FStatManager::Get().GetFrequency().QuadPart);
		FFrameSpikeProfiler::Get().RecordSection(Name, Elapsed);
	}

private:
	const char* Name;
	LARGE_INTEGER StartTime;
};

// --- SCOPE_STAT 매크로 ---
#define SCOPE_STAT_CONCAT2(a, b) a##b
#define SCOPE_STAT_CONCAT(a, b)  SCOPE_STAT_CONCAT2(a, b)

#if STATS
#define SCOPE_STAT(Name) FScopedTimer SCOPE_STAT_CONCAT(_ScopedTimer_, __COUNTER__)(Name)
#define FRAME_SPIKE_SCOPE(Name) FFrameSectionTimer SCOPE_STAT_CONCAT(_FrameSectionTimer_, __COUNTER__)(Name)
#else
#define SCOPE_STAT(Name) ((void)0)
#define FRAME_SPIKE_SCOPE(Name) ((void)0)
#endif


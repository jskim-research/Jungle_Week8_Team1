#include "Core/Logging/Stats.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace
{
	void EmitSpikeLine(const std::string& Line)
	{
		const std::string Output = Line + "\n";
		OutputDebugStringA(Output.c_str());
		std::fputs(Output.c_str(), stderr);
	}
}

FStatManager::FStatManager()
{
	QueryPerformanceFrequency(&Frequency);
}

void FStatManager::RecordTime(const char* Name, double ElapsedSeconds)
{
	auto it = Stats.find(Name);
	if (it == Stats.end())
	{
		FStatEntry Entry;
		Entry.Name = Name;
		Entry.CallCount = 1;
		Entry.TotalTime = ElapsedSeconds;
		Entry.MaxTime = ElapsedSeconds;
		Entry.MinTime = ElapsedSeconds;
		Entry.LastTime = ElapsedSeconds;
		Stats[Name] = Entry;
		return;
	}

	FStatEntry& Entry = it->second;
	Entry.CallCount++;
	Entry.TotalTime += ElapsedSeconds;
	Entry.MaxTime = (std::max)(Entry.MaxTime, ElapsedSeconds);
	Entry.MinTime = (std::min)(Entry.MinTime, ElapsedSeconds);
	Entry.LastTime = ElapsedSeconds;
}

void FStatManager::TakeSnapshot()
{
	Snapshot.clear();
	Snapshot.reserve(Stats.size());

	for (auto& [Key, Entry] : Stats)
	{
		Snapshot.push_back(Entry);

		// Reset for next frame
		Entry.CallCount = 0;
		Entry.TotalTime = 0.0;
		Entry.MaxTime = 0.0;
		Entry.MinTime = DBL_MAX;
		Entry.LastTime = 0.0;
	}
}

void FFrameSpikeProfiler::BeginFrame()
{
	QueryPerformanceCounter(&FrameStartTime);
	SectionTimesMs.clear();
	CounterValues.clear();
}

void FFrameSpikeProfiler::RecordSection(const char* Name, double ElapsedSeconds)
{
	if (Name == nullptr)
	{
		return;
	}

	SectionTimesMs[Name] += ElapsedSeconds * 1000.0;
}

void FFrameSpikeProfiler::AddCounter(const char* Name, uint64 Amount)
{
	if (Name == nullptr || Amount == 0)
	{
		return;
	}

	CounterValues[Name] += Amount;
}

void FFrameSpikeProfiler::EndFrame()
{
	LARGE_INTEGER EndTime;
	QueryPerformanceCounter(&EndTime);

	const double FrameMs =
		static_cast<double>(EndTime.QuadPart - FrameStartTime.QuadPart) * 1000.0 /
		static_cast<double>(FStatManager::Get().GetFrequency().QuadPart);

	const double PreviousAverageMs = RollingAverageMs;
	const bool bHasBaseline = FrameIndex >= 30 && PreviousAverageMs > 0.0;
	const bool bSpike = bHasBaseline && (FrameMs > 33.3 || FrameMs > PreviousAverageMs * 2.0);

	if (bSpike)
	{
		struct FNamedValue
		{
			const char* Name = nullptr;
			double Value = 0.0;
		};

		struct FNamedCounter
		{
			const char* Name = nullptr;
			uint64 Value = 0;
		};

		TArray<FNamedValue> Sections;
		Sections.reserve(SectionTimesMs.size());
		for (const auto& [Name, TimeMs] : SectionTimesMs)
		{
			if (TimeMs > 0.01)
			{
				Sections.push_back({Name, TimeMs});
			}
		}

		std::sort(
			Sections.begin(),
			Sections.end(),
			[](const FNamedValue& Lhs, const FNamedValue& Rhs)
			{
				return Lhs.Value > Rhs.Value;
			});

		TArray<FNamedCounter> Counters;
		Counters.reserve(CounterValues.size());
		for (const auto& [Name, Value] : CounterValues)
		{
			if (Value > 0)
			{
				Counters.push_back({Name, Value});
			}
		}

		std::sort(
			Counters.begin(),
			Counters.end(),
			[](const FNamedCounter& Lhs, const FNamedCounter& Rhs)
			{
				return Lhs.Value > Rhs.Value;
			});

		char Header[256] = {};
		std::snprintf(
			Header,
			sizeof(Header),
			"[FrameSpike] frame=%.3f ms avg=%.3f ms sections=%zu counters=%zu",
			FrameMs,
			PreviousAverageMs,
			Sections.size(),
			Counters.size());
		EmitSpikeLine(Header);

		if (!Sections.empty())
		{
			std::string SectionLine = "[FrameSpike] section timings:";
			const size_t MaxSectionCount = (std::min)(Sections.size(), static_cast<size_t>(8));
			for (size_t Index = 0; Index < MaxSectionCount; ++Index)
			{
				char Entry[160] = {};
				std::snprintf(Entry, sizeof(Entry), " %s=%.3fms", Sections[Index].Name, Sections[Index].Value);
				SectionLine += Entry;
			}
			EmitSpikeLine(SectionLine);
		}

		if (!Counters.empty())
		{
			std::string CounterLine = "[FrameSpike] counters:";
			const size_t MaxCounterCount = (std::min)(Counters.size(), static_cast<size_t>(8));
			for (size_t Index = 0; Index < MaxCounterCount; ++Index)
			{
				char Entry[160] = {};
				std::snprintf(Entry, sizeof(Entry), " %s=%llu", Counters[Index].Name, Counters[Index].Value);
				CounterLine += Entry;
			}
			EmitSpikeLine(CounterLine);
		}
	}

	if (RollingAverageMs <= 0.0)
	{
		RollingAverageMs = FrameMs;
	}
	else
	{
		RollingAverageMs = RollingAverageMs * 0.9 + FrameMs * 0.1;
	}

	++FrameIndex;
}

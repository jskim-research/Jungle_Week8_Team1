#include "Core/Logging/GPUProfiler.h"

#include <algorithm>

void FGPUProfiler::Initialize(ID3D11Device* InDevice, ID3D11DeviceContext* InContext)
{
	Device = InDevice;
	Context = InContext;
	if (!Device || !Context)
	{
		bInitialized = false;
		return;
	}

	D3D11_QUERY_DESC disjointDesc = {};
	disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;

	D3D11_QUERY_DESC timestampDesc = {};
	timestampDesc.Query = D3D11_QUERY_TIMESTAMP;

	for (uint32 f = 0; f < FRAME_COUNT; ++f)
	{
		Device->CreateQuery(&disjointDesc, Frames[f].DisjointQuery.ReleaseAndGetAddressOf());
		Frames[f].UsedCount = 0;
		Frames[f].bSubmitted = false;

		for (uint32 i = 0; i < MAX_TIMESTAMPS; ++i)
		{
			Device->CreateQuery(&timestampDesc, Frames[f].Timestamps[i].BeginQuery.ReleaseAndGetAddressOf());
			Device->CreateQuery(&timestampDesc, Frames[f].Timestamps[i].EndQuery.ReleaseAndGetAddressOf());
			Frames[f].Timestamps[i].Name = nullptr;
		}
	}

	WriteIndex = 0;
	bInitialized = true;
}

void FGPUProfiler::Shutdown()
{
	if (!bInitialized) return;

	for (uint32 f = 0; f < FRAME_COUNT; ++f)
	{
		Frames[f].DisjointQuery.Reset();
		Frames[f].UsedCount = 0;
		Frames[f].bSubmitted = false;

		for (uint32 i = 0; i < MAX_TIMESTAMPS; ++i)
		{
			Frames[f].Timestamps[i].BeginQuery.Reset();
			Frames[f].Timestamps[i].EndQuery.Reset();
			Frames[f].Timestamps[i].Name = nullptr;
		}
	}

	Device.Reset();
	Context.Reset();
	bInitialized = false;
}

void FGPUProfiler::BeginFrame()
{
	if (!bInitialized) return;

	CollectReadyFrames();

	FFrameData* WriteFrame = nullptr;
	for (uint32 Attempt = 0; Attempt < FRAME_COUNT; ++Attempt)
	{
		const uint32 CandidateIndex = (WriteIndex + Attempt) % FRAME_COUNT;
		if (!Frames[CandidateIndex].bSubmitted)
		{
			WriteIndex = CandidateIndex;
			WriteFrame = &Frames[CandidateIndex];
			break;
		}
	}

	if (WriteFrame == nullptr)
	{
		bSkipFrame = true;
		return;
	}

	bSkipFrame = false;
	WriteFrame->UsedCount = 0;
	for (uint32 i = 0; i < MAX_TIMESTAMPS; ++i)
	{
		WriteFrame->Timestamps[i].Name = nullptr;
	}

	Context->Begin(WriteFrame->DisjointQuery.Get());
}

void FGPUProfiler::EndFrame()
{
	if (!bInitialized || bSkipFrame) return;

	Context->End(Frames[WriteIndex].DisjointQuery.Get());
	Frames[WriteIndex].bSubmitted = true;
	WriteIndex = (WriteIndex + 1u) % FRAME_COUNT;
}

uint32 FGPUProfiler::BeginTimestamp(const char* Name)
{
	if (!bInitialized || bSkipFrame) return UINT32_MAX;

	FFrameData& Write = Frames[WriteIndex];
	if (Write.UsedCount >= MAX_TIMESTAMPS) return UINT32_MAX;

	const uint32 Index = Write.UsedCount++;
	Write.Timestamps[Index].Name = Name;
	Context->End(Write.Timestamps[Index].BeginQuery.Get());
	return Index;
}

void FGPUProfiler::EndTimestamp(uint32 Index)
{
	if (!bInitialized || bSkipFrame || Index == UINT32_MAX) return;

	FFrameData& Write = Frames[WriteIndex];
	if (Index >= Write.UsedCount) return;

	Context->End(Write.Timestamps[Index].EndQuery.Get());
}

void FGPUProfiler::CollectReadyFrames()
{
	for (uint32 FrameIndex = 0; FrameIndex < FRAME_COUNT; ++FrameIndex)
	{
		if (!Frames[FrameIndex].bSubmitted)
		{
			continue;
		}

		TryCollectFrame(Frames[FrameIndex]);
	}
}

bool FGPUProfiler::TryCollectFrame(FFrameData& Frame)
{
	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT DisjointData = {};
	const HRESULT DisjointResult = Context->GetData(
		Frame.DisjointQuery.Get(),
		&DisjointData,
		sizeof(DisjointData),
		D3D11_ASYNC_GETDATA_DONOTFLUSH);
	if (DisjointResult != S_OK)
	{
		return false;
	}

	if (DisjointData.Disjoint || Frame.UsedCount == 0)
	{
		Frame.bSubmitted = false;
		Frame.UsedCount = 0;
		return true;
	}

	const double InvFrequency = 1000.0 / static_cast<double>(DisjointData.Frequency);

	for (uint32 TimestampIndex = 0; TimestampIndex < Frame.UsedCount; ++TimestampIndex)
	{
		UINT64 TimestampBegin = 0;
		UINT64 TimestampEnd = 0;

		if (Context->GetData(
				Frame.Timestamps[TimestampIndex].BeginQuery.Get(),
				&TimestampBegin,
				sizeof(UINT64),
				D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
		{
			return false;
		}

		if (Context->GetData(
				Frame.Timestamps[TimestampIndex].EndQuery.Get(),
				&TimestampEnd,
				sizeof(UINT64),
				D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
		{
			return false;
		}

		const double ElapsedMs = static_cast<double>(TimestampEnd - TimestampBegin) * InvFrequency;
		const double ElapsedSec = ElapsedMs * 0.001;
		const char* Name = Frame.Timestamps[TimestampIndex].Name;
		auto It = GPUStats.find(Name);
		if (It == GPUStats.end())
		{
			FStatEntry Entry;
			Entry.Name = Name;
			Entry.CallCount = 1;
			Entry.TotalTime = ElapsedSec;
			Entry.MaxTime = ElapsedSec;
			Entry.MinTime = ElapsedSec;
			Entry.LastTime = ElapsedSec;
			GPUStats[Name] = Entry;
		}
		else
		{
			FStatEntry& Entry = It->second;
			Entry.CallCount++;
			Entry.TotalTime += ElapsedSec;
			Entry.MaxTime = (std::max)(Entry.MaxTime, ElapsedSec);
			Entry.MinTime = (std::min)(Entry.MinTime, ElapsedSec);
			Entry.LastTime = ElapsedSec;
		}
	}

	Frame.bSubmitted = false;
	Frame.UsedCount = 0;
	return true;
}

void FGPUProfiler::TakeSnapshot()
{
	Snapshot.clear();
	Snapshot.reserve(GPUStats.size());

	for (auto& [Key, Entry] : GPUStats)
	{
		Snapshot.push_back(Entry);

		Entry.CallCount = 0;
		Entry.TotalTime = 0.0;
		Entry.MaxTime = 0.0;
		Entry.MinTime = DBL_MAX;
		Entry.LastTime = 0.0;
	}
}

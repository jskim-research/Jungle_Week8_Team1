#include "EngineStatics.h"

uint32 EngineStatics::NextUUID = 1;

uint32 EngineStatics::TotalAllocationBytes = 0;
uint32 EngineStatics::TotalAllocationCount = 0;

void* operator new(SIZE_T Size)
{
	void* Ptr = std::malloc(Size);
	if (Ptr)
	{
		EngineStatics::OnAllocated(static_cast<uint32>(Size));
	}
	return Ptr;
}

void operator delete(void* Ptr, SIZE_T Size)
{
	if (Ptr)
	{
		EngineStatics::OnDeallocated(static_cast<uint32>(Size));
		std::free(Ptr);
	}
}

#pragma once

#include "Core/Containers/Array.h"
#include "Core/Containers/Map.h"
#include "Core/CoreTypes.h"
#include "Render/Common/ShadowTypes.h"
#include "ShadowResource.h"

#include <memory>

struct ID3D11Device;

struct FShadowRequestDesc
{
	uint32 Resolution = 1024u;
	uint32 CascadeCount = 1u; // CSM only
	uint32 CubeCount = 1u;

	// 동일한 설정의 Pool 에서 가져오기 위한 정보들
	EShadowMapType MapType;
	EShadowAllocationMode AllocationMode;
};

class IShadowResourcePool
{
public:
	virtual ~IShadowResourcePool() = default;

	virtual FShadowResource* Acquire(ID3D11Device* Device, const FShadowRequestDesc& Desc) = 0;
	virtual void Release(FShadowResource* Resource) = 0;
	virtual void BeginFrame() = 0;
};

class FShadowResourcePool : public IShadowResourcePool
{
public:
	FShadowResource* Acquire(ID3D11Device* Device, const FShadowRequestDesc& Desc) override;
	void Release(FShadowResource* Resource) override;
	void BeginFrame() override;

private:
	struct FPooledShadowResourceEntry
	{
		std::unique_ptr<FShadowResource> Resource;
		FShadowRequestDesc Desc = {};
		bool bInUse = false;
	};

	TArray<FPooledShadowResourceEntry> ResourcePool;
	TMap<FShadowResource*, uint32> ResourceLookup;
};

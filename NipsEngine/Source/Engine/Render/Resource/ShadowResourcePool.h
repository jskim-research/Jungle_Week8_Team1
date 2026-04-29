#pragma once
#include "Core/CoreTypes.h"
struct ID3D11Device;
#include "ShadowResource.h"
#include "Render/Common/ShadowTypes.h"

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

	// 요청 기반 리소스 확보
	virtual FShadowResource* Acquire(ID3D11Device* Device, const FShadowRequestDesc& Desc) = 0;

	// 사용 종료 (옵션 or 프레임 기반이면 생략 가능)
	virtual void Release(FShadowResource* Resource) = 0;

	// 프레임 시작 초기화 (bInUse 리셋)
	virtual void BeginFrame() = 0;
};

class FShadowResourcePool : public IShadowResourcePool
{
public:
	FShadowResource* Acquire(ID3D11Device* Device, const FShadowRequestDesc& Desc) override;
	void Release(FShadowResource* Resource) override;
	void BeginFrame() override;
};

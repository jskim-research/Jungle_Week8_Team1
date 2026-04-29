#include "RenderPipeline.h"

#include "Core/Logging/GPUProfiler.h"
#include "Core/Logging/Stats.h"
#include "LightCullingPass.h"
#include "SkyRenderPass.h"
#include "OpaqueRenderPass.h"
#include "DecalRenderPass.h"
#include "BufferVisualizationRenderPass.h"
#include "FogRenderPass.h"
#include "FXAARenderPass.h"
#include "FontRenderPass.h"
#include "SubUVRenderPass.h"
#include "BillboardRenderPass.h"
#include "TranslucentRenderPass.h"
#include "SelectionMaskRenderPass.h"
#include "GridRenderPass.h"
#include "EditorRenderPass.h"
#include "DepthLessRenderPass.h"
#include "DepthPrepassRenderPass.h"
#include "PostProcessOutlineRenderPass.h"
#include "ToonOutlineRenderPass.h"
#include "ShadowPass.h"
#include "HitMapRenderPass.h"

bool FRenderPipeline::Initialize()
{
	LightCullingPass = std::make_shared<FLightCullingPass>();
	LightCullingPass->Initialize();

	HitMapRenderPass = std::make_shared<FHitMapRenderPass>();
	HitMapRenderPass->Initialize();

	SkyRenderPass = std::make_shared<FSkyRenderPass>();
	SkyRenderPass->Initialize();

	OpaqueRenderPass = std::make_shared<FOpaqueRenderPass>();
	OpaqueRenderPass->Initialize();

	DecalRenderPass = std::make_shared<FDecalRenderPass>();
	DecalRenderPass->Initialize();

	BufferVisualizationRenderPass = std::make_shared<FBufferVisualizationRenderPass>();
	BufferVisualizationRenderPass->Initialize();

	FogRenderPass = std::make_shared<FFogRenderPass>();
	FogRenderPass->Initialize();

	FXAARenderPass = std::make_shared<FFXAARenderPass>();
	FXAARenderPass->Initialize();

	FontRenderPass = std::make_shared<FFontRenderPass>();
	FontRenderPass->Initialize();

	SubUVRenderPass = std::make_shared<FSubUVRenderPass>();
	SubUVRenderPass->Initialize();

	BillboardRenderPass = std::make_shared<FBillboardRenderPass>();
	BillboardRenderPass->Initialize();

	TranslucentRenderPass = std::make_shared<FTranslucentRenderPass>();
	TranslucentRenderPass->Initialize();

	SelectionMaskRenderPass = std::make_shared<FSelectionMaskRenderPass>();
	SelectionMaskRenderPass->Initialize();

	GridRenderPass = std::make_shared<FGridRenderPass>();
	GridRenderPass->Initialize();

	EditorRenderPass = std::make_shared<FEditorRenderPass>();
	EditorRenderPass->Initialize();

	DepthLessRenderPass = std::make_shared<FDepthLessRenderPass>();
	DepthLessRenderPass->Initialize();

	DepthPrepassRenderPass = std::make_shared<FDepthPrepassRenderPass>();
	DepthPrepassRenderPass->Initialize();

	PostProcessOutlineRenderPass = std::make_shared<FPostProcessOutlineRenderPass>();
	PostProcessOutlineRenderPass->Initialize();

	ToonOutlineRenderPass = std::make_shared<FToonOutlineRenderPass>();
	ToonOutlineRenderPass->Initialize();

	ShadowPass = std::make_shared<FShadowPass>();
	ShadowPass->Initialize();

	FogRenderPass->SetSkipWireframe(true);
	FXAARenderPass->SetSkipWireframe(true);

	RenderPasses.push_back(ShadowPass);
	RenderPasses.push_back(DepthPrepassRenderPass);
	RenderPasses.push_back(LightCullingPass);
	RenderPasses.push_back(SkyRenderPass);
	RenderPasses.push_back(ToonOutlineRenderPass);
	RenderPasses.push_back(OpaqueRenderPass);
	RenderPasses.push_back(DecalRenderPass);
	RenderPasses.push_back(BufferVisualizationRenderPass);
	RenderPasses.push_back(HitMapRenderPass);
	RenderPasses.push_back(FogRenderPass);
	RenderPasses.push_back(FXAARenderPass);
	RenderPasses.push_back(FontRenderPass);
	RenderPasses.push_back(SubUVRenderPass);
	RenderPasses.push_back(BillboardRenderPass);
	RenderPasses.push_back(TranslucentRenderPass);
	RenderPasses.push_back(SelectionMaskRenderPass);
	RenderPasses.push_back(GridRenderPass);
	RenderPasses.push_back(EditorRenderPass);
	RenderPasses.push_back(DepthLessRenderPass);
	RenderPasses.push_back(PostProcessOutlineRenderPass);

	return true;
}

bool FRenderPipeline::Render(const FRenderPassContext* Context)
{
	OutSRV = nullptr;
	OutRTV = nullptr;

	auto ResolvePassName = [this](const FBaseRenderPass* Pass) -> const char*
	{
		if (Pass == ShadowPass.get()) return "Shadow pass";
		if (Pass == DepthPrepassRenderPass.get()) return "Depth prepass";
		if (Pass == LightCullingPass.get()) return "Light culling compute pass";
		if (Pass == OpaqueRenderPass.get()) return "Main opaque pass";
		if (Pass == BufferVisualizationRenderPass.get()) return "Debug rendering";
		if (Pass == HitMapRenderPass.get()) return "Debug rendering";
		if (Pass == GridRenderPass.get()) return "Debug rendering";
		if (Pass == EditorRenderPass.get()) return "Debug rendering";
		if (Pass == PostProcessOutlineRenderPass.get()) return "Debug rendering";
		if (Pass == SkyRenderPass.get()) return "Sky pass";
		if (Pass == ToonOutlineRenderPass.get()) return "Toon outline pass";
		if (Pass == DecalRenderPass.get()) return "Decal pass";
		if (Pass == FogRenderPass.get()) return "Fog pass";
		if (Pass == FXAARenderPass.get()) return "FXAA pass";
		if (Pass == FontRenderPass.get()) return "Font pass";
		if (Pass == SubUVRenderPass.get()) return "SubUV pass";
		if (Pass == BillboardRenderPass.get()) return "Billboard pass";
		if (Pass == TranslucentRenderPass.get()) return "Translucent pass";
		if (Pass == SelectionMaskRenderPass.get()) return "Selection mask pass";
		if (Pass == DepthLessRenderPass.get()) return "Depth-less pass";
		return "Render pass";
	};

	for (const std::shared_ptr<FBaseRenderPass>& Pass : RenderPasses)
	{
		Pass->SetPrevPassSRV(OutSRV);
		Pass->SetPrevPassRTV(OutRTV);

		const char* PassName = ResolvePassName(Pass.get());
		{
			FRAME_SPIKE_SCOPE(PassName);
			GPU_SCOPE_STAT(PassName);
			Pass->Render(Context);
		}

		OutSRV = Pass->GetOutSRV();
		OutRTV = Pass->GetOutRTV();
	}

	Context->RenderTargets->FinalSRV = OutSRV;
	Context->RenderTargets->FinalRTV = OutRTV;

	if (ShadowPass->GetShadowMaps().empty())
	{
		Context->RenderTargets->ShadowMap = nullptr;
	}
	else
	{
		Context->RenderTargets->ShadowMap = &ShadowPass->GetShadowMaps()[0];
	}

	return true;
}

void FRenderPipeline::Release()
{
	if (LightCullingPass)
	{
		LightCullingPass->Release();
		LightCullingPass.reset();
	}

	if (HitMapRenderPass)
	{
		HitMapRenderPass->Release();
		HitMapRenderPass.reset();
	}

	if (SkyRenderPass)
	{
		SkyRenderPass->Release();
		SkyRenderPass.reset();
	}

	if (OpaqueRenderPass)
	{
		OpaqueRenderPass->Release();
		OpaqueRenderPass.reset();
	}

	if (DecalRenderPass)
	{
		DecalRenderPass->Release();
		DecalRenderPass.reset();
	}

	if (ToonOutlineRenderPass)
	{
		ToonOutlineRenderPass->Release();
		ToonOutlineRenderPass.reset();
	}

	if (BufferVisualizationRenderPass)
	{
		BufferVisualizationRenderPass->Release();
		BufferVisualizationRenderPass.reset();
	}

	if (FogRenderPass)
	{
		FogRenderPass->Release();
		FogRenderPass.reset();
	}

	if (FXAARenderPass)
	{
		FXAARenderPass->Release();
		FXAARenderPass.reset();
	}

	if (FontRenderPass)
	{
		FontRenderPass->Release();
		FontRenderPass.reset();
	}

	if (SubUVRenderPass)
	{
		SubUVRenderPass->Release();
		SubUVRenderPass.reset();
	}

	if (BillboardRenderPass)
	{
		BillboardRenderPass->Release();
		BillboardRenderPass.reset();
	}

	if (TranslucentRenderPass)
	{
		TranslucentRenderPass->Release();
		TranslucentRenderPass.reset();
	}

	if (SelectionMaskRenderPass)
	{
		SelectionMaskRenderPass->Release();
		SelectionMaskRenderPass.reset();
	}

	if (GridRenderPass)
	{
		GridRenderPass->Release();
		GridRenderPass.reset();
	}

	if (EditorRenderPass)
	{
		EditorRenderPass->Release();
		EditorRenderPass.reset();
	}

	if (DepthLessRenderPass)
	{
		DepthLessRenderPass->Release();
		DepthLessRenderPass.reset();
	}

	if (DepthPrepassRenderPass)
	{
		DepthPrepassRenderPass->Release();
		DepthPrepassRenderPass.reset();
	}

	if (PostProcessOutlineRenderPass)
	{
		PostProcessOutlineRenderPass->Release();
		PostProcessOutlineRenderPass.reset();
	}

	if (ShadowPass)
	{
		ShadowPass->Release();
		ShadowPass.reset();
	}
}

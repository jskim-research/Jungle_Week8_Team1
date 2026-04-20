#include "Renderer.h"

#include <iostream>
#include <algorithm>
#include "Core/Paths.h"
#include "Core/ResourceManager.h"
#include "Render/Common/RenderTypes.h"
#include "Render/Mesh/MeshManager.h"
#include "Core/Logging/Stats.h"
#include "Core/Logging/GPUProfiler.h"
#include "Editor/UI/EditorConsoleWidget.h"
#include "Editor/Viewport/FSceneViewport.h"
#include "Render/Renderer/RenderTarget/RenderTargetFactory.h"
#include "Render/Renderer/RenderTarget/DepthStencilFactory.h"

void FRenderer::Create(HWND hWindow)
{
	Device.Create(hWindow);

	if (Device.GetDevice() == nullptr)
	{
		std::cout << "Failed to create D3D Device." << std::endl;
	}

	FResourceManager::Get().SetCachedDevice(Device.GetDevice());
	FResourceManager::Get().LoadShader("Shaders/Primitive.hlsl", "VS", "PS", PrimitiveInputLayout, ARRAYSIZE(PrimitiveInputLayout), nullptr);
    FResourceManager::Get().LoadShader("Shaders/ShaderSubUV.hlsl", "VS", "PS", FontBatcherInputLayout, ARRAYSIZE(FontBatcherInputLayout), nullptr);
    FResourceManager::Get().LoadShader("Shaders/Gizmo.hlsl", "VS", "PS", PrimitiveInputLayout, ARRAYSIZE(PrimitiveInputLayout), nullptr);
    FResourceManager::Get().LoadShader("Shaders/Editor.hlsl", "VS", "PS", PrimitiveInputLayout, ARRAYSIZE(PrimitiveInputLayout), nullptr);
    FResourceManager::Get().LoadShader("Shaders/SelectionMask.hlsl", "VS", "PS", PrimitiveInputLayout, ARRAYSIZE(PrimitiveInputLayout), nullptr);
    FResourceManager::Get().LoadShader("Shaders/OutlinePostProcess.hlsl", "VS", "PS", nullptr, 0, nullptr);
    FResourceManager::Get().LoadShader("Shaders/UberLit.hlsl", "mainVS", "mainPS", NormalVertexInputLayout, ARRAYSIZE(NormalVertexInputLayout), nullptr);
    FResourceManager::Get().LoadShader(MakeUberLitShaderCompileKey(ELightingModel::Gouraud), NormalVertexInputLayout, ARRAYSIZE(NormalVertexInputLayout));
    FResourceManager::Get().LoadShader(MakeUberLitShaderCompileKey(ELightingModel::Lambert), NormalVertexInputLayout, ARRAYSIZE(NormalVertexInputLayout));
    FResourceManager::Get().LoadShader(MakeUberLitShaderCompileKey(ELightingModel::Phong), NormalVertexInputLayout, ARRAYSIZE(NormalVertexInputLayout));
    FResourceManager::Get().LoadShader("Shaders/UberUnlit.hlsl", "mainVS", "mainPS", NormalVertexInputLayout, ARRAYSIZE(NormalVertexInputLayout), nullptr);
    FResourceManager::Get().LoadShader("Shaders/Multipass/LightPass.hlsl", "mainVS", "mainPS", nullptr, 0, nullptr);
    FResourceManager::Get().LoadShader("Shaders/Multipass/BufferVisualizationPass.hlsl", "mainVS", "mainPS", nullptr, 0, nullptr);
    FResourceManager::Get().LoadShader("Shaders/ShaderDecal.hlsl", "mainVS", "mainPS", NormalVertexInputLayout, ARRAYSIZE(NormalVertexInputLayout), nullptr);
    FResourceManager::Get().LoadShader("Shaders/Multipass/FogPass.hlsl", "mainVS", "mainPS", nullptr, 0, nullptr);
    FResourceManager::Get().LoadShader("Shaders/Multipass/FXAAPass.hlsl", "mainVS", "mainPS", nullptr, 0, nullptr);
    FResourceManager::Get().LoadShader("Shaders/ShaderFont.hlsl", "VS", "PS", FontBatcherInputLayout, ARRAYSIZE(FontBatcherInputLayout), nullptr);
    FResourceManager::Get().LoadShader("Shaders/ShaderLine.hlsl", "mainVS", "mainPS", PrimitiveInputLayout, ARRAYSIZE(PrimitiveInputLayout), nullptr);
}

void FRenderer::CreateResources()
{
	//	MeshManager init
	FMeshManager::Initialize();

	EditorLineBatcher.Create(Device.GetDevice());
	GridLineBatcher.Create(Device.GetDevice());

	// 텍스처는 ResourceManager가 소유 — Batcher 는 셰이더/버퍼만 초기화
	FontBatcher.Create(Device.GetDevice());
	SubUVBatcher.Create(Device.GetDevice());
	SceneLightBuffer.Create(Device.GetDevice(), sizeof(FGPULight), MaxSceneLightCount);
	SceneLightUploadScratch.reserve(MaxSceneLightCount);

	InitializePassBatchers();
	UseBackBufferRenderTargets();

	// GPU Profiler 초기화
	FGPUProfiler::Get().Initialize(Device.GetDevice(), Device.GetDeviceContext());

	RenderPipeline.Initialize();
    RenderPassContext = std::make_shared<FRenderPassContext>();
}

void FRenderer::Release()
{
	InvalidateSceneFinalTargets();

	RenderPipeline.Release();
    RenderPassContext.reset();

	FGPUProfiler::Get().Shutdown();

	EditorLineBatcher.Release();
	GridLineBatcher.Release();
	FontBatcher.Release();
	SubUVBatcher.Release();
	SceneLightBuffer.Release();
	SceneLightUploadScratch.clear();

    // Device::ReportLiveObjects 이전에 ResourceManager가 잡고 있던 D3D 객체를 먼저 해제한다.
    FResourceManager::Get().ReleaseGPUResources();

	Device.Release();
}

//	Bus → Batcher 데이터 수집 (CPU). BeginFrame 이전에 호출.
void FRenderer::PrepareBatchers(const FRenderBus& InRenderBus)
{
	for (uint32 i = 0; i < (uint32)ERenderPass::MAX; ++i)
	{
		if (!PassBatchers[i]) continue;

		const auto& Commands = InRenderBus.GetCommands(static_cast<ERenderPass>(i));
		const auto& AlignedCommands = GetAlignedCommands(static_cast<ERenderPass>(i), Commands);

		PassBatchers[i].Clear();
		for (const auto& Cmd : AlignedCommands)
			PassBatchers[i].Collect(Cmd, InRenderBus);
	}
}

const TArray<FRenderCommand>& FRenderer::GetAlignedCommands(ERenderPass Pass, const TArray<FRenderCommand>& Commands)
{
	// SubUV 패스: Particle(SRV) 포인터 기준 정렬 → 같은 텍스쳐끼리 연속 배치
	if (Pass == ERenderPass::SubUV && Commands.size() > 1)
	{
		SortedCommandBuffer.assign(Commands.begin(), Commands.end());

		std::sort(SortedCommandBuffer.begin(), SortedCommandBuffer.end(),
			[](const FRenderCommand& A, const FRenderCommand& B) {
				return A.Constants.SubUV.Particle < B.Constants.SubUV.Particle;
			});

		return SortedCommandBuffer;
	}

	return Commands;
}

//	GPU 프레임 시작. 반드시 Render 이전에 호출되어야 함.
void FRenderer::BeginFrame()
{
	Device.BeginFrame();
	UseBackBufferRenderTargets();

#if STATS
	FGPUProfiler::Get().BeginFrame();
#endif
}

void FRenderer::BeginViewportFrame(FRenderTargetSet* InRenderTargetSet)
{
    Device.BeginViewportFrame(InRenderTargetSet);
    UseViewportRenderTargets(InRenderTargetSet);

#if STATS
    FGPUProfiler::Get().BeginFrame();
#endif
}

void FRenderer::UseBackBufferRenderTargets()
{
	CurrentRenderTargets = Device.GetBackBufferRenderTargets();

	if (CurrentRenderTargets && CurrentRenderTargets->IsValid())
	{
        ID3D11RenderTargetView* RTV =
                CurrentRenderTargets->SceneColorRTV; // Back Buffer 의 경우 SceneColorRTV 가 FinalRTV 역할
        SceneFinalRTV = RTV;
        
		Device.GetDeviceContext()->OMSetRenderTargets(1, &RTV, CurrentRenderTargets->DepthStencilView);
		Device.SetSubViewport(0, 0,
			static_cast<int32>(CurrentRenderTargets->Width),
			static_cast<int32>(CurrentRenderTargets->Height));
	}
}

void FRenderer::UseViewportRenderTargets(FRenderTargetSet* InRenderTargetSet)
{
    CurrentRenderTargets = InRenderTargetSet;

    if (CurrentRenderTargets == nullptr || !CurrentRenderTargets->IsValid())
    {
        InvalidateSceneFinalTargets();
		// Back Buffer 아마 쓰이면 안될텐데 여기 중단점 찍히면 확인
		// 기존 단일 Viewport 구조에서 쓰이던 내용
        UseBackBufferRenderTargets();
        return;
    }

    Device.SetSubViewport(0, 0,
                          static_cast<int32>(CurrentRenderTargets->Width),
                          static_cast<int32>(CurrentRenderTargets->Height));
}

void FRenderer::InvalidateSceneFinalTargets()
{
	SceneFinalRTV.Reset();
	SceneFinalSRV.Reset();
	CurrentRenderTargets = nullptr;
}

void FRenderer::UpdateSceneLightBuffer(const FRenderBus& InRenderBus)
{
	const TArray<FRenderLight>& SceneLights = InRenderBus.GetLights();
	uint32 UploadCount = static_cast<uint32>(SceneLights.size());
	if (UploadCount > MaxSceneLightCount)
	{
		UE_LOG("[Renderer] Scene light count exceeded the %u light cap. Clamping %u lights to %u.",
			MaxSceneLightCount, UploadCount, MaxSceneLightCount);
		UploadCount = MaxSceneLightCount;
	}

	SceneLightUploadScratch.clear();
	if (UploadCount > 0)
	{
		SceneLightUploadScratch.assign(SceneLights.begin(), SceneLights.begin() + UploadCount);
	}

	SceneLightBuffer.Update(
		Device.GetDeviceContext(),
		UploadCount > 0 ? SceneLightUploadScratch.data() : nullptr,
		UploadCount);

	RenderPassContext->SceneLightBufferSRV = SceneLightBuffer.GetSRV();
	RenderPassContext->SceneLightCount = UploadCount;
}

//	RenderBus에 담긴 모든 RenderCommand에 대해서 Draw Call 수행 (GPU)
void FRenderer::Render(const FRenderBus& InRenderBus)
{
	/** Opaque 만 테스트 */
    
	RenderPassContext->Device = Device.GetDevice();
    RenderPassContext->DeviceContext = Device.GetDeviceContext();
    RenderPassContext->RenderBus = &InRenderBus;
    RenderPassContext->RenderTargets = CurrentRenderTargets;
    RenderPassContext->RenderResources = &Resources;
    RenderPassContext->FontBatcher = &FontBatcher;
    RenderPassContext->SubUVBatcher = &SubUVBatcher;
    RenderPassContext->GridLineBatcher = &GridLineBatcher;
    RenderPassContext->EditorLineBatcher = &EditorLineBatcher;
	UpdateSceneLightBuffer(InRenderBus);
	RenderPipeline.Render(RenderPassContext.get());
	
	SceneFinalSRV = RenderPipeline.GetOutSRV();
}

FViewportRenderResource& FRenderer::AcquireViewportResource(FSceneViewport* VP, uint32 Width, uint32 Height, int32 Index)
{
    assert(Index < 4 && "Index Out of Bound");

    FViewportRenderResource& Res = ViewportResources[Index];

    if (Device.GetDevice() == nullptr || Width == 0 || Height == 0)
    {
        ReleaseViewportResource(VP, Index);
        return Res;
    }

    const bool bSameSize =
        (Res.Width == Width) &&
        (Res.Height == Height);

    const bool bResourcesValid =
        (Res.ColorRTV != nullptr) &&
        (Res.SelectionMaskRTV != nullptr) &&
        (Res.DepthStencilView != nullptr);

    if (bSameSize && bResourcesValid)
    {
        return Res;
    }

    // 재생성
    ReleaseViewportResource(VP, Index);
    InitializeViewportResource(VP, Width, Height, Index);

    return Res;
}

void FRenderer::InitializeViewportResource(FSceneViewport* VP, uint32 Width, uint32 Height, int32 Index)
{
    FViewportRenderResource& Res = ViewportResources[Index];

    FRenderTarget RT;

    Res.Width = Width;
    Res.Height = Height;

    RT = FRenderTargetFactory::CreateSceneColor(Device.GetDevice(), Width, Height);
    Res.ColorTex = RT.Texture;
    Res.ColorRTV = RT.RTV;
    Res.ColorSRV = RT.SRV;

    RT = FRenderTargetFactory::CreateSceneNormal(Device.GetDevice(), Width, Height);
    Res.NormalTex = RT.Texture;
    Res.NormalRTV = RT.RTV;
    Res.NormalSRV = RT.SRV;

    RT = FRenderTargetFactory::CreateSelectionMask(Device.GetDevice(), Width, Height);
    Res.SelectionMaskTex = RT.Texture;
    Res.SelectionMaskRTV = RT.RTV;
    Res.SelectionMaskSRV = RT.SRV;

    RT = FRenderTargetFactory::CreateSceneLight(Device.GetDevice(), Width, Height);
    Res.LightTex = RT.Texture;
    Res.LightRTV = RT.RTV;
    Res.LightSRV = RT.SRV;

    RT = FRenderTargetFactory::CreateSceneFog(Device.GetDevice(), Width, Height);
    Res.FogTex = RT.Texture;
    Res.FogRTV = RT.RTV;
    Res.FogSRV = RT.SRV;

    RT = FRenderTargetFactory::CreateSceneWorldPos(Device.GetDevice(), Width, Height);
    Res.WorldPosTex = RT.Texture;
    Res.WorldPosRTV = RT.RTV;
    Res.WorldPosSRV = RT.SRV;

    RT = FRenderTargetFactory::CreateSceneFXAA(Device.GetDevice(), Width, Height);
    Res.FXAATex = RT.Texture;
    Res.FXAARTV = RT.RTV;
    Res.FXAASRV = RT.SRV;

    // Depth
    FDepthStencilResource DSR =
        FDepthStencilFactory::CreateDepthStencilView(Device.GetDevice(), Width, Height);

    Res.DepthTex = DSR.Texture;
    Res.DepthStencilView = DSR.DSV;
    Res.DepthStencilSRV = DSR.SRV;
}

void FRenderer::ReleaseViewportResource(FSceneViewport* VP, int32 Index)
{
    assert(Index < 4 && "Index Out of Bound");

    FViewportRenderResource& Res = ViewportResources[Index];

    Res.SelectionMaskSRV.Reset();
    Res.SelectionMaskRTV.Reset();
    Res.SelectionMaskTex.Reset();

    Res.ColorSRV.Reset();
    Res.ColorRTV.Reset();
    Res.ColorTex.Reset();

    Res.NormalRTV.Reset();
    Res.NormalSRV.Reset();
    Res.NormalTex.Reset();

    Res.LightRTV.Reset();
    Res.LightSRV.Reset();
    Res.LightTex.Reset();

    Res.DepthStencilView.Reset();
    Res.DepthTex.Reset();
    Res.DepthStencilSRV.Reset();

    Res.FogTex.Reset();
    Res.FogRTV.Reset();
    Res.FogSRV.Reset();

    Res.WorldPosRTV.Reset();
    Res.WorldPosSRV.Reset();
    Res.WorldPosTex.Reset();

    Res.FXAARTV.Reset();
    Res.FXAASRV.Reset();
    Res.FXAATex.Reset();

    Res.Width = 0;
    Res.Height = 0;
    Res.RenderTargetSet = {};
}

// ============================================================
// Pass Batcher 바인딩 초기화
// ============================================================
void FRenderer::InitializePassBatchers()
{
	// --- Grid 패스: 월드 그리드 + 축 → GridLineBatcher ---
    PassBatchers[(uint32)ERenderPass::Grid] = {
        /*.Clear   =*/[this]()
        { GridLineBatcher.Clear(); },
        /*.Collect =*/[this](const FRenderCommand& Cmd, const FRenderBus& Bus)
        {
			if (Cmd.Type == ERenderCommandType::Grid)
			{
				const FVector CameraPos = Bus.GetView().GetInverse().GetOrigin();
				const FVector CameraFwd = Bus.GetCameraForward();

				GridLineBatcher.AddWorldHelpers(
					Bus.GetShowFlags(),
					Cmd.Constants.Grid.GridSpacing,
					Cmd.Constants.Grid.GridHalfLineCount,
					CameraPos, CameraFwd,
					Cmd.Constants.Grid.bOrthographic);
			} },
    };

	// --- Font 패스: 텍스트 → FontBatcher ---
	PassBatchers[(uint32)ERenderPass::Font] = {
		/*.Clear   =*/ [this]() { FontBatcher.Clear(); },
		/*.Collect =*/ [this](const FRenderCommand& Cmd, const FRenderBus&) {
			if (Cmd.Type == ERenderCommandType::Font && Cmd.Constants.Font.Text && !Cmd.Constants.Font.Text->empty())
			{
				FontBatcher.AddText(
					*Cmd.Constants.Font.Text,
					Cmd.PerObjectConstants.Model,
					Cmd.Constants.Font.Scale
				);
			}
		}
	};

	// --- SubUV 패스: 스프라이트 → SubUVBatcher ---
	PassBatchers[(uint32)ERenderPass::SubUV] = {
		/*.Clear   =*/ [this]() {
			SubUVBatcher.Clear();
		},
		/*.Collect =*/ [this](const FRenderCommand& Cmd, const FRenderBus& Bus) {
			if (Cmd.Type == ERenderCommandType::SubUV && Cmd.Constants.SubUV.Particle)
			{
				const auto& SubUV = Cmd.Constants.SubUV;
				SubUVBatcher.AddSprite(
					SubUV.Particle->Texture,
					Cmd.PerObjectConstants.Model.GetOrigin(),
					Bus.GetCameraRight(),
					Bus.GetCameraUp(),
					Cmd.PerObjectConstants.Model.GetScaleVector(),
					SubUV.FrameIndex,
					SubUV.Particle->Columns,
					SubUV.Particle->Rows,
					SubUV.Width,
					SubUV.Height
				);
			}
			// 기존 SubUV 분기 아래에
			else if (Cmd.Type == ERenderCommandType::Billboard && Cmd.Constants.Billboard.Texture)
			{
				SubUVBatcher.AddSprite(
					Cmd.Constants.Billboard.Texture,
					Cmd.PerObjectConstants.Model.GetOrigin(),
					Bus.GetCameraRight(),
					Bus.GetCameraUp(),
					Cmd.PerObjectConstants.Model.GetScaleVector(),
					0,   // FrameIndex 고정
					1,   // Columns 고정
					1,   // Rows 고정
					Cmd.Constants.Billboard.Width,
					Cmd.Constants.Billboard.Height
				);
			}
		}
	};
}

//	Present the rendered frame to the screen. 반드시 Render 이후에 호출되어야 함.
void FRenderer::EndFrame()
{
#if STATS
	FGPUProfiler::Get().EndFrame();
#endif
	Device.EndFrame();
}

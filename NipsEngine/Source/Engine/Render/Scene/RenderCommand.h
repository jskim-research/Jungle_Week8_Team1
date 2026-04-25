#pragma once

/*
	Constants Buffer에 사용될 구조체와 
	에 담길 RenderCommand 구조체를 정의하고 있습니다.
	RenderCommand는 Renderer에서 Draw Call을 1회 수행하기 위해 필요한 정보를 담고 있습니다.
*/

#include "Render/Common/RenderTypes.h"
#include "Render/Resource/Buffer.h"
#include "Render/Resource/Material.h"
#include "Render/Device/D3DDevice.h"
#include "Core/CoreMinimal.h"
#include "Core/ResourceTypes.h"

#include "Math/Matrix.h"
#include "Math/Vector.h"


struct ID3D11ShaderResourceView;

enum class ERenderCommandType
{
	Primitive,
	Gizmo,
	SelectionMask,
	BillboardSelectionMask,
	PostProcessOutline,
	Billboard,
	Grid,		// Grid 패스 — LineBatcher 경유
	Font,		// TextRenderComponent — FontBatcher 경유
	SubUV,		// SubUVComponent     — SubUVBatcher 경유
	StaticMesh,	// UStaticMeshComponent — OBJ 메시 퐁셰이딩
	Decal,
	Light,
	Sky,
	ToonOutline
};

//PerObject
struct FPerObjectConstants
{
	FMatrix Model;
	FMatrix WorldInvTrans;
	FVector4 Color;

	FPerObjectConstants() = default;
	FPerObjectConstants(const FMatrix& InModel, const FVector4& InColor = FVector4(1, 1, 1, 1))
		: Model(InModel), Color(InColor)
	{
		WorldInvTrans = InModel.GetInverse().GetTransposed();
	}
};

struct FFrameConstants
{
	FMatrix View;          
	FMatrix Projection;    
	FMatrix InverseViewProjection;
	FVector CameraPosition;
	float Padding0;
	float bIsWireframe = 0.0f;
	FVector WireframeColor;
};

struct FGizmoConstants
{
	FVector4 ColorTint;
	uint32 bIsInnerGizmo;	
	uint32 bClicking;
	uint32 SelectedAxis;
	float HoveredAxisOpacity;
};

struct FEditorConstants
{
	FVector CameraPosition; // xyz 사용, w padding
	uint32 Flag;
};

struct FOutlineConstants
{
	FVector4 OutlineColor = FVector4(1.0f, 0.5f, 0.0f, 1.0f); // RGBA
	float OutlineThicknessPixels = 2.0f;
	FVector2 ViewportSize = FVector2(1.0f, 1.0f);
	float Padding0 = 0.0f;
};

struct FGridConstants
{
	float GridSpacing;
	int32 GridHalfLineCount;
	bool  bOrthographic;
	float Padding0[1];
};

struct FFontConstants
{
	const FString* Text = nullptr;			// 컴포넌트 소유 문자열 참조 (프레임 내 유효)
	const FFontResource* Font = nullptr;
	float Scale = 1.0f;
};

struct FSubUVConstants
{
	const FParticleResource* Particle = nullptr;
	uint32 FrameIndex = 0;
	float Width  = 1.0f;
	float Height = 1.0f;
};
struct FBillboardConstants
{
	UTexture* Texture = nullptr;
};
// Static mesh material constants — UberLit/UberUnlit material contract
// 완전 Obj전용입니다. NormalMap 은 Uber 셰이더 계약에 포함되고 BumpMap 은 현재 보존만 합니다.
struct FStaticMeshConstants
{
	FVector BaseColor     = { 0.8f, 0.8f, 0.8f };
	float   Opacity       = 1.0f;

	FVector SpecularColor = { 0.5f, 0.5f, 0.5f };
	float   Shininess     = 32.0f;

	// ScrollUV
	float  ScrollX          = 0.f;
	float  ScrollY          = 0.f;
	uint32 bHasDiffuseMap   = 0;
	uint32 bHasSpecularMap  = 0;

	FVector EmissiveColor   = {0.0f, 0.0f, 0.0f};
	uint32 bHasNormalMap    = 0;

	// Texture SRV (CPU-only, cbuffer 범위 밖)
	//ID3D11ShaderResourceView* DiffuseSRV  = { nullptr };
	//ID3D11ShaderResourceView* SpecularSRV = { nullptr };
	//ID3D11ShaderResourceView* NormalSRV   = { nullptr };
};

struct FDecalConstants
{
	FMatrix InvDecalWorld = FMatrix::Identity;
};

constexpr uint32 MaxFogLayerCount = 32;

struct FFogConstants
{
	FVector4 FogColor;
    float    FogDensity;
    float    HeightFalloff;
    float        FogHeight;
    float        FogStartDistance;
    float        FogCutoffDistance;
    float        FogMaxOpacity;
    float        Padding[2];
};

struct FFogPassConstants
{
    uint32 FogCount = 0;
    float  Padding0[3] = {0.0f, 0.0f, 0.0f};
    FFogConstants Layers[MaxFogLayerCount] = {};
};

struct FFXAAConstants
{
    float InvResolution[2]; // (1/Width, 1/Height)
    uint32 bEnabled;       // 0: off, 1: on
    float  Padding;
};

struct FSkyConstants
{
	FMatrix InvView = FMatrix::Identity;
	FMatrix InvProjection = FMatrix::Identity;
	FVector4 SkyZenithColor = FVector4(0.11f, 0.27f, 0.58f, 1.0f);
	FVector4 SkyHorizonColor = FVector4(0.68f, 0.82f, 0.95f, 1.0f);
	FVector4 SunColor = FVector4(1.0f, 0.92f, 0.82f, 1.0f);
	FVector4 SunDirectionAndDiskSize = FVector4(0.0f, 0.0f, 1.0f, 1.5f);
	FVector4 SkyParams0 = FVector4(14.0f, 2.0f, 1.5f, 0.0f);
	FVector4 SkyParams1 = FVector4(0.0f, 0.0f, 1.0f, 0.0f);
	FVector4 CameraForward = FVector4(1.0f, 0.0f, 0.0f, 0.0f);
	FVector4 CameraRight = FVector4(0.0f, 1.0f, 0.0f, 0.0f);
	FVector4 CameraUp = FVector4(0.0f, 0.0f, 1.0f, 0.0f);
};

struct alignas(16) FGPULight
{
    uint32 Type = static_cast<uint32>(ELightType::Max);
    float  Intensity = 0.0f;
    float  Radius = 0.0f;
    float  FalloffExponent = 1.0f;

    FVector Color = FVector::ZeroVector;
    float   SpotInnerCos = 0.0f;

    FVector Position = FVector::ZeroVector;
    float   SpotOuterCos = 0.0f;

    FVector Direction = FVector::ZeroVector;
    float   Padding0 = 0.0f;
};

using FRenderLight = FGPULight;

static_assert(sizeof(FGPULight) == 64, "FGPULight layout must match the HLSL structured buffer layout.");

struct FRenderCommand
{
	FPerObjectConstants PerObjectConstants = {};

	//	VB, IB 모두 담고 있는 MB
	FMeshBuffer* MeshBuffer = nullptr;
	UMaterialInterface* Material = nullptr;
	uint32 SectionIndexStart = 0;
	uint32 SectionIndexCount = 0;
	FDecalConstants DecalConstants = {};

	union
	{
		FGridConstants Grid;
		FFontConstants Font;
		FSubUVConstants SubUV;
		FBillboardConstants Billboard;
        FSkyConstants Sky;
        FFogConstants Fog;
        FFXAAConstants FXAA;
	} Constants;

	ERenderCommandType Type = ERenderCommandType::Primitive;
};

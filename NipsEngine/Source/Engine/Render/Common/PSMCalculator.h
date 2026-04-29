#pragma once
#include "Engine/Geometry/AABB.h"
#include "Render/Scene/RenderCommand.h"
#include "Engine/Component/CameraComponent.h"
#include "Math/Utils.h"
#include <algorithm>
#include <cfloat>
#include <cmath>

struct FCamera
{
    FVector Position;
    FVector Up;
    FVector Forward;
    FVector Right;

    FCameraState CameraState;
};

namespace PSM
{

inline bool IsFiniteFloat(float Value)
{
    return std::isfinite(Value);
}

inline bool IsFiniteVector(const FVector& Value)
{
    return IsFiniteFloat(Value.X) && IsFiniteFloat(Value.Y) && IsFiniteFloat(Value.Z);
}

inline bool IsFiniteMatrix(const FMatrix& Value)
{
    for (int32 Row = 0; Row < 4; ++Row)
    {
        for (int32 Col = 0; Col < 4; ++Col)
        {
            if (!IsFiniteFloat(Value.M[Row][Col]))
            {
                return false;
            }
        }
    }

    return true;
}

inline FVector GetPostPerspectiveBoundsCenter()
{
    // D3D11 NDC uses z in [0, 1], so the post-perspective fit volume is
    // centered at z = 0.5 instead of the OpenGL-style origin-centered cube.
    return FVector(0.0f, 0.0f, 0.5f);
}

inline FVector GetPostPerspectiveBoundsExtent()
{
    return FVector(1.0f, 1.0f, 0.5f);
}

inline float GetPostPerspectiveBoundsRadius()
{
    return GetPostPerspectiveBoundsExtent().Size();
}

inline bool ProjectWorldPositionToPostPerspective(
    const FVector& WorldPosition,
    const FMatrix& InView,
    const FMatrix& InProj,
    FVector& OutPositionPP)
{
    const FVector4 ClipPosition = (InView * InProj).TransformVector4(FVector4(WorldPosition, 1.0f));
    if (!IsFiniteFloat(ClipPosition.W) || std::fabs(ClipPosition.W) <= 1.0e-5f)
    {
        return false;
    }

    const float InvW = 1.0f / ClipPosition.W;
    OutPositionPP = FVector(
        ClipPosition.X * InvW,
        ClipPosition.Y * InvW,
        ClipPosition.Z * InvW);
    return IsFiniteVector(OutPositionPP);
}

// Shadow Caster 전체를 감싸는 World AABB 반환
inline FAABB GenerateShadowCasterAABB(const TArray<FRenderCommand>& Commands)
{
    FAABB Result = {};
    for (const auto& Cmd : Commands)
    {
        if (Cmd.WorldBounds.IsValid())
        {
            Result.Merge(Cmd.WorldBounds);
        }
    }
    return Result;
}

// MainCamera의 NearZ/FarZ를 ShadowCaster AABB에 맞게 조정
// → VirtualCamera frustum이 caster를 잘라내지 않도록 보장
inline void GetCameraFitNearZ(const TArray<FRenderCommand>& Commands, FCamera& OutCamera)
{
    if (Commands.empty())
    {
        return;
    }

    const FAABB CasterAABB = GenerateShadowCasterAABB(Commands);
    if (!CasterAABB.IsValid())
    {
        return;
    }

    float MinZ = FLT_MAX;
    float MaxZ = -FLT_MAX;

    for (int i = 0; i < 8; ++i)
    {
        const FVector ToBB = CasterAABB.GetPoint(i) - OutCamera.Position;
        const float Z = OutCamera.Forward.DotProduct(ToBB);
        MinZ = std::min(Z, MinZ);
        MaxZ = std::max(Z, MaxZ);
    }

    if (!IsFiniteFloat(MinZ) || !IsFiniteFloat(MaxZ))
    {
        return;
    }

    // NearZ: caster에 가장 가까운 면 (최소 0.1)
    if (OutCamera.CameraState.NearZ < MinZ)
        OutCamera.CameraState.NearZ = std::max(0.1f, MinZ);

    // FarZ: caster를 완전히 포함하도록
    if (OutCamera.CameraState.FarZ < MaxZ)
        OutCamera.CameraState.FarZ = MaxZ * 1.1f;
}

// VirtualCamera View/Projection 생성
// 현재 엔진의 LookAt은 up "벡터"를 받으므로 reference의 upPoint 전달을 그대로 쓰면 안 된다.
inline bool GenerateVirtualCameraViewProjection(float VirtualSliderBack, const FCamera& InCamera,
                                                FMatrix& OutProj, FMatrix& OutView)
{
    if (InCamera.CameraState.bIsOrthogonal ||
        InCamera.Forward.IsNearlyZero() ||
        InCamera.Up.IsNearlyZero() ||
        !IsFiniteVector(InCamera.Position) ||
        !IsFiniteVector(InCamera.Forward) ||
        !IsFiniteVector(InCamera.Up))
    {
        return false;
    }

    const FVector Pos = InCamera.Position - InCamera.Forward * VirtualSliderBack;
    const FVector Target = InCamera.Position + InCamera.Forward;
    const FVector Up = InCamera.Up;

    OutView = FMatrix::MakeViewLookAtLH(Pos, Target, Up);
    if (!IsFiniteMatrix(OutView))
    {
        return false;
    }

    // Near: 원본 NearZ + SliderBack (카메라를 뒤로 뺀 만큼 Near도 보정)
    // Far:  원본 FarZ + SliderBack
    const float Near = std::max(0.05f, InCamera.CameraState.NearZ + VirtualSliderBack);
    const float Far = std::max(Near + 0.1f, InCamera.CameraState.FarZ + VirtualSliderBack);
    const float AspectRatio = std::max(InCamera.CameraState.AspectRatio, 1.0e-3f);

    OutProj = FMatrix::MakePerspectiveFovLH(InCamera.CameraState.FOV, AspectRatio, Near, Far);
    return IsFiniteMatrix(OutProj);
}

// PostPerspective 공간의 View/Projection 생성
// LightDirToLight: receiver -> light 방향.
inline bool GeneratePostPerspectiveViewProjection(const FVector& LightDirToLight,
                                                  FMatrix& OutProjPP, FMatrix& OutViewPP,
                                                  const FMatrix& InView, const FMatrix& InProj)
{
    if (LightDirToLight.IsNearlyZero())
    {
        return false;
    }

    const FVector CubeCenterPP = GetPostPerspectiveBoundsCenter();
    const float CubeRadiusPP = GetPostPerspectiveBoundsRadius();

    FVector LightPosPP;
    float FovPP = 0.0f;
    float NearPP = 0.0f;
    float FarPP = 0.0f;

    // Eye 공간의 LightDir
    const FVector EyeLightDir = InView.TransformVector(LightDirToLight);
    if (EyeLightDir.IsNearlyZero() || !IsFiniteVector(EyeLightDir))
    {
        return false;
    }

    // PP 공간으로 변환
    const FVector4 LightPP = InProj.TransformVector4(FVector4(EyeLightDir, 0.0f));
    if (!IsFiniteFloat(LightPP.X) ||
        !IsFiniteFloat(LightPP.Y) ||
        !IsFiniteFloat(LightPP.Z) ||
        !IsFiniteFloat(LightPP.W))
    {
        return false;
    }

    const bool LightIsBehindOfEye = (LightPP.W < 0.0f);

    static const float W_EPSILON = 0.001f;
    const bool IsOrthoMatrix = (fabsf(LightPP.W) <= W_EPSILON);

    const float WidthPP = 1.0f;
    const float HeightPP = 1.0f;

    if (IsOrthoMatrix)
    {
        const FVector LightDirPP = FVector(LightPP.X, LightPP.Y, LightPP.Z).GetSafeNormal();
        if (LightDirPP.IsNearlyZero())
        {
            return false;
        }

        // NDC Unit Cube를 감싸는 카메라 위치
        LightPosPP = CubeCenterPP + LightDirPP * 2.0f * CubeRadiusPP;
        const float DistToCenter = LightPosPP.Size();

        NearPP = DistToCenter - CubeRadiusPP;
        FarPP = DistToCenter + CubeRadiusPP;

        FVector UpVector = FVector::UpVector;
        if (fabsf(UpVector.DotProduct((CubeCenterPP - LightPosPP).GetSafeNormal())) > 0.99f)
            UpVector = FVector::RightVector;

        OutViewPP = FMatrix::MakeViewLookAtLH(LightPosPP, CubeCenterPP, LightPosPP + UpVector);
        OutProjPP = FMatrix::MakeOrthographicLH(CubeRadiusPP * 2.0f, CubeRadiusPP * 2.0f, NearPP, FarPP);
    }
    else
    {
        // PP 공간의 LightPos 계산
        const float wRecip = 1.0f / LightPP.W;
        LightPosPP.X = LightPP.X * wRecip;
        LightPosPP.Y = LightPP.Y * wRecip;
        LightPosPP.Z = LightPP.Z * wRecip;
        if (!IsFiniteVector(LightPosPP))
        {
            return false;
        }

        FVector LookAtCubePP = CubeCenterPP - LightPosPP;
        const float DistLookAtCubePP = LookAtCubePP.Size();
        if (DistLookAtCubePP <= 1.0e-5f || !IsFiniteFloat(DistLookAtCubePP))
        {
            return false;
        }
        LookAtCubePP /= DistLookAtCubePP;

        if (LightIsBehindOfEye)
        {
            const FVector ToBSphere = CubeCenterPP - LightPosPP;
            const float DistToBSphere = ToBSphere.Size();

            NearPP = DistToBSphere - CubeRadiusPP;
            FovPP = 2.0f * atanf(CubeRadiusPP / DistToBSphere);

            // Inverse Perspective 트릭: Near를 음수로
            NearPP = std::max(0.1f, NearPP);
            FarPP = NearPP; // ← 원본: FarPP = NearPP 스왑 후 NearPP 반전
            NearPP = -NearPP;

            OutProjPP = FMatrix::MakePerspectiveFovLH(FovPP, WidthPP / HeightPP, NearPP, FarPP);
        }
        else
        {
            FovPP = 2.0f * atanf(CubeRadiusPP / DistLookAtCubePP);
            NearPP = std::max(0.1f, DistLookAtCubePP - CubeRadiusPP);
            FarPP = DistLookAtCubePP + CubeRadiusPP;

            OutProjPP = FMatrix::MakePerspectiveFovLH(FovPP, 1.0f, NearPP, FarPP);
        }

        FVector UpVector = FVector::UpVector;
        if (fabsf(FVector::UpVector.DotProduct(LookAtCubePP)) > 0.99f)
            UpVector = FVector::RightVector;

        OutViewPP = FMatrix::MakeViewLookAtLH(LightPosPP, CubeCenterPP, LightPosPP + UpVector);
    }

    return IsFiniteMatrix(OutViewPP) &&
           IsFiniteMatrix(OutProjPP) &&
           IsFiniteFloat(NearPP) &&
           IsFiniteFloat(FarPP) &&
           (FarPP > NearPP);
}

struct FSpotLightPerspectiveFitStats
{
    float Near = 0.0f;
    float Far = 0.0f;
    float MinClipX = 0.0f;
    float MaxClipX = 0.0f;
    float MinClipY = 0.0f;
    float MaxClipY = 0.0f;
    int32 ValidPointCount = 0;
};

inline FMatrix MakeNdcCropMatrix(
    float MinX,
    float MaxX,
    float MinY,
    float MaxY,
    float MinZ,
    float MaxZ)
{
    const float ScaleX = 2.0f / (MaxX - MinX);
    const float ScaleY = 2.0f / (MaxY - MinY);
    const float ScaleZ = 1.0f / (MaxZ - MinZ);
    const float OffsetX = -(MinX + MaxX) / (MaxX - MinX);
    const float OffsetY = -(MinY + MaxY) / (MaxY - MinY);
    const float OffsetZ = -MinZ / (MaxZ - MinZ);

    return FMatrix(
        ScaleX, 0.0f, 0.0f, 0.0f,
        0.0f, ScaleY, 0.0f, 0.0f,
        0.0f, 0.0f, ScaleZ, 0.0f,
        OffsetX, OffsetY, OffsetZ, 1.0f);
}

// Reference PSM only covers directional lights. For finite spot lights we keep the
// physical light view/projection fixed to the real light transform, then apply a
// camera-dependent clip-space crop. That tightens texel distribution without making
// the spotlight cone behave as if it follows the camera.
inline bool GenerateSpotLightPerspectiveFitViewProjection(
    const FCamera& Camera,
    const TArray<FRenderCommand>& ReceiverCommands,
    const TArray<FRenderCommand>& CasterCommands,
    const FVector& LightPosition,
    const FVector& LightDirection,
    float SpotOuterCos,
    float LightRange,
    FMatrix& OutView,
    FMatrix& OutProj,
    FSpotLightPerspectiveFitStats* OutStats = nullptr)
{
    const FVector LightDir = LightDirection.GetSafeNormal();
    if (LightDir.IsNearlyZero() ||
        !IsFiniteVector(LightPosition) ||
        !IsFiniteVector(Camera.Position) ||
        Camera.CameraState.bIsOrthogonal)
    {
        return false;
    }

    FVector UpVector = FVector::UpVector;
    if (std::fabs(FVector::DotProduct(LightDir, UpVector)) > 0.99f)
    {
        UpVector = FVector::RightVector;
    }

    OutView = FMatrix::MakeViewLookAtLH(LightPosition, LightPosition + LightDir, UpVector);
    if (!IsFiniteMatrix(OutView))
    {
        return false;
    }

    const float HalfAngle = std::acos(std::clamp(SpotOuterCos, 0.001f, 0.9999f));
    const float FovRad = HalfAngle * 2.0f;
    const float NearZ = 0.1f;
    const float SafeRange = std::max(LightRange, 1.0f);
    if (!IsFiniteFloat(FovRad) || FovRad <= 1.0e-4f)
    {
        return false;
    }

    const FMatrix BaseProjection = FMatrix::MakePerspectiveFovLH(FovRad, 1.0f, NearZ, SafeRange);
    if (!IsFiniteMatrix(BaseProjection))
    {
        return false;
    }

    const float CameraNear = std::max(Camera.CameraState.NearZ, 0.05f);
    const float ApproxVisibleFar =
        FVector::Dist(Camera.Position, LightPosition) + SafeRange;
    const float CameraFar =
        std::max(CameraNear + 0.1f, std::min(Camera.CameraState.FarZ, ApproxVisibleFar));

    const float SampleDepths[3] =
    {
        CameraNear,
        (CameraNear + CameraFar) * 0.5f,
        CameraFar
    };
    const float SampleCoords[3] = { -1.0f, 0.0f, 1.0f };

    float MinDepth = FLT_MAX;
    float MaxDepth = -FLT_MAX;
    float MinClipX = FLT_MAX;
    float MaxClipX = -FLT_MAX;
    float MinClipY = FLT_MAX;
    float MaxClipY = -FLT_MAX;
    float MinClipZ = FLT_MAX;
    float MaxClipZ = -FLT_MAX;
    int32 ValidPointCount = 0;

    auto AccumulateWorldPoint = [&](const FVector& WorldPoint)
    {
        const FVector LightSpacePoint = OutView.TransformPosition(WorldPoint);
        if (!IsFiniteVector(LightSpacePoint))
        {
            return;
        }

        const float Depth = LightSpacePoint.X;
        if (Depth <= NearZ || Depth > SafeRange)
        {
            return;
        }

        FVector ShadowNdc = FVector::ZeroVector;
        if (!ProjectWorldPositionToPostPerspective(WorldPoint, OutView, BaseProjection, ShadowNdc) ||
            !IsFiniteVector(ShadowNdc))
        {
            return;
        }

        constexpr float ClipPadding = 0.05f;
        if (ShadowNdc.Z < -ClipPadding || ShadowNdc.Z > (1.0f + ClipPadding) ||
            ShadowNdc.X < (-1.0f - ClipPadding) || ShadowNdc.X > (1.0f + ClipPadding) ||
            ShadowNdc.Y < (-1.0f - ClipPadding) || ShadowNdc.Y > (1.0f + ClipPadding))
        {
            return;
        }

        MinDepth = std::min(MinDepth, Depth);
        MaxDepth = std::max(MaxDepth, Depth);
        MinClipX = std::min(MinClipX, ShadowNdc.X);
        MaxClipX = std::max(MaxClipX, ShadowNdc.X);
        MinClipY = std::min(MinClipY, ShadowNdc.Y);
        MaxClipY = std::max(MaxClipY, ShadowNdc.Y);
        MinClipZ = std::min(MinClipZ, ShadowNdc.Z);
        MaxClipZ = std::max(MaxClipZ, ShadowNdc.Z);
        ++ValidPointCount;
    };

    const float HalfTanFov = std::tan(Camera.CameraState.FOV * 0.5f);
    for (float Depth : SampleDepths)
    {
        const float HalfHeight = HalfTanFov * Depth;
        const float HalfWidth = HalfHeight * std::max(Camera.CameraState.AspectRatio, 1.0e-3f);
        for (float Horizontal : SampleCoords)
        {
            for (float Vertical : SampleCoords)
            {
                const FVector WorldPoint =
                    Camera.Position +
                    Camera.Forward * Depth +
                    Camera.Right * (Horizontal * HalfWidth) +
                    Camera.Up * (Vertical * HalfHeight);
                AccumulateWorldPoint(WorldPoint);
            }
        }
    }

    auto AccumulateCommandCenters = [&](const TArray<FRenderCommand>& Commands)
    {
        for (const FRenderCommand& Command : Commands)
        {
            if (!Command.WorldBounds.IsValid())
            {
                continue;
            }

            AccumulateWorldPoint(Command.WorldBounds.GetCenter());
            for (int32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
            {
                AccumulateWorldPoint(Command.WorldBounds.GetPoint(CornerIndex));
            }
        }
    };

    AccumulateCommandCenters(ReceiverCommands);
    AccumulateCommandCenters(CasterCommands);

    if (OutStats != nullptr)
    {
        OutStats->ValidPointCount = ValidPointCount;
        OutStats->Near = (ValidPointCount > 0 && IsFiniteFloat(MinDepth)) ? MinDepth : 0.0f;
        OutStats->Far = (ValidPointCount > 0 && IsFiniteFloat(MaxDepth)) ? MaxDepth : 0.0f;
        OutStats->MinClipX =
            (ValidPointCount > 0 && IsFiniteFloat(MinClipX)) ? MinClipX : 0.0f;
        OutStats->MaxClipX =
            (ValidPointCount > 0 && IsFiniteFloat(MaxClipX)) ? MaxClipX : 0.0f;
        OutStats->MinClipY =
            (ValidPointCount > 0 && IsFiniteFloat(MinClipY)) ? MinClipY : 0.0f;
        OutStats->MaxClipY =
            (ValidPointCount > 0 && IsFiniteFloat(MaxClipY)) ? MaxClipY : 0.0f;
    }

    if (ValidPointCount <= 0 ||
        !IsFiniteFloat(MinDepth) ||
        !IsFiniteFloat(MaxDepth) ||
        !IsFiniteFloat(MinClipX) ||
        !IsFiniteFloat(MaxClipX) ||
        !IsFiniteFloat(MinClipY) ||
        !IsFiniteFloat(MaxClipY) ||
        !IsFiniteFloat(MinClipZ) ||
        !IsFiniteFloat(MaxClipZ))
    {
        return false;
    }

    auto ExpandClipRange = [](float& MinValue, float& MaxValue, float ClampMin, float ClampMax)
    {
        const float Padding = std::max(0.02f, (MaxValue - MinValue) * 0.1f);
        MinValue = std::max(ClampMin, MinValue - Padding);
        MaxValue = std::min(ClampMax, MaxValue + Padding);
        if (MaxValue - MinValue < 0.05f)
        {
            const float Center = (MinValue + MaxValue) * 0.5f;
            const float HalfExtent = 0.025f;
            MinValue = std::max(ClampMin, Center - HalfExtent);
            MaxValue = std::min(ClampMax, Center + HalfExtent);
        }
    };

    ExpandClipRange(MinClipX, MaxClipX, -1.0f, 1.0f);
    ExpandClipRange(MinClipY, MaxClipY, -1.0f, 1.0f);
    ExpandClipRange(MinClipZ, MaxClipZ, 0.0f, 1.0f);
    if (MaxClipX <= MinClipX || MaxClipY <= MinClipY || MaxClipZ <= MinClipZ)
    {
        return false;
    }

    const FMatrix CropMatrix = MakeNdcCropMatrix(MinClipX, MaxClipX, MinClipY, MaxClipY, MinClipZ, MaxClipZ);
    OutProj = BaseProjection * CropMatrix;
    if (!IsFiniteMatrix(OutProj))
    {
        return false;
    }

    if (OutStats != nullptr)
    {
        OutStats->Near = MinDepth;
        OutStats->Far = MaxDepth;
        OutStats->MinClipX = MinClipX;
        OutStats->MaxClipX = MaxClipX;
        OutStats->MinClipY = MinClipY;
        OutStats->MaxClipY = MaxClipY;
    }

    return true;
}

} // namespace PSM

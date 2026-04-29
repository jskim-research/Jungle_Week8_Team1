#pragma once
#include "Engine/Geometry/AABB.h"
#include "Render/Scene/RenderCommand.h"
#include "Engine/Component/CameraComponent.h"

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

// Shadow Caster 전체를 감싸는 World AABB 반환
inline FAABB GenerateShadowCasterAABB(const TArray<FRenderCommand>& Commands)
{
    FAABB Result = {};
    for (const auto& Cmd : Commands)
        Result.Merge(Cmd.WorldBounds);
    return Result;
}

// MainCamera의 NearZ/FarZ를 ShadowCaster AABB에 맞게 조정
// → VirtualCamera frustum이 caster를 잘라내지 않도록 보장
inline void GetCameraFitNearZ(const TArray<FRenderCommand>& Commands, FCamera& OutCamera)
{
    const FAABB CasterAABB = GenerateShadowCasterAABB(Commands);

    float MinZ = FLT_MAX;
    float MaxZ = -FLT_MAX;

    for (int i = 0; i < 8; ++i)
    {
        const FVector ToBB = CasterAABB.GetPoint(i) - OutCamera.Position;
        const float Z = OutCamera.Forward.DotProduct(ToBB);
        MinZ = std::min(Z, MinZ);
        MaxZ = std::max(Z, MaxZ);
    }

    // NearZ: caster에 가장 가까운 면 (최소 0.1)
    if (OutCamera.CameraState.NearZ < MinZ)
        OutCamera.CameraState.NearZ = std::max(0.1f, MinZ);

    // FarZ: caster를 완전히 포함하도록
    if (OutCamera.CameraState.FarZ < MaxZ)
        OutCamera.CameraState.FarZ = MaxZ * 1.1f;
}

// VirtualCamera View/Projection 생성
// 원본: Up = Pos + PrevUp (CreateViewMatrix가 upPoint를 받는 구현)
inline bool GenerateVirtualCameraViewProjection(float VirtualSliderBack, const FCamera& InCamera,
                                                FMatrix& OutProj, FMatrix& OutView)
{
    const FVector PrevUp = InCamera.Up;
    const FVector Pos = InCamera.Position - InCamera.Forward * VirtualSliderBack;
    const FVector Target = InCamera.Position + InCamera.Forward;
    const FVector Up = Pos + PrevUp; // upPoint (위치)

    OutView = FMatrix::MakeViewLookAtLH(Pos, Target, Up);

    // Near: 원본 NearZ + SliderBack (카메라를 뒤로 뺀 만큼 Near도 보정)
    // Far:  원본 FarZ + SliderBack
    const float Near = InCamera.CameraState.NearZ + VirtualSliderBack;
    const float Far = InCamera.CameraState.FarZ + VirtualSliderBack;

    OutProj = FMatrix::MakePerspectiveFovLH(InCamera.CameraState.FOV, 1.0f, Near, Far);
    return true;
}

// PostPerspective 공간의 View/Projection 생성
// LightDir: 라이트 방향 (내부에서 -LightDir로 EyeSpace 변환)
inline void GeneratePostPerspectiveViewProjection(const FVector& LightDir,
                                                  FMatrix& OutProjPP, FMatrix& OutViewPP,
                                                  const FMatrix& InView, const FMatrix& InProj)
{
    // 원본과 동일: CubeRadius 하나 (xy/z 분리 없음)
    const FVector CubeCenterPP = FVector::Zero();
    const float CubeRadiusPP = FVector::OneVector.Size(); // sqrt(3) ≈ 1.732

    FVector LightPosPP;
    float FovPP = 0.0f;
    float NearPP = 0.0f;
    float FarPP = 0.0f;

    // Eye 공간의 LightDir
    const FVector EyeLightDir = InView.TransformVector4(FVector4(-LightDir, 0.0f)).ToVector3();

    // PP 공간으로 변환
    const FVector4 LightPP = InProj.TransformVector4(FVector4(EyeLightDir, 0.0f));

    const bool LightIsBehindOfEye = (LightPP.W < 0.0f);

    static const float W_EPSILON = 0.001f;
    const bool IsOrthoMatrix = (fabsf(LightPP.W) <= W_EPSILON);

    const float WidthPP = 1.0f;
    const float HeightPP = 1.0f;

    if (IsOrthoMatrix)
    {
        // 원본: LightDirPP에 CubeRadiusPP 별도 스케일 없이 xyz 그대로 사용
        const FVector LightDirPP(LightPP.X, LightPP.Y, LightPP.Z);

        // NDC Unit Cube를 감싸는 카메라 위치
        LightPosPP = CubeCenterPP + LightDirPP* 2.0f * CubeRadiusPP;
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

        FVector LookAtCubePP = CubeCenterPP - LightPosPP;
        const float DistLookAtCubePP = LookAtCubePP.Size();
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
}

} // namespace PSM
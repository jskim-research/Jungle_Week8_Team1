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


FAABB GenerateShadowCasterAABB(const TArray<FRenderCommand>& Commands)
{
    FAABB ShadowReceiverAABB = {};
    for (auto Cmd : Commands)
    {
        ShadowReceiverAABB.Merge(Cmd.WorldBounds);
    }
    return ShadowReceiverAABB;
}

// 카메라 뒤로 빼기
void GetCameraFitNearZ(const TArray<FRenderCommand>& Commands, FCamera& outCamera)
{
    FAABB LightWolrdBound = GenerateShadowCasterAABB(Commands);


    float MinZ = FLT_MAX;
    float MaxZ = -FLT_MAX;

    FVector MainCameraForward = outCamera.Forward;
    FVector MainCameraRight = outCamera.Right;

    for (int i = 0; i < 8; ++i)
    {
        FVector ToBB = (LightWolrdBound.GetPoint(i) - outCamera.Position);
        float Z = MainCameraForward.DotProduct(ToBB);
        float X = MainCameraRight.DotProduct(ToBB);

        MinZ = std::min(Z, MinZ);
        MaxZ = std::max(Z, MaxZ);
    }

    if (outCamera.CameraState.NearZ < MinZ)
        outCamera.CameraState.NearZ = std::max(0.1f, MinZ);

    // FarZ도 맞춰줘야 오브젝트가 안 잘림
    if (outCamera.CameraState.FarZ < MaxZ)
        outCamera.CameraState.FarZ = MaxZ * 1.1f; // 약간 여유
}

bool GenerateVirtualCameraViewProjection(float VirtualSliderBack, FCamera inCamera, FMatrix& OutProj, FMatrix& OutView)
{
    FAABB LightWolrdBound;

    auto PrevUp = inCamera.Up;
    FVector Pos = inCamera.Position - (inCamera.Forward * VirtualSliderBack);
    FVector Target = inCamera.Position + inCamera.Forward;
    FVector Up = Pos + PrevUp;

    OutView = FMatrix::MakeViewLookAtLH(Pos, Target, Up);


    // float OuterAngleRad = acos(Light.SpotOuterCos); // 반각(half angle)
    // float FovRad = OuterAngleRad * 2.0f;            // 전체 FOV

    // float NearZ = 0.1f;
    // float Radius = Light.Radius;
    // float FarZ = std::max(Radius, NearZ + 0.1f);
    OutProj = FMatrix::MakePerspectiveFovLH(inCamera.CameraState.FOV, 1.0f,
                                            inCamera.CameraState.NearZ + VirtualSliderBack, inCamera.CameraState.FarZ); // Far = 라이트반경)

    return true;
}


void GeneratePostPerspectiveViewProjection(FVector LightDir,
                                           FMatrix& OutProjPP, FMatrix& OutViewPP,
                                           const FMatrix& InView, const FMatrix& InProj /*, bool updatePPCamera*/)
{
    FVector CubeCenterPP = FVector::Zero();
    float CubeRadiusPPxy = FVector::OneVector.Size(); // 6. PP 공간의 큐브의 반지름을 구함. (OpenGL은 NDC 공간이 X, Y, Z 모두 길이 2임)
    float CubeRadiusPPz = FVector::OneVector.Size() / 2;

    FVector LightPosPP;
    float FovPP = 0.0f;
    float NearPP = 0.0f;
    float FarPP = 0.0f;

    // Camera 공간의 LightDir 구함
    FVector EyeLightDir = InView.TransformVector4(FVector4(-LightDir, 0.0f)).ToVector3();

    // Camera 공간의 LightDir을 PP 공간으로 이동시킴
    FVector4 LightPP = InProj.TransformVector4(FVector4(EyeLightDir, 0.0f));

    // 라이트가 Eye의 뒤쪽에 있는지 판단한다.
    bool LightIsBehindOfEye = (LightPP.W < 0.0f);

    // 시야 방향과 라이트가 직교하는 상태인지 확인하고, 직교하면 OrthoMatrix 사용
    static float W_EPSILON = 0.001f;
    bool IsOrthoMatrix = (fabsf(LightPP.W) <= W_EPSILON);

    float WidthPP = 1.0f;
    float HeightPP = 1.0f;

    if (IsOrthoMatrix)
    {
        FVector LightDirPP(LightPP.X * CubeRadiusPPxy, LightPP.Y * CubeRadiusPPxy, LightPP.Z * CubeRadiusPPz);

        // NDC Unit Cube를 딱 감쌀 수 있는 View와 Projection Matrix를 생성합니다.
        LightPosPP = CubeCenterPP + LightDirPP * 2.0 * CubeRadiusPPxy;
        float DistToCenter = LightPosPP.Size();

        NearPP = DistToCenter - CubeRadiusPPz;
        FarPP = DistToCenter + CubeRadiusPPz;

        FVector UpVector = FVector::UpVector;
        if (fabsf(UpVector.DotProduct((CubeCenterPP - LightPosPP).GetSafeNormal())) > 0.99f)
            UpVector = FVector::RightVector;

        OutViewPP = FMatrix::MakeViewLookAtLH(LightPosPP, CubeCenterPP, (LightPosPP + UpVector));
        OutProjPP = FMatrix::MakeOrthographicLH(CubeRadiusPPxy * 2, CubeRadiusPPxy * 2, FarPP, NearPP);

        // PP 공간 디버깅용 카메라 업데이트
        // if (updatePPCamera)
        //{
        //    OutPPCamera = std::shared_ptr<jCamera>(jCamera::CreateCamera(LightPosPP, CubeCenterPP, (LightPosPP + UpVector), FovPP, NearPP, FarPP, CubeRadiusPP * 2, CubeRadiusPP * 2, !IsOrthoMatrix));
        //    OutPPCamera->UpdateCamera();
        //}
    }
    else
    {
        // PP 공간의 LightDir로 변경한 후, LightDir을 LightPos으로 변경함.
        float wRecip = 1.0f / LightPP.W;
        LightPosPP.X = LightPP.X * wRecip;
        LightPosPP.Y = LightPP.Y * wRecip;
        LightPosPP.Z = LightPP.Z * wRecip;

        // LightPP위치에서 CubeCenter를 바라보는 벡터와 그 벡터의 거리를 구함.
        FVector LookAtCubePP = (CubeCenterPP - LightPosPP);
        float DistLookAtCubePP = LookAtCubePP.Size();
        LookAtCubePP /= DistLookAtCubePP;

         if (LightIsBehindOfEye)
        {
             FVector ToBSphereDirection = CubeCenterPP - LightPosPP;
             const float DistToBSphereDirection = ToBSphereDirection.Size();
             ToBSphereDirection = ToBSphereDirection.GetSafeNormal();

            NearPP = DistToBSphereDirection - CubeRadiusPPz;
            FovPP = 2.0f * atanf(CubeRadiusPPz / DistToBSphereDirection);

            // Perspective Matrix의 Near를 마이너스로 두는 트릭을 사용함.
            NearPP = std::max(0.1f, NearPP);
            FarPP = NearPP;
            NearPP = -NearPP;

            // PostPerspective 공간에서 사용할 Projection 을 계산
            OutProjPP = FMatrix::MakePerspectiveFovLH(FovPP, WidthPP / HeightPP,NearPP, FarPP);
        }
        else
        {
            // NDC Unit Cube 공간의 바운드박스를 만듬
            FovPP = 2.0f * atanf(CubeRadiusPPxy / DistLookAtCubePP);
            float AspectPP = 1.0f;

            NearPP = std::max(0.1f, DistLookAtCubePP - CubeRadiusPPz);
            FarPP = DistLookAtCubePP + CubeRadiusPPz;

            // PostPerspective 공간에서 사용할 Projection 을 계산
            OutProjPP = FMatrix::MakePerspectiveFovLH(FovPP, 1.0f, NearPP, FarPP);
        }

        FVector UpVector = FVector::UpVector;
        if (fabsf(FVector::UpVector.DotProduct(LookAtCubePP)) > 0.99f)
            UpVector = FVector::RightVector;

        // PP에서의 라이트 위치, PP의 중심, 위에서 구한 Up벡터를 사용해서 PP에서의 ViewMatrix 구함.
        OutViewPP = FMatrix::MakeViewLookAtLH(LightPosPP, CubeCenterPP, (LightPosPP + UpVector));

        // PP 공간 디버깅용 카메라 업데이트
        // if (updatePPCamera)
        //{
        //    OutPPCamera = std::shared_ptr<jCamera>(jCamera::CreateCamera(LightPosPP, CubeCenterPP, (LightPosPP + UpVector), FovPP, NearPP, FarPP, WidthPP, HeightPP, !IsOrthoMatrix));
        //    OutPPCamera->UpdateCamera();
        //}
    }
}
} // namespace PSM
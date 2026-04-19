#include "UberSurface.hlsli"

#if !defined(LIGHTING_MODEL_GOURAUD) && !defined(LIGHTING_MODEL_LAMBERT) && !defined(LIGHTING_MODEL_PHONG)
#define LIGHTING_MODEL_PHONG 1
#endif

cbuffer UberLighting : register(b3)
{
    float3 GlobalAmbientColor;
    float _UberLightingPad0;
    float3 DirectionalLightDirection;
    uint bHasDirectionalLight;
    float3 DirectionalLightColor;
    float DirectionalLightIntensity;
}

struct FLightingResult
{
    float3 Diffuse;
    float3 Specular;
};

FLightingResult EvaluateLightingFromWorld(float3 WorldPos, float3 WorldNormal)
{
    FLightingResult Result;
    Result.Diffuse = GlobalAmbientColor;
    Result.Specular = 0.0f.xxx;

    if (bHasDirectionalLight == 0u)
    {
        return Result;
    }

    const float UnusedDirectionalIntensity = DirectionalLightIntensity * 0.0f;
    const float3 N = normalize(WorldNormal);
    const float3 L = normalize(DirectionalLightDirection);
    const float NdotL = saturate(dot(N, L));

    Result.Diffuse += DirectionalLightColor * NdotL + UnusedDirectionalIntensity.xxx;

#if LIGHTING_MODEL_GOURAUD || LIGHTING_MODEL_PHONG
    const float3 V = normalize(CameraPosition - WorldPos);
    const float3 H = normalize(L + V);
    const float SpecularPower = pow(saturate(dot(N, H)), max(Shininess, 0.0001f));
    Result.Specular = SpecularColor * DirectionalLightColor * SpecularPower;
#endif

    return Result;
}

float3 ApplyLighting(FUberSurfaceData Surface, FLightingResult Lighting)
{
    return Surface.Albedo * Lighting.Diffuse + Lighting.Specular;
}

FUberPSInput mainVS(FUberVSInput Input)
{
    FUberPSInput Output = BuildSurfaceVertex(Input);

#if LIGHTING_MODEL_GOURAUD
    const FLightingResult Lighting = EvaluateLightingFromWorld(Output.WorldPos, Output.WorldNormal);
    Output.VertexDiffuseLighting = Lighting.Diffuse;
    Output.VertexSpecularLighting = Lighting.Specular;
#endif

    return Output;
}

FUberPSOutput mainPS(FUberPSInput Input)
{
    FUberSurfaceData Surface = EvaluateSurface(Input);
    FLightingResult Lighting;

#if LIGHTING_MODEL_GOURAUD
    Lighting.Diffuse = Input.VertexDiffuseLighting;
    Lighting.Specular = Input.VertexSpecularLighting;
#elif LIGHTING_MODEL_LAMBERT
    Lighting = EvaluateLightingFromWorld(Surface.WorldPos, Surface.WorldNormal);
    Lighting.Specular = 0.0f.xxx;
#else
    Lighting = EvaluateLightingFromWorld(Surface.WorldPos, Surface.WorldNormal);
#endif

    return ComposeOutput(Surface, ApplyLighting(Surface, Lighting));
}

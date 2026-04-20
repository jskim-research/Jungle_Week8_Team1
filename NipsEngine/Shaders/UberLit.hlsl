#include "UberSurface.hlsli"

#if !defined(LIGHTING_MODEL_GOURAUD) && !defined(LIGHTING_MODEL_LAMBERT) && !defined(LIGHTING_MODEL_PHONG)
#define LIGHTING_MODEL_PHONG 1
#endif

cbuffer UberLighting : register(b3)
{
    uint SceneLightCount;
    float3 _UberLightingPad0;
}

struct FGPULight
{
    uint Type;
    float Intensity;
    float Radius;
    float FalloffExponent;

    float3 Color;
    float SpotInnerCos;

    float3 Position;
    float SpotOuterCos;

    float3 Direction;
    float Padding0;
};

StructuredBuffer<FGPULight> SceneLights : register(t3);

static const uint LIGHT_TYPE_DIRECTIONAL = 0u;
static const uint LIGHT_TYPE_POINT = 1u;
static const uint LIGHT_TYPE_SPOT = 2u;
static const uint LIGHT_TYPE_AMBIENT = 3u;
static const float3 DEFAULT_AMBIENT_COLOR = float3(0.02f, 0.02f, 0.02f);

struct FLightingResult
{
    float3 Diffuse;
    float3 Specular;
};

float ComputeDistanceAttenuation(float Distance, float Radius, float FalloffExponent)
{
    if (Radius <= 0.0f)
    {
        return 0.0f;
    }

    const float RadiusFactor = saturate(1.0f - (Distance / max(Radius, 1.0e-4f)));
    return pow(RadiusFactor, max(FalloffExponent, 1.0e-4f));
}

void AccumulateDirectLight(float3 WorldPos, float3 N, float3 V, float3 L, float3 LightContribution, inout FLightingResult Result)
{
    const float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0f)
    {
        return;
    }

    Result.Diffuse += LightContribution * NdotL;

#if LIGHTING_MODEL_GOURAUD || LIGHTING_MODEL_PHONG
    const float3 H = normalize(L + V);
    const float SpecularPower = pow(saturate(dot(N, H)), max(Shininess, 1.0e-4f));
    Result.Specular += SpecularColor * LightContribution * SpecularPower;
#endif
}

FLightingResult EvaluateLightingFromWorld(float3 WorldPos, float3 WorldNormal)
{
    FLightingResult Result;
    Result.Diffuse = 0.0f.xxx;
    Result.Specular = 0.0f.xxx;
    const float3 N = normalize(WorldNormal);
    const float3 V = normalize(CameraPosition - WorldPos);
    float3 AmbientContribution = DEFAULT_AMBIENT_COLOR;
    uint bHasAmbientLight = 0u;

    [loop]
    for (uint LightIndex = 0u; LightIndex < SceneLightCount; ++LightIndex)
    {
        const FGPULight Light = SceneLights[LightIndex];
        const float3 LightColor = Light.Color * Light.Intensity;

        if (Light.Type == LIGHT_TYPE_AMBIENT)
        {
            if (bHasAmbientLight == 0u)
            {
                AmbientContribution = 0.0f.xxx;
                bHasAmbientLight = 1u;
            }

            AmbientContribution += LightColor;
            continue;
        }

        if (Light.Type == LIGHT_TYPE_DIRECTIONAL)
        {
            const float3 L = normalize(Light.Direction);
            AccumulateDirectLight(WorldPos, N, V, L, LightColor, Result);
            continue;
        }

        if (Light.Type == LIGHT_TYPE_POINT || Light.Type == LIGHT_TYPE_SPOT)
        {
            const float3 ToLight = Light.Position - WorldPos;
            const float Distance = length(ToLight);
            if (Distance <= 1.0e-4f)
            {
                continue;
            }

            const float3 L = ToLight / Distance;
            float Attenuation = ComputeDistanceAttenuation(Distance, Light.Radius, Light.FalloffExponent);
            if (Attenuation <= 0.0f)
            {
                continue;
            }

            if (Light.Type == LIGHT_TYPE_SPOT)
            {
                const float3 LightToSurface = -L;
                const float ConeFactor = dot(normalize(Light.Direction), LightToSurface);
                const float ConeRange = max(Light.SpotInnerCos - Light.SpotOuterCos, 1.0e-4f);
                const float ConeAttenuation = saturate((ConeFactor - Light.SpotOuterCos) / ConeRange);
                Attenuation *= ConeAttenuation;

                if (Attenuation <= 0.0f)
                {
                    continue;
                }
            }

            AccumulateDirectLight(WorldPos, N, V, L, LightColor * Attenuation, Result);
        }
    }

    Result.Diffuse += AmbientContribution;

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

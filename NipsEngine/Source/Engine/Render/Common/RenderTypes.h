#pragma once

//	Primtive Type Enum
enum class EPrimitiveType
{
	EPT_TransGizmo,
	EPT_RotGizmo,
	EPT_ScaleGizmo,
	EPT_Line,
	EPT_Axis,
	EPT_Grid,
	EPT_StaticMesh,
	EPT_Billboard,
	EPT_Text, // TextRenderComponent — MeshBuffer 없음, FontBatcher가 처리
	EPT_SubUV, // SubUVComponent     — MeshBuffer 없음, SubUVBatcher가 처리
	EPT_SKY,
	EPT_FOG,
	EPT_Decal,
	MAX
};

enum class ERenderPass : uint32
{
	Sky,
	Shadow,
	Opaque,
	Decal,
	Light,
	Fog,
	FXAA,
	Font, // TextRenderComponent → FontBatcher 경유
	SubUV, // SubUVComponent     → SubUVBatcher 경유
	Billboard,
	Translucent,
	SelectionMask,
	Grid, 
	Editor,
	DepthLess,
	PostProcessOutline,
	ToonOutline,
	MAX
};

enum class ELightType
{
	LightType_Directional = 0,
	LightType_Point = 1,
	LightType_Spot = 2,
	LightType_AmbientLight = 3,
	Max
};

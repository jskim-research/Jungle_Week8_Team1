#pragma once


#include "Core/Containers/String.h"
// 엔진 전역 경로를 관리합니다.
// 모든 경로는 실행 파일 기준 상대 경로이며, 한글 경로를 위해 wstring 기반입니다.
class FPaths
{
public:
	// 프로젝트 루트 (실행 파일이 있는 디렉터리)
	static FWString RootDir();

	// 주요 디렉터리
	static FWString ShaderDir();      // Shaders/
	static FWString SceneDir();       // Asset/Scene/
	static FWString DumpDir();        // Saves/Dump/
	static FWString SettingsDir();    // Settings/
	static FWString MaterialTextureDir(); //Model/Texture/

	// 주요 파일 경로
	static FWString ShaderFilePath(); // Shaders/ShaderW0.hlsl
	static FWString SettingsFilePath();  // Settings/Editor.ini
	static FWString ViewerSettingsFilePath(); // Settings/ObjViewer.ini
	static FWString AssetDirectoryPath();  // Settings/Resource.ini
	static FWString ResourceDefaultMaterialTexture(); // Asset/Mesh/Default.png
	static FWString ToRelative(const FWString& AbsolutePath);
	static FWString ToAbsolute(const FWString& RelativePath);
	static FString ToRelativeString(const FWString& AbsolutePath);
	static FString ToAbsoluteString(const FWString& RelativePath);

	// 경로 결합: FPaths::Combine(L"Asset/Scene", L"Default.Scene")
	static FWString Combine(const FWString& Base, const FWString& Child);

	// 디렉터리가 없으면 재귀적으로 생성
	static void CreateDir(const FWString& Path);

	// 변환 유틸리티 (한글 경로 지원)
	static FWString ToWide(const FString& Utf8Str);
	static FString ToUtf8(const FWString& WideStr);
	static FString ToString(const FWString& wstring);

	static FString Normalize(const FString& Path);
};


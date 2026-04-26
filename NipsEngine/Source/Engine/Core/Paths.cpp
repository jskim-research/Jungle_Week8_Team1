#include "Engine/Core/Paths.h"
#include <filesystem>

FWString FPaths::RootDir()
{
	static FWString Cached;
	if (Cached.empty())
	{
		WCHAR Buffer[MAX_PATH];
		GetModuleFileNameW(nullptr, Buffer, MAX_PATH);
		std::filesystem::path ExeDir = std::filesystem::path(Buffer).parent_path();

		// 1. 배포 환경: exe 파일과 같은 폴더에 바로 Shaders/ 가 있는 경우
		if (std::filesystem::exists(ExeDir / L"Shaders"))
		{
			Cached = ExeDir.generic_wstring() + L"/";
		}
		else
		{
			// 2. 개발 환경: 빌드 깊이에 무관하게 exe 상위를 루트까지 순회하며 탐색
			//    (Bin/ObjViewer/, x64/Release/, Bin/Debug/x64/ 등 깊이가 달라도 대응)
			bool bFound = false;
			std::filesystem::path SearchDir = ExeDir;

			while (SearchDir.has_parent_path())
			{
				SearchDir = SearchDir.parent_path();

				if (std::filesystem::exists(SearchDir / L"Shaders"))
				{
					Cached = SearchDir.generic_wstring() + L"/";
					bFound = true;
					break;
				}

				// 드라이브 루트까지 올라갔으면 중단
				if (SearchDir == SearchDir.root_path())
				{
					break;
				}
			}

			// 3. 어디서도 못 찾으면 CWD Fallback
			if (!bFound)
			{
				Cached = std::filesystem::current_path().generic_wstring() + L"/";
			}
		}
	}
	return Cached;
}

// 나머지 함수는 동일 ...

FWString FPaths::ShaderDir() { return RootDir() + L"Shaders/"; }
FWString FPaths::SceneDir() { return RootDir() + L"Asset/Scene/"; }
FWString FPaths::DumpDir() { return RootDir() + L"Saves/Dump/"; }
FWString FPaths::SettingsDir() { return RootDir() + L"Settings/"; }
FWString FPaths::ShaderFilePath() { return RootDir() + L"Shaders/ShaderW0.hlsl"; }
FWString FPaths::SettingsFilePath() { return RootDir() + L"Settings/Editor.ini"; }
FWString FPaths::ViewerSettingsFilePath() { return RootDir() + L"Settings/ObjViewer.ini"; }
FWString FPaths::AssetDirectoryPath() { return RootDir() + L"Asset"; }
FWString FPaths::ResourceDefaultMaterialTexture() { return RootDir() + L"Asset/Mesh/Default.png"; }


FWString FPaths::Combine(const FWString& Base, const FWString& Child)
{
	std::filesystem::path Result(Base);
	Result /= Child;
	return Result.generic_wstring(); // backslash를 slash로 변환해서 반환한다.
}

void FPaths::CreateDir(const FWString& Path)
{
	std::filesystem::create_directories(Path);
}

FWString FPaths::ToWide(const FString& Utf8Str)
{
	if (Utf8Str.empty()) return {};
	int32_t Size = MultiByteToWideChar(CP_UTF8, 0, Utf8Str.c_str(), -1, nullptr, 0);
	FWString Result(Size - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, Utf8Str.c_str(), -1, &Result[0], Size);
	return Result;
}

FString FPaths::ToUtf8(const FWString& WideStr)
{
	if (WideStr.empty()) return {};
	int32_t Size = WideCharToMultiByte(CP_UTF8, 0, WideStr.c_str(), static_cast<int>(WideStr.length()), nullptr, 0, nullptr, nullptr);
	FString Result(Size, '\0');
	WideCharToMultiByte(CP_UTF8, 0, WideStr.c_str(), static_cast<int>(WideStr.length()), Result.data(), Size, nullptr, nullptr);
	return Result;
}

FString FPaths::ToString(const FWString& wstring)
{
	return ToUtf8(wstring);
}

//	절대경로를 입력받아서 RootDir 기준의 상대 경로로 변환한다.
FWString FPaths::ToRelative(const FWString& AbsolutePath)
{
	if (AbsolutePath.empty()) return L"";

	std::filesystem::path AbsPath(AbsolutePath);
	std::filesystem::path Root(RootDir());
	
	//	RootDir 기준으로 상대 경로를 계산 (예: Asset/Scene/map.json)
	std::filesystem::path RelPath = std::filesystem::relative(AbsPath, Root);
	
	return RelPath.generic_wstring();
}

//	상대 경로를 입력받아서 RootDir 기준의 절대 경로로 변환한다.
FWString FPaths::ToAbsolute(const FWString& RelativePath)
{
	if (RelativePath.empty()) return L"";

	std::filesystem::path TargetPath(RelativePath);

	// 이미 C:/ 등 절대 경로라면 변환하지 않고 그대로 반환
	if (TargetPath.is_absolute())
	{
		return TargetPath.generic_wstring();
	}

	std::filesystem::path Root(RootDir());
	
	// 상대 경로라면 RootDir에 붙인 뒤, 불필요한 슬래시 등을 정리(lexically_normal)
	return (Root / TargetPath).lexically_normal().generic_wstring();
}

// 절대경로를 입력받아서 RootDir 기준의 상대 경로 string으로 변환한다.
FString FPaths::ToRelativeString(const FWString &AbsolutePath) 
{
	return ToUtf8(ToRelative(AbsolutePath));
}

// 상대 경로를 입력받아서 RootDir 기준의 절대 경로 string으로 변환한다.
FString FPaths::ToAbsoluteString(const FWString &RelativePath) 
{
	return ToUtf8(ToAbsolute(RelativePath));
}

FString FPaths::Normalize(const FString& Path)
{
	FWString WidePath = ToWide(Path);
	std::filesystem::path NormalizedPath(WidePath);
	FWString NormalizedWide = NormalizedPath.lexically_normal().generic_wstring();
	return ToUtf8(NormalizedWide);
}

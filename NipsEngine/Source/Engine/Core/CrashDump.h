#pragma once

struct _EXCEPTION_POINTERS;
using EXCEPTION_POINTERS = _EXCEPTION_POINTERS;

// SEH 필터 함수: __except() 안에서 GetExceptionInformation()과 함께 사용
// 크래시 발생 시 실행 파일 옆에 .dmp 파일을 생성합니다.
long __stdcall WriteCrashDump(EXCEPTION_POINTERS* ExceptionInfo);
void WriteCrashLog(EXCEPTION_POINTERS* ExceptionInfo);
int32 ReportCrash(EXCEPTION_POINTERS* ExceptionInfo);

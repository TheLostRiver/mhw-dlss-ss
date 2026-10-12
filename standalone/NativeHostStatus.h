#pragma once
#include <Windows.h>
#include <filesystem>
#include <iterator>
#include <string>

namespace mhwsr {
// Per-process status is separate from preferences. Verify process creation time
// so an old file cannot describe a later process that reuses the same PID.
inline std::wstring ProcessStartToken(HANDLE process) {
    FILETIME created{},ended{},kernel{},user{};
    if(!GetProcessTimes(process,&created,&ended,&kernel,&user))return {};
    ULARGE_INTEGER time{};time.LowPart=created.dwLowDateTime;time.HighPart=created.dwHighDateTime;
    return std::to_wstring(time.QuadPart);
}
inline std::filesystem::path NativeStatusPath(const std::filesystem::path& root,DWORD pid) {
    return root/(L"MhwNativeHost-"+std::to_wstring(pid)+L".state.ini");
}
inline bool WriteNativeStatus(const std::filesystem::path& path,const std::wstring& token,
                              const wchar_t* state,const wchar_t* reason) {
    if(path.empty()||token.empty())return false;
    std::wstring values;
    const auto append=[&](const wchar_t* key,const std::wstring& value){values+=key;values+=L'=';values+=value;values.push_back(L'\0');};
    append(L"Version",L"3");append(L"ProcessStart",token);append(L"State",state);append(L"Reason",reason);
    append(L"SuperResolutionActive",L"0");values.push_back(L'\0');
    return WritePrivateProfileSectionW(L"Runtime",values.c_str(),path.c_str())!=FALSE;
}
inline std::wstring NativeStatusField(const std::filesystem::path& path,const wchar_t* key) {
    wchar_t text[256]{};GetPrivateProfileStringW(L"Runtime",key,L"",text,DWORD(std::size(text)),path.c_str());return text;
}
}

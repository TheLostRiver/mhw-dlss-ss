// Launch the game and load this project's fixed standalone input host early.
// Failure leaves the game running; this launcher never terminates a game process.
#include "../readback/CaptureCommon.h"
#include <tlhelp32.h>
#include <stdexcept>

namespace {
struct Handle {HANDLE value=nullptr;~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
std::ofstream logFile;
void Log(const char* line){if(logFile){logFile<<line<<'\n';logFile.flush();}}
uintptr_t ModuleBase(DWORD pid,const std::wstring& name,const std::wstring* path=nullptr) {
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid)};
    if(snapshot.value==INVALID_HANDLE_VALUE)return 0;
    MODULEENTRY32W item{};item.dwSize=sizeof(item);
    for(BOOL ok=Module32FirstW(snapshot.value,&item);ok;ok=Module32NextW(snapshot.value,&item))
        if(!_wcsicmp(item.szModule,name.c_str())&&(!path||!_wcsicmp(item.szExePath,path->c_str())))return reinterpret_cast<uintptr_t>(item.modBaseAddr);
    return 0;
}
bool GameRunning() {
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0)};
    if(snapshot.value==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot check existing game processes");
    PROCESSENTRY32W item{};item.dwSize=sizeof(item);
    for(BOOL ok=Process32FirstW(snapshot.value,&item);ok;ok=Process32NextW(snapshot.value,&item)) {
        if(_wcsicmp(item.szExeFile,L"MonsterHunterWorld.exe"))continue;
        if(!item.cntThreads)continue; // Ignore already-ended process objects retained by old launcher handles.
        Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,item.th32ProcessID)};DWORD code=0;
        if(!process.value||!GetExitCodeProcess(process.value,&code)||code==STILL_ACTIVE)return true;
    }
    return false;
}
void LoadHost(HANDLE process,DWORD pid,const std::filesystem::path& dll) {
    const auto load=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");HMODULE owner=nullptr;
    if(!load||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(load),&owner))
        throw std::runtime_error("System loader owner unavailable");
    const auto ownerName=ModulePath(owner).filename().wstring();uintptr_t base=0;const auto deadline=GetTickCount64()+10000;
    while(!base&&GetTickCount64()<deadline){base=ModuleBase(pid,ownerName);if(base)break;DWORD code=0;if(!GetExitCodeProcess(process,&code)||code!=STILL_ACTIVE)break;Sleep(10);}
    if(!base)throw std::runtime_error("Game system loader did not become available");
    const auto remoteLoad=base+reinterpret_cast<uintptr_t>(load)-reinterpret_cast<uintptr_t>(owner);
    const auto path=dll.wstring();const SIZE_T bytes=(path.size()+1)*sizeof(wchar_t);
    void* allocation=VirtualAllocEx(process,nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!allocation)throw std::runtime_error("Host path allocation failed");
    SIZE_T written=0;
    if(!WriteProcessMemory(process,allocation,path.c_str(),bytes,&written)||written!=bytes){VirtualFreeEx(process,allocation,0,MEM_RELEASE);throw std::runtime_error("Host path write failed");}
    Handle thread{CreateRemoteThread(process,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteLoad),allocation,0,nullptr)};
    if(!thread.value){VirtualFreeEx(process,allocation,0,MEM_RELEASE);throw std::runtime_error("Host loader thread could not start");}
    if(WaitForSingleObject(thread.value,30000)!=WAIT_OBJECT_0) {
        // The live loader may still reference the path. Keep it and do not retry.
        throw std::runtime_error("Host loader timed out; game and pending loader left running");
    }
    VirtualFreeEx(process,allocation,0,MEM_RELEASE);
    if(!ModuleBase(pid,dll.filename().wstring(),&path))throw std::runtime_error("Standalone host not found after loading");
    Log("Standalone host loaded. See MhwNativeHost-<PID>.jsonl for readiness; SR is not enabled in this diagnostic build.");
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    try {
        const auto root=ModulePath(nullptr).parent_path();logFile.open(root/L"MhwSrLauncher.log",std::ios::app);
        const auto game=root/L"MonsterHunterWorld.exe",host=root/L"MhwNativeHost.dll";
        if(!std::filesystem::is_regular_file(game)||!std::filesystem::is_regular_file(host)||!std::filesystem::is_regular_file(root/L"MhwNativeMethods.ini"))
            throw std::runtime_error("Game, standalone host, or native method calibration is missing");
        if(GameRunning())throw std::runtime_error("The game is already running; exit normally before starting the standalone launcher");
        std::wstring command=L"\""+game.wstring()+L"\"";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION pi{};
        if(!CreateProcessW(game.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT,nullptr,root.c_str(),&startup,&pi))
            throw std::runtime_error("Game process creation failed");
        Handle process{pi.hProcess},thread{pi.hThread};
        logFile<<"Started game PID "<<pi.dwProcessId<<"\n";logFile.flush();LoadHost(process.value,pi.dwProcessId,host);return 0;
    }catch(const std::exception& e){Log(e.what());std::wstring message;for(const char c:std::string(e.what()))message.push_back(wchar_t(c));
        MessageBoxW(nullptr,message.c_str(),L"MHW SR standalone launcher",MB_OK|MB_ICONERROR);return 1;}
}

// Loads explicitly named research observers into a selected MHW process.
// Uses normal LoadLibraryW. No executable payload, remote code patch, or auto-start installation.
#include <Windows.h>
#include <TlHelp32.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    operator HANDLE() const { return value; }
};

uintptr_t RemoteModuleBase(DWORD pid, const wchar_t* basename, const std::wstring* exactPath = nullptr) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (snapshot.value == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot, &entry)) return 0;
    do {
        if (_wcsicmp(entry.szModule, basename) == 0 &&
            (!exactPath || _wcsicmp(entry.szExePath, exactPath->c_str()) == 0))
            return reinterpret_cast<uintptr_t>(entry.modBaseAddr);
    } while (Module32NextW(snapshot, &entry));
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::wcerr << L"Usage: MhwProbeInjector <PID> <absolute supported observer DLL path>\n"; return 2; }
    wchar_t* end = nullptr;
    const unsigned long pidNumber = wcstoul(argv[1], &end, 10);
    if (!pidNumber || !end || *end) return 2;
    const DWORD pid = static_cast<DWORD>(pidNumber);
    const auto path = std::filesystem::absolute(argv[2]);
    const auto observerName = path.filename().wstring();
    if (!std::filesystem::is_regular_file(path) ||
        (_wcsicmp(observerName.c_str(), L"MhwViewportProbe.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwViewportProbe_v2.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwTextureProbe.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwTextureProbe_v2.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwPassProbe.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwPassProbe_v2.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwScreenProbe.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwScreenProbe_v2.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwScreenProbe_v3.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwScreenProbe_v4.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwScalePilot.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwScalePilot_v2.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwScalePilot_v3.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwQuadPilot.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwSrBridge.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwSrBridge_r1.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwSrBridge_r2.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwSrBridge_r3.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwSrBridge_inputs.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwSrBridge_inputs_v2.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwSrBridge_post.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwSrBridge_vertices.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwSrBridge_vertices_v2.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwSrBridge_jitter.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwSrBridge_handoff.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwSrBridge_handoff_v2.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwSrBridge_textures.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwSrBridge_textures_v2.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwSrBridge_textures_v3.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwSrBridge_textures_v4.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwSrBridge_constants.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwSrBridge_depth.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwSrBridge_quality_v2.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwSrBridge_quality_v3.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwSrBridge_quality_v4.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwSrBridge_quality_v5.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwSrBridge_quality_v6.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwSrBridge_quality_v7.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwSrBridge_quality_v8.dll") != 0 && _wcsicmp(observerName.c_str(), L"MhwSrBridge_quality_v9.dll") != 0 &&
         _wcsicmp(observerName.c_str(), L"MhwSrBridge_quality_v10.dll") != 0)) {
        std::wcerr << L"Only explicitly named research observer DLLs are accepted.\n"; return 2;
    }
    Handle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
        PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD, FALSE, pid));
    if (!process.value) { std::wcerr << L"OpenProcess failed: " << GetLastError() << '\n'; return 1; }
    std::vector<wchar_t> executable(32768);
    DWORD length = static_cast<DWORD>(executable.size());
    if (!QueryFullProcessImageNameW(process, 0, executable.data(), &length) ||
        _wcsicmp(std::filesystem::path(executable.data()).filename().c_str(), L"MonsterHunterWorld.exe") != 0) {
        std::wcerr << L"The selected process is not MonsterHunterWorld.exe.\n"; return 1;
    }
    const std::wstring fullPath = path.wstring();
    if (RemoteModuleBase(pid, observerName.c_str(), &fullPath)) {
        std::wcout << L"This observer is already loaded; no second load requested.\n"; return 0;
    }
    const auto load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE owner = nullptr;
    if (!load || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(load), &owner)) return 1;
    std::vector<wchar_t> ownerPath(32768);
    if (!GetModuleFileNameW(owner, ownerPath.data(), static_cast<DWORD>(ownerPath.size()))) return 1;
    const auto basename = std::filesystem::path(ownerPath.data()).filename().wstring();
    const uintptr_t remoteOwner = RemoteModuleBase(pid, basename.c_str());
    if (!remoteOwner) { std::wcerr << L"Could not locate the matching system loader module.\n"; return 1; }
    const uintptr_t remoteLoad = remoteOwner + reinterpret_cast<uintptr_t>(load) - reinterpret_cast<uintptr_t>(owner);
    const SIZE_T bytes = (fullPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) return 1;
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, fullPath.c_str(), bytes, &written) || written != bytes) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE); return 1;
    }
    Handle thread(CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteLoad), remotePath, 0, nullptr));
    if (!thread.value) { VirtualFreeEx(process, remotePath, 0, MEM_RELEASE); return 1; }
    const DWORD wait = WaitForSingleObject(thread, 30000);
    if (wait != WAIT_OBJECT_0) {
        // The loader could still be reading the path; do not free memory out from under it.
        std::wcerr << L"Loader did not complete within 30 seconds; remote path retained. No second request was made.\n";
        return 1;
    }
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    const auto base = RemoteModuleBase(pid, observerName.c_str(), &fullPath);
    if (!base) { std::wcerr << L"The observer was not found in the module list after loading.\n"; return 1; }
    std::wcout << L"Observer loaded in PID " << pid << L" at 0x" << std::hex << base
        << L". Check its log for hook activation; a loaded module alone does not prove capture.\n";
    return 0;
}

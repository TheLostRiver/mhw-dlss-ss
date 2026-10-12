// Native control window. This process never injects, loads a render DLL, or
// changes game rendering. Only the panel's own settings are writable here.
#include <Windows.h>
#include <CommCtrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
constexpr UINT kTrayMessage=WM_APP+1,kShowMessage=WM_APP+2,kTimer=1;
constexpr int kHotkeyId=1,kSave=101,kReset=102,kHide=103,kQuit=104,kOpen=105;
constexpr WORD kDefaultHotkey=VK_END;
struct Handle {
    HANDLE value=nullptr;
    Handle()=default;
    explicit Handle(HANDLE h):value(h){}
    ~Handle(){Reset();}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    void Reset(HANDLE h=nullptr){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=h;}
};
HINSTANCE g_instance{};
HWND g_window{},g_title{},g_subtitle{},g_connection{},g_sr{},g_mode{},g_key{},g_notice{},g_startHidden{};
HWND g_lastGameWindow{};
HFONT g_font{},g_titleFont{};
HBRUSH g_background{};
Handle g_game,g_instanceMutex;
DWORD g_gamePid=0;
UINT g_dpi=96,g_taskbarCreated=0;
WORD g_hotkey=kDefaultHotkey;
bool g_registered=false,g_trayAdded=false,g_auto=false,g_seenGame=false,g_quitting=false;
bool g_waitKeyRelease=false,g_topmost=true;
uint64_t g_nextGameScan=0,g_retryHotkeyAt=0,g_launchTime=0;
std::filesystem::path g_root,g_ini;
std::wstring g_className,g_noticeText;
struct Layout {HWND control;int x,y,w,h;bool title=false;};
std::vector<Layout> g_layout;

int Dip(int value){return MulDiv(value,int(g_dpi),96);}
std::filesystem::path OwnFolder() {
    std::array<wchar_t,32768> path{};const DWORD size=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
    return size&&size<path.size()?std::filesystem::path(path.data()).parent_path():std::filesystem::path();
}
std::wstring InstanceName(const std::filesystem::path& root) {
    auto value=root.wstring();CharLowerBuffW(value.data(),DWORD(value.size()));
    uint64_t hash=14695981039346656037ull;
    for(wchar_t ch:value){hash^=uint16_t(ch);hash*=1099511628211ull;}
    return L"MhwSrPanel."+std::to_wstring(hash);
}
bool ValidHotkey(WORD hotkey) {
    const BYTE key=LOBYTE(hotkey),flags=HIBYTE(hotkey);
    if(!key||(flags&~(HOTKEYF_CONTROL|HOTKEYF_ALT|HOTKEYF_SHIFT|HOTKEYF_EXT)))return false;
    // F8 is still handled directly by the native capture host. F12 belongs to
    // the debugger; Esc is kept as the panel's close shortcut.
    if(key==VK_F8||key==VK_F12||key==VK_ESCAPE||key==VK_LWIN||key==VK_RWIN||
       key==VK_SHIFT||key==VK_CONTROL||key==VK_MENU||key==VK_TAB||key==VK_RETURN||key==VK_SPACE)return false;
    if(flags&(HOTKEYF_CONTROL|HOTKEYF_ALT))return true;
    return (key>=VK_F1&&key<=VK_F24)||key==VK_END||key==VK_HOME||key==VK_INSERT||
        key==VK_DELETE||key==VK_PRIOR||key==VK_NEXT||key==VK_PAUSE||key==VK_SCROLL;
}
UINT Modifiers(WORD key) {
    const BYTE flags=HIBYTE(key);return MOD_NOREPEAT|((flags&HOTKEYF_CONTROL)?MOD_CONTROL:0)|
        ((flags&HOTKEYF_ALT)?MOD_ALT:0)|((flags&HOTKEYF_SHIFT)?MOD_SHIFT:0);
}
std::wstring KeyName(WORD key) {
    std::wstring name;const BYTE flags=HIBYTE(key);
    if(flags&HOTKEYF_CONTROL)name+=L"Ctrl + ";if(flags&HOTKEYF_ALT)name+=L"Alt + ";if(flags&HOTKEYF_SHIFT)name+=L"Shift + ";
    UINT scan=MapVirtualKeyW(LOBYTE(key),MAPVK_VK_TO_VSC_EX);
    LONG parameter=LONG((scan&255)<<16);if((scan&0xff00)||(flags&HOTKEYF_EXT))parameter|=1<<24;
    wchar_t text[96]{};
    if(GetKeyNameTextW(parameter,text,int(std::size(text))))name+=text;else name+=L"Key "+std::to_wstring(LOBYTE(key));
    return name;
}
void SetText(HWND window,const std::wstring& text) {
    const int size=GetWindowTextLengthW(window);std::wstring current(size+1,L'\0');
    GetWindowTextW(window,current.data(),int(current.size()));current.resize(size);
    if(current!=text)SetWindowTextW(window,text.c_str());
}
void Notice(const std::wstring& text) {g_noticeText=text;if(g_notice)SetText(g_notice,text);}
void UnregisterToggle(){if(g_registered){UnregisterHotKey(g_window,kHotkeyId);g_registered=false;}}
bool PanelForeground() {return GetAncestor(GetForegroundWindow(),GA_ROOT)==g_window;}
bool GameForeground() {DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);return g_gamePid&&pid==g_gamePid;}
bool EditingKey() {return PanelForeground()&&GetFocus()==g_key;}
bool RegisterToggle(WORD value) {
    if(!RegisterHotKey(g_window,kHotkeyId,Modifiers(value),LOBYTE(value)))return false;
    g_registered=true;return true;
}
void RefreshHotkey() {
    if((!PanelForeground()&&!GameForeground())||EditingKey()){UnregisterToggle();return;}
    if(g_waitKeyRelease){
        if(GetAsyncKeyState(LOBYTE(g_hotkey))&0x8000)return;
        g_waitKeyRelease=false;
    }
    if(!g_registered&&GetTickCount64()>=g_retryHotkeyAt){
        if(!RegisterToggle(g_hotkey)){
            g_retryHotkeyAt=GetTickCount64()+2000;
            Notice(L"快捷键被占用，请更换；也可双击托盘图标打开面板。");
        }
    }
}
void RefreshTray() {
    NOTIFYICONDATAW icon{};icon.cbSize=sizeof(icon);icon.hWnd=g_window;icon.uID=1;
    icon.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;icon.uCallbackMessage=kTrayMessage;
    icon.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    const auto tip=L"MHW SR · "+KeyName(g_hotkey)+L" 显示 / 隐藏";
    wcsncpy_s(icon.szTip,tip.c_str(),_TRUNCATE);
    g_trayAdded=Shell_NotifyIconW(g_trayAdded?NIM_MODIFY:NIM_ADD,&icon)!=FALSE;
}
void ShowPanel() {
    if(GameForeground())g_lastGameWindow=GetForegroundWindow();
    ShowWindow(g_window,SW_SHOWNORMAL);SetWindowPos(g_window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
    g_topmost=true;
    SetForegroundWindow(g_window);SetFocus(GetDlgItem(g_window,kHide));g_retryHotkeyAt=0;RefreshHotkey();
}
void HidePanel() {
    if(!g_trayAdded){RefreshTray();if(!g_trayAdded){Notice(L"托盘不可用，面板保持打开。可点击“退出面板”关闭。");return;}}
    const bool hadFocus=PanelForeground();ShowWindow(g_window,SW_HIDE);UnregisterToggle();
    DWORD pid=0;if(IsWindow(g_lastGameWindow))GetWindowThreadProcessId(g_lastGameWindow,&pid);
    if(hadFocus&&g_gamePid&&pid==g_gamePid)SetForegroundWindow(g_lastGameWindow);
    RefreshHotkey();
}
void SaveSettings(WORD candidate,bool reset=false) {
    if(!ValidHotkey(candidate)){Notice(L"请选择 End / Insert / 功能键，或 Ctrl、Alt 组合；F8、F12、Esc 不可用。");return;}
    const WORD previous=g_hotkey;UnregisterToggle();
    if(!RegisterToggle(candidate)){
        RegisterToggle(previous);Notice(L"此快捷键已被占用，原快捷键仍保留。");return;
    }
    const auto encoded=std::to_wstring(candidate);
    if(!WritePrivateProfileStringW(L"UI",L"ToggleHotkey",encoded.c_str(),g_ini.c_str())){
        UnregisterToggle();RegisterToggle(previous);Notice(L"保存失败，请检查面板目录是否可写。原快捷键仍保留。");return;
    }
    g_hotkey=candidate;if(reset)SendMessageW(g_key,HKM_SETHOTKEY,candidate,0);
    const bool hidden=SendMessageW(g_startHidden,BM_GETCHECK,0,0)==BST_CHECKED;
    const bool saved=WritePrivateProfileStringW(L"UI",L"StartHidden",hidden?L"1":L"0",g_ini.c_str())!=FALSE;
    RefreshTray();Notice(L"已保存："+KeyName(g_hotkey)+(saved?L"。立即生效，下次启动保留。":L"。启动显示选项保存失败。"));
    SetFocus(GetDlgItem(g_window,kSave));RefreshHotkey();
}
bool SameGame(HANDLE process) {
    std::array<wchar_t,32768> path{};DWORD count=DWORD(path.size());
    return QueryFullProcessImageNameW(process,0,path.data(),&count)&&
        !_wcsicmp(path.data(),(g_root/L"MonsterHunterWorld.exe").c_str());
}
void FindGame() {
    if(g_game.value){
        if(WaitForSingleObject(g_game.value,0)==WAIT_TIMEOUT)return;
        g_game.Reset();g_gamePid=0;g_lastGameWindow=nullptr;
        if(g_auto&&g_seenGame){g_quitting=true;DestroyWindow(g_window);return;}
    }
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));if(snapshot.value==INVALID_HANDLE_VALUE)return;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
    for(BOOL ok=Process32FirstW(snapshot.value,&entry);ok;ok=Process32NextW(snapshot.value,&entry)){
        if(!entry.cntThreads||_wcsicmp(entry.szExeFile,L"MonsterHunterWorld.exe"))continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,entry.th32ProcessID));
        if(process.value&&WaitForSingleObject(process.value,0)==WAIT_TIMEOUT&&SameGame(process.value)){
            g_game.Reset(process.value);process.value=nullptr;g_gamePid=entry.th32ProcessID;g_seenGame=true;break;
        }
    }
}
void RefreshConnection() {
    if(!g_gamePid){SetText(g_connection,L"游戏未运行");return;}
    const auto name=L"Local\\MhwNativeHost."+std::to_wstring(g_gamePid)+L".Start";
    Handle ready(OpenEventW(SYNCHRONIZE,FALSE,name.c_str()));
    if(ready.value)SetText(g_connection,L"独立入口已连接");
    else if(!GetPrivateProfileIntW(L"Standalone",L"Enabled",0,(g_root/L"MhwNativeHost.ini").c_str()))SetText(g_connection,L"独立入口未启用或已停止");
    else SetText(g_connection,L"游戏已启动，等待独立入口");
}
void MakeFonts() {
    HFONT font=CreateFontW(-Dip(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    HFONT title=CreateFontW(-Dip(25),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    for(const auto& item:g_layout){
        SendMessageW(item.control,WM_SETFONT,reinterpret_cast<WPARAM>(item.title?title:font),TRUE);
        SetWindowPos(item.control,nullptr,Dip(item.x),Dip(item.y),Dip(item.w),Dip(item.h),SWP_NOZORDER|SWP_NOACTIVATE);
    }
    if(g_font)DeleteObject(g_font);if(g_titleFont)DeleteObject(g_titleFont);g_font=font;g_titleFont=title;
}
LRESULT CALLBACK HotkeyProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR) {
    if(message==WM_SETFOCUS)UnregisterToggle();
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,HotkeyProc,1);
    return DefSubclassProc(window,message,wparam,lparam);
}
HWND Control(const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id=0,bool title=false) {
    HWND control=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,Dip(x),Dip(y),Dip(w),Dip(h),g_window,
        reinterpret_cast<HMENU>(INT_PTR(id)),g_instance,nullptr);
    if(control)g_layout.push_back({control,x,y,w,h,title});return control;
}
void BuildControls() {
    g_title=Control(L"STATIC",L"MHW SR",0,24,18,496,38,0,true);
    g_subtitle=Control(L"STATIC",L"超分辨率控制面板",0,24,60,496,24);
    Control(L"STATIC",L"连接状态",0,24,108,125,24);
    g_connection=Control(L"STATIC",L"等待游戏",0,158,108,360,24);
    Control(L"STATIC",L"DLSS 超分辨率",0,24,145,130,24);
    g_sr=Control(L"STATIC",L"尚未接通",0,158,145,360,24);
    Control(L"STATIC",L"超分档位",0,24,182,125,24);
    g_mode=Control(WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_TABSTOP,158,178,362,180);
    for(const auto* value:{L"待独立 SR 接通后开放",L"质量",L"平衡",L"性能",L"超高性能"})SendMessageW(g_mode,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(value));
    SendMessageW(g_mode,CB_SETCURSEL,0,0);EnableWindow(g_mode,FALSE);
    Control(L"STATIC",L"当前版本用于输入与投影采集。F8 不会开启超分。",0,24,223,496,25);
    Control(L"STATIC",L"显示 / 隐藏面板",0,24,275,496,24);
    g_key=Control(HOTKEY_CLASSW,L"",WS_TABSTOP,24,307,252,32);
    SetWindowSubclass(g_key,HotkeyProc,1,0);
    SendMessageW(g_key,HKM_SETRULES,0,0);SendMessageW(g_key,HKM_SETHOTKEY,g_hotkey,0);
    Control(L"BUTTON",L"保存",WS_TABSTOP|BS_PUSHBUTTON,288,307,92,32,kSave);
    Control(L"BUTTON",L"恢复 End",WS_TABSTOP|BS_PUSHBUTTON,392,307,128,32,kReset);
    Control(L"STATIC",L"点击输入框，按下新组合键，再点保存。",0,24,350,496,23);
    g_startHidden=Control(L"BUTTON",L"随游戏启动时隐藏面板",BS_AUTOCHECKBOX|WS_TABSTOP,24,382,496,28);
    SendMessageW(g_startHidden,BM_SETCHECK,GetPrivateProfileIntW(L"UI",L"StartHidden",1,g_ini.c_str())?BST_CHECKED:BST_UNCHECKED,0);
    g_notice=Control(L"STATIC",L"",0,24,421,496,48);
    Control(L"BUTTON",L"退出面板",WS_TABSTOP|BS_PUSHBUTTON,280,484,112,34,kQuit);
    Control(L"BUTTON",L"隐藏面板",WS_TABSTOP|BS_DEFPUSHBUTTON,404,484,116,34,kHide);
    MakeFonts();Notice(L"当前快捷键："+KeyName(g_hotkey)+L"。关闭窗口会收起到托盘。");
}
void TrayMenu() {
    HMENU menu=CreatePopupMenu();if(!menu)return;
    AppendMenuW(menu,MF_STRING,kOpen,L"打开 MHW SR");AppendMenuW(menu,MF_STRING,kQuit,L"退出面板");
    POINT point{};GetCursorPos(&point);SetForegroundWindow(g_window);
    const auto chosen=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,g_window,nullptr);
    DestroyMenu(menu);PostMessageW(g_window,WM_NULL,0,0);
    if(chosen==kOpen)ShowPanel();else if(chosen==kQuit)DestroyWindow(g_window);
}
LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    switch(message){
    case WM_CREATE:g_window=window;g_dpi=GetDpiForWindow(window);BuildControls();RefreshTray();SetTimer(window,kTimer,50,nullptr);return 0;
    case WM_TIMER:
        if(GetTickCount64()>=g_nextGameScan){g_nextGameScan=GetTickCount64()+1000;FindGame();if(g_quitting)return 0;RefreshConnection();
            if(g_auto&&!g_seenGame&&GetTickCount64()-g_launchTime>60000){DestroyWindow(window);return 0;}}
        if(GameForeground())g_lastGameWindow=GetForegroundWindow();
        if(IsWindowVisible(window)){
            const bool onTop=GameForeground()||PanelForeground();
            if(g_topmost!=onTop){SetWindowPos(window,onTop?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,
                SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);g_topmost=onTop;}}
        RefreshHotkey();return 0;
    case WM_HOTKEY:
        // Recheck scope: registration may outlive a foreground change by one tick.
        if(wparam==kHotkeyId&&!EditingKey()&&(GameForeground()||PanelForeground())){
            // Unregistering/re-registering on a foreground transition must not
            // turn a held key into a second toggle, even with MOD_NOREPEAT.
            g_waitKeyRelease=true;UnregisterToggle();
            if(IsWindowVisible(window)&&!IsIconic(window))HidePanel();else ShowPanel();}
        return 0;
    case WM_COMMAND:
        switch(LOWORD(wparam)){
        case kSave:SaveSettings(WORD(SendMessageW(g_key,HKM_GETHOTKEY,0,0)));break;
        case kReset:SaveSettings(kDefaultHotkey,true);break;
        case kHide:HidePanel();break;
        case kQuit:DestroyWindow(window);break;
        case IDCANCEL:HidePanel();break;
        default:break;}
        return 0;
    case WM_CLOSE:HidePanel();return 0;
    case WM_ACTIVATE:RefreshHotkey();return 0;
    case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:{
        HDC dc=reinterpret_cast<HDC>(wparam);SetBkColor(dc,RGB(247,249,252));SetTextColor(dc,RGB(35,45,61));
        if(reinterpret_cast<HWND>(lparam)==g_subtitle)SetTextColor(dc,RGB(95,105,121));
        return reinterpret_cast<LRESULT>(g_background);}
    case WM_ERASEBKGND:{RECT rect{};GetClientRect(window,&rect);FillRect(reinterpret_cast<HDC>(wparam),&rect,g_background);return 1;}
    case WM_DPICHANGED:{g_dpi=HIWORD(wparam);MakeFonts();const auto* rect=reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
    case kTrayMessage:
        if(lparam==WM_LBUTTONDBLCLK||lparam==NIN_KEYSELECT)ShowPanel();
        else if(lparam==WM_RBUTTONUP||lparam==WM_CONTEXTMENU)TrayMenu();return 0;
    case kShowMessage:ShowPanel();return 0;
    case WM_DESTROY:{
        g_quitting=true;KillTimer(window,kTimer);UnregisterToggle();
        NOTIFYICONDATAW icon{};icon.cbSize=sizeof(icon);icon.hWnd=window;icon.uID=1;Shell_NotifyIconW(NIM_DELETE,&icon);
        PostQuitMessage(0);return 0;}
    default:if(message==g_taskbarCreated){g_trayAdded=false;RefreshTray();return 0;}break;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR command,int) {
    g_instance=instance;g_auto=std::wstring(command)==L"--auto";g_launchTime=GetTickCount64();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_root=OwnFolder();if(g_root.empty())return 1;g_ini=g_root/L"MhwSrPanel.ini";g_className=InstanceName(g_root);
    const auto mutex=CreateMutexW(nullptr,FALSE,(L"Local\\"+g_className).c_str());const auto mutexError=GetLastError();
    g_instanceMutex.Reset(mutex);
    if(!g_instanceMutex.value)return 1;
    if(mutexError==ERROR_ALREADY_EXISTS){if(!g_auto){const auto previous=FindWindowW(g_className.c_str(),nullptr);
            if(previous)PostMessageW(previous,kShowMessage,0,0);}return 0;}
    const auto configured=GetPrivateProfileIntW(L"UI",L"ToggleHotkey",kDefaultHotkey,g_ini.c_str());
    if(configured<=0xffff&&ValidHotkey(WORD(configured)))g_hotkey=WORD(configured);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_HOTKEY_CLASS|ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
    g_background=CreateSolidBrush(RGB(247,249,252));g_taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.hInstance=instance;cls.lpfnWndProc=WindowProc;cls.lpszClassName=g_className.c_str();
    cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(nullptr,IDI_APPLICATION);cls.hbrBackground=g_background;
    if(!RegisterClassExW(&cls))return 1;
    const DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
    const DWORD extra=WS_EX_TOPMOST|WS_EX_CONTROLPARENT;
    g_dpi=GetDpiForSystem();RECT size{0,0,Dip(544),Dip(542)};AdjustWindowRectExForDpi(&size,style,FALSE,extra,g_dpi);
    HWND window=CreateWindowExW(extra,cls.lpszClassName,L"MHW SR",style,CW_USEDEFAULT,CW_USEDEFAULT,
        size.right-size.left,size.bottom-size.top,nullptr,nullptr,instance,nullptr);
    if(!window)return 1;
    if(!g_auto||!GetPrivateProfileIntW(L"UI",L"StartHidden",1,g_ini.c_str())||!g_trayAdded)ShowPanel();
    MSG message{};
    while(GetMessageW(&message,nullptr,0,0)>0){
        // The hotkey editor must receive Tab/Enter/Esc itself; elsewhere normal
        // dialog navigation and Escape-to-hide are available.
        if(EditingKey()){UnregisterToggle();TranslateMessage(&message);DispatchMessageW(&message);}
        else if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
    }
    if(g_font)DeleteObject(g_font);if(g_titleFont)DeleteObject(g_titleFont);if(g_background)DeleteObject(g_background);
    return 0;
}

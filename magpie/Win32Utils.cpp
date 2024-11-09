#include "pch.h"
#include "Win32Utils.h"
#include "Logger.h"
#include "StrUtils.h"
#include "com_ptr.h"
#include <io.h>
#include <Psapi.h>
#include <winternl.h>
#include <dwmapi.h>

UINT Win32Utils::GetWindowShowCmd(HWND hWnd) noexcept {
    assert(hWnd != NULL);

    WINDOWPLACEMENT wp{ .length = sizeof(wp) };
    if (!GetWindowPlacement(hWnd, &wp)) {
        Logger::Get().Win32Error("GetWindowPlacement 出错");
    }

    return wp.showCmd;
}

bool Win32Utils::GetClientScreenRect(HWND hWnd, RECT& rect) noexcept {
    if (!GetClientRect(hWnd, &rect)) {
        Logger::Get().Win32Error("GetClientRect 出错");
        return false;
    }

    POINT p{};
    if (!ClientToScreen(hWnd, &p)) {
        Logger::Get().Win32Error("ClientToScreen 出错");
        return false;
    }

    rect.bottom += p.y;
    rect.left += p.x;
    rect.right += p.x;
    rect.top += p.y;

    return true;
}

bool Win32Utils::GetWindowFrameRect(HWND hWnd, RECT& rect) noexcept {
    HRESULT hr = DwmGetWindowAttribute(hWnd,
        DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect));
    if (FAILED(hr)) {
        Logger::Get().ComError("DwmGetWindowAttribute 失败", hr);
        return false;
    }

    // Win11 中最大化的窗口的 extended frame bounds 有一部分在屏幕外面，
    // 不清楚 Win10 是否有这种情况
    if (GetWindowShowCmd(hWnd) == SW_SHOWMAXIMIZED) {
        HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ .cbSize = sizeof(mi) };
        if (!GetMonitorInfo(hMon, &mi)) {
            Logger::Get().Win32Error("GetMonitorInfo 失败");
            return false;
        }

        // 不能裁剪到工作区，因为窗口可以使用 SetWindowPos 以任意尺寸显示“最大化”
        // 的窗口，缩放窗口就使用了这个技术以和 Wallpaper Engine 兼容。OS 虽然不
        // 会阻止跨越多个屏幕，但只在一个屏幕上有画面，因此可以认为最大化的窗口只在
        // 一个屏幕上。
        // 注意 Win11 中最大化窗口的 extended frame bounds 包含了下边框，但对我们
        // 没有影响，因为缩放时下边框始终会被裁剪掉。
        IntersectRect(&rect, &rect, &mi.rcMonitor);
    }

    // 对于使用 SetWindowRgn 自定义形状的窗口，裁剪到最小矩形边框
    RECT rgnRect;
    int regionType = GetWindowRgnBox(hWnd, &rgnRect);
    if (regionType == SIMPLEREGION || regionType == COMPLEXREGION) {
        RECT windowRect;
        if (!GetWindowRect(hWnd, &windowRect)) {
            Logger::Get().Win32Error("GetWindowRect 失败");
            return false;
        }

        // 转换为屏幕坐标
        OffsetRect(&rgnRect, windowRect.left, windowRect.top);

        IntersectRect(&rect, &rect, &rgnRect);
    }

    return true;
}

bool Win32Utils::ReadFile(const wchar_t* fileName, std::vector<uint8_t>& result) noexcept {
    Logger::Get().Info(StrUtils::Concat("读取文件: ", StrUtils::UTF16ToUTF8(fileName)));

    CREATEFILE2_EXTENDED_PARAMETERS extendedParams{
        .dwSize = sizeof(CREATEFILE2_EXTENDED_PARAMETERS),
        .dwFileAttributes = FILE_ATTRIBUTE_NORMAL,
        .dwFileFlags = FILE_FLAG_SEQUENTIAL_SCAN,
        .dwSecurityQosFlags = SECURITY_ANONYMOUS
    };

    unique_handle hFile(CreateFile2(fileName, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &extendedParams));

    if (!hFile) {
        Logger::Get().Error("打开文件失败");
        return false;
    }

    DWORD size = GetFileSize(hFile.get(), nullptr);
    result.resize(size);

    DWORD readed;
    if (!::ReadFile(hFile.get(), result.data(), size, &readed, nullptr)) {
        Logger::Get().Error("读取文件失败");
        return false;
    }

    return true;
}

bool Win32Utils::ReadTextFile(const wchar_t* fileName, std::string& result) noexcept {
    unique_file hFile;
    if (_wfopen_s(hFile.put(), fileName, L"rt") || !hFile) {
        Logger::Get().Error(StrUtils::Concat("打开文件 ", StrUtils::UTF16ToUTF8(fileName), " 失败"));
        return false;
    }

    // 获取文件长度
    int fd = _fileno(hFile.get());
    long size = _filelength(fd);

    result.clear();
    result.resize(static_cast<size_t>(size) + 1, 0);

    size_t readed = fread(result.data(), 1, size, hFile.get());
    result.resize(readed);

    return true;
}

bool Win32Utils::WriteFile(const wchar_t* fileName, const void* buffer, size_t bufferSize) noexcept {
    unique_file hFile;
    if (_wfopen_s(hFile.put(), fileName, L"wb") || !hFile) {
        Logger::Get().Error(StrUtils::Concat("打开文件 ", StrUtils::UTF16ToUTF8(fileName), " 失败"));
        return false;
    }

    if (bufferSize > 0) {
        [[maybe_unused]] size_t writed = fwrite(buffer, 1, bufferSize, hFile.get());
        assert(writed == bufferSize);
    }

    return true;
}

bool Win32Utils::WriteTextFile(const wchar_t* fileName, std::string_view text) noexcept {
    unique_file hFile;
    if (_wfopen_s(hFile.put(), fileName, L"wt") || !hFile) {
        Logger::Get().Error(StrUtils::Concat("打开文件 ", StrUtils::UTF16ToUTF8(fileName), " 失败"));
        return false;
    }

    fwrite(text.data(), 1, text.size(), hFile.get());
    return true;
}

const Win32Utils::OSVersion& Win32Utils::GetOSVersion() noexcept {
    static OSVersion version = []() -> OSVersion {
        HMODULE hNtDll = GetModuleHandle(L"ntdll.dll");
        assert(hNtDll);

        auto rtlGetVersion = (LONG(WINAPI*)(PRTL_OSVERSIONINFOW))GetProcAddress(hNtDll, "RtlGetVersion");
        if (!rtlGetVersion) {
            Logger::Get().Win32Error("获取 RtlGetVersion 地址失败");
            assert(false);
            return {};
        }

        RTL_OSVERSIONINFOW versionInfo{ .dwOSVersionInfoSize = sizeof(versionInfo) };
        rtlGetVersion(&versionInfo);

        return { versionInfo.dwMajorVersion, versionInfo.dwMinorVersion, versionInfo.dwBuildNumber };
    }();

    return version;
}

struct TPContext {
    std::function<void(uint32_t)> func;
    std::atomic<uint32_t> id;
};

#pragma warning(push)
#pragma warning(disable: 4505)	// 已删除具有内部链接的未引用函数

static void CALLBACK TPCallback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_WORK) {
    TPContext* ctxt = (TPContext*)context;
    const uint32_t id = ctxt->id.fetch_add(1, std::memory_order_relaxed) + 1;
    ctxt->func(id);
}

#pragma warning(pop) 

void Win32Utils::RunParallel(std::function<void(uint32_t)> func, uint32_t times) noexcept {
#ifdef _DEBUG
    // 为了便于调试，DEBUG 模式下不使用线程池
    for (UINT i = 0; i < times; ++i) {
        func(i);
    }
#else
    if (times == 0) {
        return;
    }

    if (times == 1) {
        return func(0);
    }

    TPContext ctxt = { func, 0 };
    PTP_WORK work = CreateThreadpoolWork(TPCallback, &ctxt, nullptr);
    if (work) {
        // 在线程池中执行 times - 1 次
        for (uint32_t i = 1; i < times; ++i) {
            SubmitThreadpoolWork(work);
        }

        func(0);

        WaitForThreadpoolWorkCallbacks(work, FALSE);
        CloseThreadpoolWork(work);
    } else {
        Logger::Get().Win32Error("CreateThreadpoolWork 失败，回退到单线程");

        // 回退到单线程
        for (uint32_t i = 0; i < times; ++i) {
            func(i);
        }
    }
#endif // _DEBUG
}

bool Win32Utils::SetForegroundWindow(HWND hWnd) noexcept {
    if (::SetForegroundWindow(hWnd)) {
        return true;
    }

    // 有多种原因会导致 SetForegroundWindow 失败，因此使用一个 trick 强制切换前台窗口
    // 来自 https://pinvoke.net/default.aspx/user32.SetForegroundWindow
    DWORD foreThreadId = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD curThreadId = GetCurrentThreadId();

    if (foreThreadId != curThreadId) {
        if (!AttachThreadInput(foreThreadId, curThreadId, TRUE)) {
            Logger::Get().Win32Error("AttachThreadInput 失败");
            return false;
        }
        BringWindowToTop(hWnd);
        ShowWindow(hWnd, SW_SHOW);
        AttachThreadInput(foreThreadId, curThreadId, FALSE);
    } else {
        BringWindowToTop(hWnd);
        ShowWindow(hWnd, SW_SHOW);
    }

    return true;
}

static bool MapKeycodeToUnicode(
    const int vCode,
    HKL layout,
    const BYTE* keyState,
    std::array<wchar_t, 3>& outBuffer
) noexcept {
    // Get the scan code from the virtual key code
    const UINT scanCode = MapVirtualKeyEx(vCode, MAPVK_VK_TO_VSC, layout);
    // Get the unicode representation from the virtual key code and scan code pair
    const int result = ToUnicodeEx(vCode, scanCode, keyState, outBuffer.data(), (int)outBuffer.size(), 0, layout);
    return result != 0;
}

static const std::array<std::wstring, 256>& GetKeyNames() noexcept {
    // 取自 https://github.com/microsoft/PowerToys/blob/fa3a5f80a113568155d9c2dbbcea8af16e15afa1/src/common/interop/keyboard_layout.cpp#L63
    static HKL previousLayout = 0;
    static std::array<std::wstring, 256> keyboardLayoutMap;

    // Get keyboard layout for current thread
    const HKL layout = GetKeyboardLayout(0);
    if (layout == previousLayout && !keyboardLayoutMap[0].empty()) {
        return keyboardLayoutMap;
    }
    previousLayout = layout;

    // 0 为非法
    keyboardLayoutMap[0] = L"Undefined";

    std::array<BYTE, 256> btKeys = { 0 };
    // Only set the Caps Lock key to on for the key names in uppercase
    btKeys[VK_CAPITAL] = 1;

    // Iterate over all the virtual key codes. virtual key 0 is not used
    for (int i = 1; i < 256; i++) {
        std::array<wchar_t, 3> szBuffer = { 0 };
        if (MapKeycodeToUnicode(i, layout, btKeys.data(), szBuffer)) {
            keyboardLayoutMap[i] = szBuffer.data();
            continue;
        }

        // Store the virtual key code as string
        std::wstring vk = L"VK ";
        vk += std::to_wstring(i);
        keyboardLayoutMap[i] = vk;
    }

    // Override special key names like Shift, Ctrl etc because they don't have unicode mappings and key names like Enter, Space as they appear as "\r", " "
    // To do: localization
    keyboardLayoutMap[VK_CANCEL] = L"Break";
    keyboardLayoutMap[VK_BACK] = L"Backspace";
    keyboardLayoutMap[VK_TAB] = L"Tab";
    keyboardLayoutMap[VK_CLEAR] = L"Clear";
    keyboardLayoutMap[VK_RETURN] = L"Enter";
    keyboardLayoutMap[VK_SHIFT] = L"Shift";
    keyboardLayoutMap[VK_CONTROL] = L"Ctrl";
    keyboardLayoutMap[VK_MENU] = L"Alt";
    keyboardLayoutMap[VK_PAUSE] = L"Pause";
    keyboardLayoutMap[VK_CAPITAL] = L"Caps Lock";
    keyboardLayoutMap[VK_ESCAPE] = L"Esc";
    keyboardLayoutMap[VK_SPACE] = L"Space";
    keyboardLayoutMap[VK_PRIOR] = L"PgUp";
    keyboardLayoutMap[VK_NEXT] = L"PgDn";
    keyboardLayoutMap[VK_END] = L"End";
    keyboardLayoutMap[VK_HOME] = L"Home";
    keyboardLayoutMap[VK_LEFT] = L"Left";
    keyboardLayoutMap[VK_UP] = L"Up";
    keyboardLayoutMap[VK_RIGHT] = L"Right";
    keyboardLayoutMap[VK_DOWN] = L"Down";
    keyboardLayoutMap[VK_SELECT] = L"Select";
    keyboardLayoutMap[VK_PRINT] = L"Print";
    keyboardLayoutMap[VK_EXECUTE] = L"Execute";
    keyboardLayoutMap[VK_SNAPSHOT] = L"Print Screen";
    keyboardLayoutMap[VK_INSERT] = L"Insert";
    keyboardLayoutMap[VK_DELETE] = L"Delete";
    keyboardLayoutMap[VK_HELP] = L"Help";
    keyboardLayoutMap[VK_LWIN] = L"Win (Left)";
    keyboardLayoutMap[VK_RWIN] = L"Win (Right)";
    keyboardLayoutMap[VK_APPS] = L"Apps/Menu";
    keyboardLayoutMap[VK_SLEEP] = L"Sleep";
    keyboardLayoutMap[VK_NUMPAD0] = L"NumPad 0";
    keyboardLayoutMap[VK_NUMPAD1] = L"NumPad 1";
    keyboardLayoutMap[VK_NUMPAD2] = L"NumPad 2";
    keyboardLayoutMap[VK_NUMPAD3] = L"NumPad 3";
    keyboardLayoutMap[VK_NUMPAD4] = L"NumPad 4";
    keyboardLayoutMap[VK_NUMPAD5] = L"NumPad 5";
    keyboardLayoutMap[VK_NUMPAD6] = L"NumPad 6";
    keyboardLayoutMap[VK_NUMPAD7] = L"NumPad 7";
    keyboardLayoutMap[VK_NUMPAD8] = L"NumPad 8";
    keyboardLayoutMap[VK_NUMPAD9] = L"NumPad 9";
    keyboardLayoutMap[VK_SEPARATOR] = L"Separator";
    keyboardLayoutMap[VK_F1] = L"F1";
    keyboardLayoutMap[VK_F2] = L"F2";
    keyboardLayoutMap[VK_F3] = L"F3";
    keyboardLayoutMap[VK_F4] = L"F4";
    keyboardLayoutMap[VK_F5] = L"F5";
    keyboardLayoutMap[VK_F6] = L"F6";
    keyboardLayoutMap[VK_F7] = L"F7";
    keyboardLayoutMap[VK_F8] = L"F8";
    keyboardLayoutMap[VK_F9] = L"F9";
    keyboardLayoutMap[VK_F10] = L"F10";
    keyboardLayoutMap[VK_F11] = L"F11";
    keyboardLayoutMap[VK_F12] = L"F12";
    keyboardLayoutMap[VK_F13] = L"F13";
    keyboardLayoutMap[VK_F14] = L"F14";
    keyboardLayoutMap[VK_F15] = L"F15";
    keyboardLayoutMap[VK_F16] = L"F16";
    keyboardLayoutMap[VK_F17] = L"F17";
    keyboardLayoutMap[VK_F18] = L"F18";
    keyboardLayoutMap[VK_F19] = L"F19";
    keyboardLayoutMap[VK_F20] = L"F20";
    keyboardLayoutMap[VK_F21] = L"F21";
    keyboardLayoutMap[VK_F22] = L"F22";
    keyboardLayoutMap[VK_F23] = L"F23";
    keyboardLayoutMap[VK_F24] = L"F24";
    keyboardLayoutMap[VK_NUMLOCK] = L"Num Lock";
    keyboardLayoutMap[VK_SCROLL] = L"Scroll Lock";
    keyboardLayoutMap[VK_LSHIFT] = L"Shift (Left)";
    keyboardLayoutMap[VK_RSHIFT] = L"Shift (Right)";
    keyboardLayoutMap[VK_LCONTROL] = L"Ctrl (Left)";
    keyboardLayoutMap[VK_RCONTROL] = L"Ctrl (Right)";
    keyboardLayoutMap[VK_LMENU] = L"Alt (Left)";
    keyboardLayoutMap[VK_RMENU] = L"Alt (Right)";
    keyboardLayoutMap[VK_BROWSER_BACK] = L"Browser Back";
    keyboardLayoutMap[VK_BROWSER_FORWARD] = L"Browser Forward";
    keyboardLayoutMap[VK_BROWSER_REFRESH] = L"Browser Refresh";
    keyboardLayoutMap[VK_BROWSER_STOP] = L"Browser Stop";
    keyboardLayoutMap[VK_BROWSER_SEARCH] = L"Browser Search";
    keyboardLayoutMap[VK_BROWSER_FAVORITES] = L"Browser Favorites";
    keyboardLayoutMap[VK_BROWSER_HOME] = L"Browser Home";
    keyboardLayoutMap[VK_VOLUME_MUTE] = L"Volume Mute";
    keyboardLayoutMap[VK_VOLUME_DOWN] = L"Volume Down";
    keyboardLayoutMap[VK_VOLUME_UP] = L"Volume Up";
    keyboardLayoutMap[VK_MEDIA_NEXT_TRACK] = L"Next Track";
    keyboardLayoutMap[VK_MEDIA_PREV_TRACK] = L"Previous Track";
    keyboardLayoutMap[VK_MEDIA_STOP] = L"Stop Media";
    keyboardLayoutMap[VK_MEDIA_PLAY_PAUSE] = L"Play/Pause Media";
    keyboardLayoutMap[VK_LAUNCH_MAIL] = L"Start Mail";
    keyboardLayoutMap[VK_LAUNCH_MEDIA_SELECT] = L"Select Media";
    keyboardLayoutMap[VK_LAUNCH_APP1] = L"Start App 1";
    keyboardLayoutMap[VK_LAUNCH_APP2] = L"Start App 2";
    keyboardLayoutMap[VK_PACKET] = L"Packet";
    keyboardLayoutMap[VK_ATTN] = L"Attn";
    keyboardLayoutMap[VK_CRSEL] = L"CrSel";
    keyboardLayoutMap[VK_EXSEL] = L"ExSel";
    keyboardLayoutMap[VK_EREOF] = L"Erase EOF";
    keyboardLayoutMap[VK_PLAY] = L"Play";
    keyboardLayoutMap[VK_ZOOM] = L"Zoom";
    keyboardLayoutMap[VK_PA1] = L"PA1";
    keyboardLayoutMap[VK_OEM_CLEAR] = L"Clear";
    keyboardLayoutMap[0xFF] = L"Undefined";
    // keyboardLayoutMap[CommonSharedConstants::VK_WIN_BOTH] = L"Win";
    keyboardLayoutMap[VK_KANA] = L"IME Kana";
    keyboardLayoutMap[VK_HANGEUL] = L"IME Hangeul";
    keyboardLayoutMap[VK_HANGUL] = L"IME Hangul";
    keyboardLayoutMap[VK_JUNJA] = L"IME Junja";
    keyboardLayoutMap[VK_FINAL] = L"IME Final";
    keyboardLayoutMap[VK_HANJA] = L"IME Hanja";
    keyboardLayoutMap[VK_KANJI] = L"IME Kanji";
    keyboardLayoutMap[VK_CONVERT] = L"IME Convert";
    keyboardLayoutMap[VK_NONCONVERT] = L"IME Non-Convert";
    keyboardLayoutMap[VK_ACCEPT] = L"IME Kana";
    keyboardLayoutMap[VK_MODECHANGE] = L"IME Mode Change";
    // keyboardLayoutMap[CommonSharedConstants::VK_DISABLED] = L"Disable";

    return keyboardLayoutMap;
}

const std::wstring& Win32Utils::GetKeyName(uint8_t key) noexcept {
    return GetKeyNames()[key];
}

static std::wstring_view ExtractDirectory(std::wstring_view path) noexcept {
    size_t delimPos = path.find_last_of(L'\\');
    return delimPos == std::wstring_view::npos ? path : path.substr(0, delimPos + 1);
}

#include <pathcch.h>
#include <strsafe.h>

static PCWSTR find_last_path_segment(_In_ PCWSTR path)
{
    auto const pathLength = wcslen(path);
    // If there is a trailing slash ignore that in the search.
    auto const limitedLength = ((pathLength > 0) && (path[pathLength - 1] == L'\\')) ? (pathLength - 1) : pathLength;

    PCWSTR result = nullptr;
    auto const offset = FindStringOrdinal(FIND_FROMEND, path, static_cast<int>(limitedLength), L"\\", 1, TRUE);
    if (offset == -1)
    {
        result = path + pathLength; // null terminator
    }
    else
    {
        result = path + offset + 1; // just past the slash
    }
    return result;
}

static bool try_get_parent_path_range(_In_ PCWSTR path, _Out_ size_t* parentPathLength)
{
    *parentPathLength = 0;
    bool hasParent = false;
    PCWSTR rootEnd = nullptr;
    if (SUCCEEDED(PathCchSkipRoot(path, &rootEnd)) && (*rootEnd != L'\0'))
    {
        auto const lastSegment = find_last_path_segment(path);
        *parentPathLength = lastSegment - path;
        hasParent = (*parentPathLength != 0);
    }
    return hasParent;
}

HRESULT Win32Utils::CreateDirectoryDeepNoThrow(PCWSTR path) noexcept {
    if (::CreateDirectoryW(path, nullptr) == FALSE)
    {
        DWORD lastError = ::GetLastError();
        if (lastError == ERROR_PATH_NOT_FOUND)
        {
            size_t parentLength{};
            if (try_get_parent_path_range(path, &parentLength))
            {
                std::unique_ptr<wchar_t[]> parent(new (std::nothrow) wchar_t[parentLength + 1]);
                if (parent.get() == nullptr)
                {
                    return E_OUTOFMEMORY;
                }
                HRESULT hr = StringCchCopyNW(parent.get(), parentLength + 1, path, parentLength);
                if (hr != S_OK)
                {
                    return hr;
                }
                hr = CreateDirectoryDeepNoThrow(parent.get()); // recurs
                if (hr != S_OK)
                {
                    return hr;
                }
            }
            if (::CreateDirectoryW(path, nullptr) == FALSE)
            {
                lastError = ::GetLastError();
                if (lastError != ERROR_ALREADY_EXISTS)
                {
                    return lastError;
                }
            }
        }
        else if (lastError != ERROR_ALREADY_EXISTS)
        {
            return lastError;
        }
    }
    return S_OK;
}

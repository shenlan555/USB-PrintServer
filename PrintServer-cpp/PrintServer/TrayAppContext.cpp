#include "pch.h"
#include "TrayAppContext.h"

#include <string>

#include "LogHelper.h"
#include "Program.h"
#include "resource.h"

namespace ps
{

namespace
{

const UINT kTrayCallbackMessage = WM_APP + 1;
const UINT kIconId = 1;

const UINT_PTR kCheckTimerId = 1;   // 2 秒状态检测
const UINT_PTR kHideTimerId = 2;    // 停止提示后延迟隐藏

const UINT kMenuOpen = 40001;
const UINT kMenuExit = 40002;

// 对应 ServiceControllerStatus 的取值（供 _lastStatus 比较与 switch 使用）
const DWORD kStatusStopped = SERVICE_STOPPED;
const DWORD kStatusStartPending = SERVICE_START_PENDING;
const DWORD kStatusStopPending = SERVICE_STOP_PENDING;
const DWORD kStatusRunning = SERVICE_RUNNING;
const DWORD kStatusContinuePending = SERVICE_CONTINUE_PENDING;
const DWORD kStatusPausePending = SERVICE_PAUSE_PENDING;
const DWORD kStatusPaused = SERVICE_PAUSED;

const DWORD kStatusQueryFailed = 0xFFFFFFFF;      // 查询失败（服务不存在）
const DWORD kStatusQueryError = 0xFFFFFFFE;       // 查询失败（其他错误）

} // namespace

IMPLEMENT_DYNAMIC(CTrayAppContext, CWnd)

BEGIN_MESSAGE_MAP(CTrayAppContext, CWnd)
    ON_MESSAGE(kTrayCallbackMessage, &CTrayAppContext::OnTrayCallback)
    ON_WM_TIMER()
    ON_WM_CLOSE()
END_MESSAGE_MAP()

CTrayAppContext::CTrayAppContext()
    : m_iconVisible(false)
    , m_disposed(false)
    , m_wasInstalled(false)
    , m_timerStarted(false)
    , m_lastStatus(kStatusStopped)
    , m_lastQueryError(ERROR_SUCCESS)
{
    memset(&m_nid, 0, sizeof(m_nid));
}

CTrayAppContext::~CTrayAppContext()
{
    CleanupAndExit();
}

bool CTrayAppContext::Initialize()
{
    // 检查 PrintServer 服务是否存在
    if (!ServiceExists())
    {
        Log::WriteLog("[Tray] PrintServer服务未安装，托盘助手退出");
        return false;
    }

    m_wasInstalled = true;

    // 隐藏的消息窗口，用于接收托盘回调与定时器消息
    LPCTSTR className = ::AfxRegisterWndClass(0);
    if (!CreateEx(0, className, L"PrintServerTray", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr))
        return false;

    // 初始化托盘图标（默认隐藏，等检测到服务运行再显示）
    m_nid.cbSize = sizeof(m_nid);
    m_nid.hWnd = GetSafeHwnd();
    m_nid.uID = kIconId;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = kTrayCallbackMessage;

    HICON icon = (HICON)::LoadImageW(::AfxGetInstanceHandle(),
                                     MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 0, 0,
                                     LR_DEFAULTSIZE | LR_SHARED);
    if (icon == nullptr)
        icon = ::LoadIconW(nullptr, IDI_APPLICATION);
    m_nid.hIcon = icon;

    wcscpy_s(m_nid.szTip, L"网络打印服务器 - 监控中...");

    // 右键菜单
    m_menu.CreatePopupMenu();
    m_menu.AppendMenuW(MF_STRING, kMenuOpen, L"打开控制面板");
    m_menu.AppendMenuW(MF_SEPARATOR);
    m_menu.AppendMenuW(MF_STRING, kMenuExit, L"退出托盘监控");

    // 对应 C# 的 mnuOpen.Font = Bold：默认菜单项以加粗绘制
    m_menu.SetDefaultItem(0, TRUE);

    // 定时器：每 2 秒检测一次服务状态
    SetTimer(kCheckTimerId, 2000, nullptr);
    m_timerStarted = true;

    // 立即执行首次检测
    CheckServiceStatus();

    Log::WriteLog("[Tray] 托盘助手已启动，监控 PrintServer 服务状态");
    return true;
}

bool CTrayAppContext::ServiceExists()
{
    SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr)
        return false;

    SC_HANDLE service = ::OpenServiceW(manager, L"PrintServer", SERVICE_QUERY_STATUS);
    ::CloseServiceHandle(manager);

    if (service == nullptr)
        return false;

    ::CloseServiceHandle(service);
    return true;
}

DWORD CTrayAppContext::QueryServiceStatus()
{
    if (m_disposed)
        return kStatusQueryFailed;

    // 每次查询都现开现关句柄：SCM 只有在“服务已停止 + 所有服务句柄都已关闭”后，
    // 才会把已被标记删除的服务真正从数据库移除。若在这里长期持有服务句柄，
    // 用户点“卸载服务”时 sc delete 只会把服务标记为待删除（再次删除返回 1072），
    // 卸载看起来就永远失败。
    SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr)
    {
        m_lastQueryError = ::GetLastError();
        return kStatusQueryError;
    }

    SC_HANDLE service = ::OpenServiceW(manager, L"PrintServer", SERVICE_QUERY_STATUS);
    if (service == nullptr)
    {
        m_lastQueryError = ::GetLastError();
        ::CloseServiceHandle(manager);

        if (m_lastQueryError == ERROR_SERVICE_DOES_NOT_EXIST ||
            m_lastQueryError == ERROR_SERVICE_MARKED_FOR_DELETE ||
            m_lastQueryError == ERROR_INVALID_HANDLE)
            return kStatusQueryFailed;   // 已卸载 / 正在卸载 → 按“服务不存在”处理

        return kStatusQueryError;
    }

    SERVICE_STATUS_PROCESS status;
    memset(&status, 0, sizeof(status));
    DWORD needed = 0;

    if (!::QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed))
    {
        DWORD error = ::GetLastError();
        m_lastQueryError = error;

        ::CloseServiceHandle(service);
        ::CloseServiceHandle(manager);

        if (error == ERROR_SERVICE_DOES_NOT_EXIST || error == ERROR_INVALID_HANDLE)
            return kStatusQueryFailed;

        return kStatusQueryError;
    }

    const DWORD state = status.dwCurrentState;

    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);

    m_lastQueryError = ERROR_SUCCESS;
    return state;
}

void CTrayAppContext::SetIconVisible(bool visible)
{
    if (visible == m_iconVisible)
        return;

    if (visible)
    {
        m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        ::Shell_NotifyIconW(NIM_ADD, &m_nid);
        m_iconVisible = true;
    }
    else
    {
        ::Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_iconVisible = false;
    }
}

void CTrayAppContext::SetIconText(const wchar_t* text)
{
    // 对应 C# 的 _notifyIcon.Text = ...
    wcscpy_s(m_nid.szTip, text);

    if (m_iconVisible)
    {
        m_nid.uFlags = NIF_TIP;
        ::Shell_NotifyIconW(NIM_MODIFY, &m_nid);
    }
}

void CTrayAppContext::ShowBalloon(const std::wstring& title, const std::wstring& text, DWORD flags)
{
    m_nid.uFlags = NIF_INFO;
    m_nid.dwInfoFlags = flags;
    m_nid.uTimeout = 3000;   // 对应 C# ShowBalloonTip(3000, ...)

    wcsncpy_s(m_nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(m_nid.szInfo, text.c_str(), _TRUNCATE);

    ::Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

void CTrayAppContext::CheckServiceStatus()
{
    if (m_disposed)
        return;

    DWORD currentStatus = QueryServiceStatus();

    if (currentStatus == kStatusQueryFailed)
    {
        // 服务被卸载
        if (m_wasInstalled)
        {
            SetIconVisible(false);
            SetIconText(L"网络打印服务器 - 未安装");
            ShowBalloon(L"网络打印服务器", L"PrintServer 服务已被卸载，托盘监控退出",
                        NIIF_WARNING);

            m_wasInstalled = false;
            Log::WriteLog("[Tray] 服务已被卸载，托盘助手即将退出");
            CleanupAndExit();
        }
        return;
    }

    if (currentStatus == kStatusQueryError)
    {
        Log::WriteLog("[Tray] 检测服务状态异常: 无法查询服务状态（错误码 " +
                      std::to_string(m_lastQueryError) + "）");
        // 连续失败不立即退出，等待下次检测
        return;
    }

    // 状态未变化，只确保 Visible 正确
    if (currentStatus == m_lastStatus)
    {
        SetIconVisible(currentStatus == kStatusRunning);
        return;
    }

    switch (currentStatus)
    {
    case kStatusRunning:
        // 服务启动 → 显示托盘 + 气泡
        SetIconVisible(true);
        SetIconText(L"网络打印服务器 - 运行中");
        ShowBalloon(L"网络打印服务器", L"打印服务器已启动 ✓", NIIF_INFO);
        Log::WriteLog("[Tray] 检测到服务已启动，显示托盘图标");
        break;

    case kStatusStopped:
    case kStatusStopPending:
        // 服务停止 → 隐藏托盘 + 气泡
        if (m_lastStatus == kStatusRunning)
        {
            SetIconVisible(true);   // 短暂显示以发送气泡
            ShowBalloon(L"网络打印服务器", L"打印服务器已停止", NIIF_WARNING);

            // 延迟隐藏，让气泡有时间显示
            SetTimer(kHideTimerId, 3500, nullptr);
        }
        else
        {
            SetIconVisible(false);
        }
        SetIconText(L"网络打印服务器 - 已停止");
        Log::WriteLog("[Tray] 检测到服务已停止，隐藏托盘图标");
        break;

    case kStatusPaused:
    case kStatusPausePending:
        SetIconVisible(true);
        SetIconText(L"网络打印服务器 - 已暂停");
        break;

    case kStatusStartPending:
    case kStatusContinuePending:
        SetIconVisible(true);
        SetIconText(L"网络打印服务器 - 启动中...");
        break;

    default:
        SetIconVisible(false);
        break;
    }

    m_lastStatus = currentStatus;
}

void CTrayAppContext::OpenControlPanel()
{
    std::wstring exePath = ProgramHost::GetExecutablePath();

    HINSTANCE result = ::ShellExecuteW(nullptr, L"open", exePath.c_str(),
                                       nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32)
    {
        Log::WriteLog("[Tray] 打开控制面板失败: 错误码 " +
                      std::to_string((unsigned long)reinterpret_cast<INT_PTR>(result)));
    }
}

void CTrayAppContext::CleanupAndExit()
{
    if (m_disposed)
        return;

    m_disposed = true;

    if (m_timerStarted)
    {
        KillTimer(kCheckTimerId);
        KillTimer(kHideTimerId);
        m_timerStarted = false;
    }

    SetIconVisible(false);

    if (GetSafeHwnd() != nullptr)
    {
        // 对应 C# 的 ExitThread()
        ::PostQuitMessage(0);
    }

    Log::WriteLog("[Tray] 托盘助手已退出");
}

LRESULT CTrayAppContext::OnTrayCallback(WPARAM /*wParam*/, LPARAM lParam)
{
    switch (LOWORD(lParam))
    {
    case WM_RBUTTONUP:
    {
        CPoint point;
        ::GetCursorPos(&point);

        SetForegroundWindow();
        UINT command = (UINT)::TrackPopupMenuEx(m_menu.GetSafeHmenu(),
                                                TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                                point.x, point.y, GetSafeHwnd(), nullptr);
        // 经典托盘菜单要求：关闭菜单后补一条空消息，避免菜单残留
        PostMessage(WM_NULL);

        if (command == kMenuOpen)
        {
            OpenControlPanel();
        }
        else if (command == kMenuExit)
        {
            Log::WriteLog("[Tray] 用户手动退出托盘监控");
            CleanupAndExit();
        }
        break;
    }

    case WM_LBUTTONDBLCLK:
        // 对应 C# 的 _notifyIcon.DoubleClick（异常静默忽略）
        OpenControlPanel();
        break;

    default:
        break;
    }

    return 0;
}

void CTrayAppContext::OnTimer(UINT_PTR nIDEvent)
{
    if (nIDEvent == kHideTimerId)
    {
        KillTimer(kHideTimerId);
        if (!m_disposed)
            SetIconVisible(false);
        return;
    }

    if (nIDEvent == kCheckTimerId)
    {
        CheckServiceStatus();
        return;
    }

    CWnd::OnTimer(nIDEvent);
}

// 外部（控制面板卸载服务时）请求托盘助手退出：必须走这里而不是直接 DestroyWindow，
// 否则托盘图标会残留在通知区
void CTrayAppContext::OnClose()
{
    Log::WriteLog("[Tray] 收到退出请求，托盘助手退出");
    CleanupAndExit();
}

} // namespace ps

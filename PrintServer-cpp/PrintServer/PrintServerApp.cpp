#include "pch.h"
#include "PrintServerApp.h"
#include "CryptoSelfTest.h"
#include "TrayAppContext.h"
#include "ServiceHost.h"
#include "Program.h"
#include "LicenseHelper.h"
#include "LicenseDlg.h"
#include "PrintServerDlg.h"
#include "LogHelper.h"
#include "Config.h"

#include <string>
#include <vector>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CPrintServerApp, CWinApp)
END_MESSAGE_MAP()

CPrintServerApp::CPrintServerApp()
    : m_trayMutex(nullptr)
{
}

CPrintServerApp theApp;

namespace
{

bool EqualsIgnoreCase(const std::wstring& text, const wchar_t* value)
{
    return _wcsicmp(text.c_str(), value) == 0;
}

std::wstring ToLowerInvariant(const std::wstring& text)
{
    std::wstring result = text;
    ::CharLowerBuffW(&result[0], (DWORD)result.size());
    return result;
}

// 对应 C# Program.CheckLicense()
// 修复Bug1：license.dat 不存在时跳过联网验证，强制弹出授权窗口
bool CheckLicense()
{
    if (!ps::License::HasLocalLicenseFile())
    {
        ps::License::ForceLicenseReactivation();

        ps::CLicenseDlg licenseDlg;
        if (licenseDlg.DoModal() != IDOK)
            return false;

        if (ps::License::CurrentStatus() != ps::License::Status::Licensed)
        {
            ::MessageBoxW(nullptr, L"授权验证失败，程序无法启动", L"授权失败",
                          MB_OK | MB_ICONERROR);
            return false;
        }
    }
    else
    {
        // license.dat 存在，正常联网验证
        if (!ps::License::StartValidation())
        {
            ps::CLicenseDlg licenseDlg;
            if (licenseDlg.DoModal() != IDOK)
                return false;

            if (ps::License::CurrentStatus() != ps::License::Status::Licensed)
            {
                ::MessageBoxW(nullptr, L"授权验证失败，程序无法启动", L"授权失败",
                              MB_OK | MB_ICONERROR);
                return false;
            }
        }
    }

    return true;
}

} // namespace

BOOL CPrintServerApp::InitInstance()
{
    CWinApp::InitInstance();

    // 对应 C# Main(string[] args)：args 不含可执行文件本身
    std::vector<std::wstring> args;
    {
        int argc = 0;
        LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
        if (argv != nullptr)
        {
            for (int i = 1; i < argc; ++i)
                args.push_back(argv[i]);

            ::LocalFree(argv);
        }
    }

    // 首次运行按内置模板生成 <exe>.config（不再依赖构建脚本复制 App.config）。
    // 必须在任何 GetSetting 之前执行，否则首次启动会一路走默认值、且保存设置无效。
    ps::Config::EnsureConfigFile();

#ifdef _DEBUG
    // ===== 以下命令行自检开关仅存在于 Debug 构建；Release 交付版已剥离 =====
    // 原因：/guitest 会跳过管理员检查与授权门禁，/svctest 会跳过授权直接启动服务，
    //       留在交付版等于提供免授权运行的后门。
    // -------- 命令行自检模式（不显示界面，仅供开发与现场诊断）--------
    if (!args.empty())
    {
        const std::wstring& command = args[0];

        // 用法: PrintServer.exe /cryptotest [向量文件] [报告文件]
        if (EqualsIgnoreCase(command, L"/cryptotest"))
        {
            std::wstring vectors = (args.size() >= 2) ? args[1] : L"";
            std::wstring report = (args.size() >= 3) ? args[2] : L"";

            if (vectors.empty())
            {
                wchar_t temp[MAX_PATH] = { 0 };
                ::GetTempPathW(MAX_PATH, temp);
                vectors = std::wstring(temp) + L"crypto_vectors.txt";
                report = std::wstring(temp) + L"crypto_selftest_report.txt";
            }
            else if (report.empty())
            {
                report = vectors + L".report.txt";
            }

            ps::RunCryptoSelfTest(vectors, report);
            return FALSE;
        }

        // 用法: PrintServer.exe /tcptest [报告文件]
        if (EqualsIgnoreCase(command, L"/tcptest"))
        {
            std::wstring report = (args.size() >= 2) ? args[1] : L"";
            if (report.empty())
            {
                wchar_t temp[MAX_PATH] = { 0 };
                ::GetTempPathW(MAX_PATH, temp);
                report = std::wstring(temp) + L"tcp_selftest_report.txt";
            }

            ps::RunTcpSelfTest(report);
            return FALSE;
        }

        // 用法: PrintServer.exe /apitest [报告文件]
        if (EqualsIgnoreCase(command, L"/apitest"))
        {
            std::wstring report = (args.size() >= 2) ? args[1] : L"";
            if (report.empty())
            {
                wchar_t temp[MAX_PATH] = { 0 };
                ::GetTempPathW(MAX_PATH, temp);
                report = std::wstring(temp) + L"api_selftest_report.txt";
            }

            ps::RunApiSelfTest(report);
            return FALSE;
        }

        // 用法: PrintServer.exe /svctest [报告文件]
        if (EqualsIgnoreCase(command, L"/svctest"))
        {
            std::wstring report = (args.size() >= 2) ? args[1] : L"";
            if (report.empty())
            {
                wchar_t temp[MAX_PATH] = { 0 };
                ::GetTempPathW(MAX_PATH, temp);
                report = std::wstring(temp) + L"service_selftest_report.txt";
            }

            ps::RunServiceSelfTest(report);
            return FALSE;
        }

        // 用法: PrintServer.exe /tlstest [报告文件]
        if (EqualsIgnoreCase(command, L"/tlstest"))
        {
            std::wstring report = (args.size() >= 2) ? args[1] : L"";
            if (report.empty())
            {
                wchar_t temp[MAX_PATH] = { 0 };
                ::GetTempPathW(MAX_PATH, temp);
                report = std::wstring(temp) + L"tls_selftest_report.txt";
            }

            ps::RunTlsSelfTest(report);
            return FALSE;
        }

        // 用法: PrintServer.exe /guitest [license]
        // 免提权直接打开界面，仅用于界面录制与视觉核对；
        // 带 license 参数时打开授权窗口（跳过提权与授权门禁）
        if (EqualsIgnoreCase(command, L"/guitest"))
        {
            INITCOMMONCONTROLSEX controls = { 0 };
            controls.dwSize = sizeof(controls);
            controls.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES |
                             ICC_PROGRESS_CLASS | ICC_DATE_CLASSES | ICC_BAR_CLASSES;
            ::InitCommonControlsEx(&controls);

            ps::License::Initialize();

            if (args.size() >= 2 && EqualsIgnoreCase(args[1], L"license"))
            {
                ps::CLicenseDlg licenseDialog;
                m_pMainWnd = &licenseDialog;
                licenseDialog.DoModal();
                return FALSE;
            }

            ps::CPrintServerDlg dialog;
            m_pMainWnd = &dialog;
            dialog.DoModal();
            return FALSE;
        }
    }
#endif // _DEBUG

    // ========== 模式判断 ==========
    // 托盘助手模式：不需要管理员权限、不需要反调试/授权
    if (!args.empty() &&
        (EqualsIgnoreCase(args[0], L"/tray") || EqualsIgnoreCase(args[0], L"-tray")))
    {
        return RunTrayMode() ? TRUE : FALSE;
    }

    // ========== 管理员权限检查（托盘模式除外） ==========
    if (!ps::ProgramHost::IsAdministrator())
    {
        // 非管理员 → 以 runas 提权重启自身
        ps::ProgramHost::RestartAsAdmin(args);
        return FALSE;
    }

    // ========== 管理员权限已确认，执行正常流程 ==========

    // 初始化授权系统
    ps::License::Initialize();
    ps::License::SetStatusChangedHandler(
        [](ps::License::Status /*status*/) {
            // 对应 C# 的 OnLicenseStatusChanged（当前仅作为扩展日志点）
        });

    // 处理命令行参数
    if (!args.empty())
    {
        std::wstring command = ToLowerInvariant(args[0]);

        if (command == L"/service" || command == L"-service" || command == L"/s")
        {
            // 服务模式：连接 SCM 并阻塞直到服务停止
            ps::ServiceHost host;
            host.Run();
            return FALSE;
        }

        if (command == L"/install" || command == L"-install")
        {
            ps::ProgramHost::InstallService();
            return FALSE;
        }

        if (command == L"/uninstall" || command == L"-uninstall")
        {
            ps::ProgramHost::UninstallService();
            return FALSE;
        }
    }

    // GUI 模式（对应 C# Program.RunGUI()）
    // 对应 C# 的 Application.EnableVisualStyles() + SetCompatibleTextRenderingDefault
    INITCOMMONCONTROLSEX icc = { 0 };
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES |
                ICC_PROGRESS_CLASS | ICC_DATE_CLASSES | ICC_BAR_CLASSES;
    ::InitCommonControlsEx(&icc);

    // 单实例：对应 C# 的 Mutex("PrintServer_Mutex")
    HANDLE guiMutex = ::CreateMutexW(nullptr, TRUE, L"PrintServer_Mutex");
    if (guiMutex == nullptr)
        return FALSE;

    if (::GetLastError() == ERROR_ALREADY_EXISTS)
    {
        ::CloseHandle(guiMutex);
        ::MessageBoxW(nullptr, L"打印服务器已经在运行中！", L"提示", MB_OK | MB_ICONINFORMATION);
        return FALSE;
    }

    // 强制授权验证
    if (!CheckLicense())
    {
        ::CloseHandle(guiMutex);
        return FALSE;
    }

    ps::CPrintServerDlg dialog;
    m_pMainWnd = &dialog;
    dialog.DoModal();

    ::CloseHandle(guiMutex);

    // 对话框已关闭，返回 FALSE 结束应用
    return FALSE;
}

int CPrintServerApp::ExitInstance()
{
    if (m_trayMutex != nullptr)
    {
        ::CloseHandle(m_trayMutex);
        m_trayMutex = nullptr;
    }

    return CWinApp::ExitInstance();
}

bool CPrintServerApp::RunTrayMode()
{
    // 使用独立互斥锁，与 GUI 不冲突
    m_trayMutex = ::CreateMutexW(nullptr, TRUE, L"PrintServerTray_Mutex");
    if (m_trayMutex == nullptr)
        return false;

    if (::GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // 托盘助手已在运行，静默退出
        ::CloseHandle(m_trayMutex);
        m_trayMutex = nullptr;
        return false;
    }

    ps::CTrayAppContext* context = new ps::CTrayAppContext();
    if (!context->Initialize())
    {
        delete context;
        return false;
    }

    m_pMainWnd = context;
    return true;
}


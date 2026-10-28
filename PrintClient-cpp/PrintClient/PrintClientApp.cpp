#include "pch.h"
#include "PrintClientApp.h"

#include <string>
#include <vector>

#include "ClientSelfTest.h"
#include "FileUtil.h"
#include "LanScanner.h"
#include "MainDlg.h"
#include "Program.h"
#include "Wmi.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace ps
{

BEGIN_MESSAGE_MAP(CPrintClientApp, CWinApp)
END_MESSAGE_MAP()

CPrintClientApp::CPrintClientApp()
{
}

int CPrintClientApp::ExitInstance()
{
    int code = CWinApp::ExitInstance();
    return m_selfTestExitCode >= 0 ? m_selfTestExitCode : code;
}

CPrintClientApp theApp;

namespace
{

bool EqualsIgnoreCase(const std::wstring& text, const wchar_t* value)
{
    return _wcsicmp(text.c_str(), value) == 0;
}

} // namespace

BOOL CPrintClientApp::InitInstance()
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

#ifdef _DEBUG
    // ===== /clienttest 仅在 Debug 构建中提供；Release 交付版已剥离 =====
    // -------- 命令行自检模式（不显示界面，仅供开发与现场诊断）--------
    // 用法: PrintClient.exe /clienttest <server> <apiPort> [printerIndex] [reportFile] [--install [installName]]
    //
    // --install 解析规则（宽松、确定性）：
    //   1) 找到第一个 --install 令牌即为安装模式，并按它把其余参数切成"前段/后段"；
    //   2) 位置参数 server / apiPort / printerIndex / reportFile 先在前段按序取（最多 4 个）；
    //      前段不足 2 个时继续从后段取（兼容 --install 写在最前面的写法）；
    //   3) 未被用作位置参数的令牌中，第一个（先看前段剩余，再看后段剩余）作为安装名，
    //      没有则用缺省 "<打印机名>(共享)"。
    //   因此以下写法都等价（安装名 MyName）：
    //     /clienttest host 9180 0 report.txt --install MyName
    //     /clienttest host 9180 0 --install MyName
    //     /clienttest --install host 9180 0 report.txt MyName
    //   安装模式下不再弹出任何对话框：参数不足时向 stderr 输出用法并以退出码 2 结束。
    if (!args.empty() && EqualsIgnoreCase(args[0], L"/clienttest"))
    {
        size_t installPosition = (size_t)-1;
        for (size_t i = 1; i < args.size(); ++i)
        {
            if (EqualsIgnoreCase(args[i], L"--install"))
            {
                installPosition = i;
                break;
            }
        }

        bool install = installPosition != (size_t)-1;

        std::vector<std::wstring> before;
        std::vector<std::wstring> after;
        for (size_t i = 1; i < args.size(); ++i)
        {
            if (i == installPosition)
                continue;

            if (!install || i < installPosition)
                before.push_back(args[i]);
            else
                after.push_back(args[i]);
        }

        std::vector<std::wstring> positional;
        size_t beforeUsed = 0;
        while (beforeUsed < before.size() && positional.size() < 4)
            positional.push_back(before[beforeUsed++]);

        size_t afterUsed = 0;
        if (positional.size() < 2)
        {
            while (afterUsed < after.size() && positional.size() < 4)
                positional.push_back(after[afterUsed++]);
        }

        if (positional.size() < 2)
        {
            const wchar_t* usage =
                L"用法: PrintClient.exe /clienttest <server> <apiPort> [printerIndex] [reportFile] "
                L"[--install [installName]]";

            if (install)
            {
                fwprintf(stderr, L"%ls\n", usage);
                m_selfTestExitCode = 2;
                return FALSE;
            }

            ::MessageBoxW(nullptr, usage, L"PrintClient 自检", MB_OK | MB_ICONINFORMATION);
            return FALSE;
        }

        std::wstring installName;
        if (beforeUsed < before.size())
            installName = before[beforeUsed];
        else if (afterUsed < after.size())
            installName = after[afterUsed];

        ClientSelfTestOptions options;
        options.server = positional[0];
        options.apiPort = _wtoi(positional[1].c_str());
        options.printerIndex = (positional.size() >= 3) ? _wtoi(positional[2].c_str()) : 0;
        options.reportPath = (positional.size() >= 4)
                                 ? positional[3]
                                 : PathCombine(GetTempDirectory(), L"client_selftest_report.txt");
        options.install = install;
        options.installName = installName;

        int failures = ps::RunClientSelfTest(options);

        // 仅 --install 模式把失败项数量作为进程退出码（未通过即非 0）；
        // 普通自检模式保持原有行为（退出码 0），报告内容两种模式完全一致。
        if (install)
            m_selfTestExitCode = failures;

        return FALSE;
    }
#endif // _DEBUG

    // ========== 权限检查：安装打印机驱动必须管理员 ==========
    // 装驱动要写驱动库、建端口、改 HKLM、重启 Spooler，普通权限下
    // printui/rundll32 会“假成功”（rundll32 恒返回 0），最终只能失败。
    // 因此这里先自提权，避免出现“日志全绿但实际没装上”的半成功状态。
    if (!ProgramHost::IsAdministrator())
    {
        if (!ProgramHost::RelaunchElevated(args))
            return FALSE;   // 用户拒绝 UAC（RelaunchElevated 内已提示）

        return FALSE;       // 已启动提权副本，当前实例退出
    }

    // ========== GUI 模式（对应 C# Application.Run(new MainForm())） ==========
    // 对应 Application.EnableVisualStyles()
    INITCOMMONCONTROLSEX controls = { 0 };
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS |
                     ICC_BAR_CLASSES | ICC_USEREX_CLASSES;
    ::InitCommonControlsEx(&controls);

    // Winsock 供局域网扫描使用
    EnsureWinsockInitialized();

    CMainDlg dialog;
    m_pMainWnd = &dialog;
    dialog.DoModal();

    // 对话框已关闭，返回 FALSE 结束应用
    return FALSE;
}

} // namespace ps

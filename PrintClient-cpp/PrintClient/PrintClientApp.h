#pragma once

// 应用对象（对应 C# Program.cs 的 Main）

#include <afxwin.h>

namespace ps
{

class CPrintClientApp : public CWinApp
{
public:
    CPrintClientApp();

    BOOL InitInstance() override;
    int ExitInstance() override;

private:
    // 命令行自检模式显式指定的进程退出码（-1 表示使用 MFC 默认行为）
    int m_selfTestExitCode = -1;

public:
    DECLARE_MESSAGE_MAP()
};

} // namespace ps

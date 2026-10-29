#pragma once

#include <windows.h>

// 应用程序类：对应 C# 的 Program.Main 入口职责
class CPrintServerApp : public CWinApp
{
public:
    CPrintServerApp();

    virtual BOOL InitInstance();
    virtual int ExitInstance();

    DECLARE_MESSAGE_MAP()

private:
    // /tray 模式的独立互斥锁，与 GUI 模式不冲突
    bool RunTrayMode();

    HANDLE m_trayMutex;

    // 进程退出码：命令行自检模式（/cryptotest 等）把失败数折算成 1 返回，
    // 便于 CI 直接按退出码判定，不必再解析报告文本。
    int m_exitCode = 0;
};

extern CPrintServerApp theApp;

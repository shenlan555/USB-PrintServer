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
};

extern CPrintServerApp theApp;

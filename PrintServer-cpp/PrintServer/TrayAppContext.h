#pragma once

// 托盘助手（对应 C# TrayAppContext.cs）
//
// 独立轻量进程（/tray 启动），仅监控 PrintServer Windows 服务状态：
//   - 服务未安装 / 无法连接 → 记录日志后静默退出
//   - 服务运行中 → 显示托盘图标 + 气泡；停止 → 提示后隐藏图标
//   - 服务被卸载 → 气泡提示并退出
// 右键菜单：「打开控制面板」（加粗）/ 分隔线 / 「退出托盘监控」
// 双击图标 = 打开控制面板

#include <afxwin.h>
#include <shellapi.h>
#include <winsvc.h>
#include <string>

namespace ps
{

class CTrayAppContext : public CWnd
{
    DECLARE_DYNAMIC(CTrayAppContext)

public:
    CTrayAppContext();
    virtual ~CTrayAppContext();

    // 返回 false 表示应立即退出（服务未安装或无法连接）
    bool Initialize();

protected:
    afx_msg LRESULT OnTrayCallback(WPARAM wParam, LPARAM lParam);
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg void OnClose();
    DECLARE_MESSAGE_MAP()

private:
    static bool ServiceExists();
    DWORD QueryServiceStatus();

    void CheckServiceStatus();
    void OpenControlPanel();
    void CleanupAndExit();

    void ShowBalloon(const std::wstring& title, const std::wstring& text, DWORD flags);
    void SetIconVisible(bool visible);
    void SetIconText(const wchar_t* text);

    NOTIFYICONDATAW m_nid;
    CMenu m_menu;
    bool m_iconVisible;
    bool m_disposed;
    bool m_wasInstalled;
    bool m_timerStarted;

    // 对应 C# 的 _lastStatus（ServiceControllerStatus）
    DWORD m_lastStatus;

    // 最近一次 QueryServiceStatusEx 失败的错误码
    DWORD m_lastQueryError;
};

} // namespace ps

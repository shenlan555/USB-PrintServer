#pragma once

// 授权管理窗口（对应 C# LicenseForm.cs）
//
// 提供在线激活、离线激活、机器码显示与复制、机器信息上报、检查授权。
// 未授权/授权失效时由调用方强制弹出；用户在未授权状态下直接关闭窗口将结束进程。

#include <afxwin.h>
#include <string>
#include <vector>

#include "resource.h"
#include "LicenseHelper.h"
#include "UiTheme.h"

namespace ps
{

class CLicenseDlg : public CDialogEx
{
    DECLARE_DYNAMIC(CLicenseDlg)

public:
    explicit CLicenseDlg(CWnd* pParent = nullptr);
    virtual ~CLicenseDlg();

    enum { IDD = IDD_LICENSE_DIALOG };

    // 对应 C# LicenseForm.CheckAndClose()
    void CheckAndClose();

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();

    afx_msg void OnPaint();
    afx_msg void OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct);
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);
    afx_msg void OnClose();
    afx_msg void OnActivateOnline();
    afx_msg void OnSelectLicFile();
    afx_msg void OnActivateOffline();
    afx_msg void OnReportMachine();
    afx_msg void OnCheckLicense();
    afx_msg void OnCopyMachineCode();
    afx_msg LRESULT OnSegChanged(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnAsyncDone(WPARAM wParam, LPARAM lParam);
    DECLARE_MESSAGE_MAP()

private:
    enum AsyncTask
    {
        TaskServerReachability = 1,
        TaskActivateOnline,
        TaskActivateOffline,
        TaskReportMachine,
        TaskCheckLicense
    };

    // 异步任务结果（工作线程创建，界面线程销毁）
    struct AsyncResult
    {
        AsyncTask task;
        bool value;
        std::string message;
    };

    void BuildControls();
    void Layout();
    void LoadMachineCode();
    void SelectTabPage(int index);
    void ResizeSeg();
    void RegisterLicenseSurfaces();
    RECT CardRect(int index) const;
    void UpdateOfflineStatusLayout();
    void StartServerReachabilityCheck();
    bool IsPageControl(HWND control) const;

    std::string GetKeyTextUtf8() const;
    void SetStatusLabel(UINT id, const wchar_t* text, COLORREF color);
    void SetOfflineStatus(const wchar_t* text, COLORREF color);
    void SetButtonText(UINT id, const wchar_t* text);

    // 缓存机器码，确保显示与复制的内容完全一致
    std::string m_cachedMachineCode;

    std::wstring m_selectedLicFilePath;

    // 外网授权服务器是否可达（可达时禁用离线激活）
    bool m_isServerReachable;
    bool m_offlineTabDisabled;
    bool m_reachabilityTaskRunning;

    CBrush m_whiteBrush;
    CBrush m_faceBrush;

    // 当前分段页（0 = 在线激活，1 = 离线激活）
    int m_page;

    // 页脚与 QQ 群行的纵坐标（贴合客户区底部，由 Layout 计算）
    int m_footStatusY;
    int m_footHintY;
    int m_footButtonY;
    int m_qqTop;

    // 输入框底板的表面登记矩形（BuildControls 计算后供换页时复用）
    RECT m_keyFieldRect;

    std::vector<HWND> m_onlineControls;
    std::vector<HWND> m_offlineControls;
};

} // namespace ps

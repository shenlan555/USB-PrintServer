#pragma once

// 主界面（对应 C# MainForm.cs）
//
// 全部子控件在 OnInitDialog 中按 WinForms 的像素坐标动态创建，
// 窗口尺寸固定为 560x670（对应 C# this.Size = new Size(560, 670) + FixedSingle）。

#include <afxwin.h>
#include <string>
#include <vector>

#include "resource.h"
#include "DriverDownloader.h"

namespace ps
{

class CMainDlg : public CDialogEx
{
    DECLARE_DYNAMIC(CMainDlg)

public:
    explicit CMainDlg(CWnd* pParent = nullptr);

    enum { IDD = IDD_MAIN_DIALOG };

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;

    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct);
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);

    afx_msg void OnScanClicked();
    afx_msg void OnConnectClicked();
    afx_msg void OnDownloadInstallClicked();
    afx_msg void OnClearLogClicked();

    afx_msg void OnPrinterListChanged(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnPrinterListDoubleClick(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnPrinterListCustomDraw(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnPrinterHeaderCustomDraw(NMHDR* pNMHDR, LRESULT* pResult);

    // 后台线程回传消息（WM_APP+n）
    afx_msg LRESULT OnScanProgress(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnScanDone(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnConnectTested(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnConnectInfo(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnDownloadProgress(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnDownloadDone(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnInstallProgress(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnInstallDone(WPARAM wParam, LPARAM lParam);

    DECLARE_MESSAGE_MAP()

private:
    void BuildControls();
    void LoadLocalSubnets();

    void Log(const std::string& message);
    void AppendLogLineInternal(const std::string& message);
    void TrimLogLines();

    void SetConnectStatus(const std::wstring& text, COLORREF color);
    void SetProgress(int percent, const std::wstring& label);

    void FillPrinterList();
    void ApplySelection(int index);
    void UpdateButtonState();
    int SelectedPrinterIndex() const;

    std::string ReadServerAddress() const;
    int ReadApiPort() const;

    void ConnectToServer(const std::string& server, int apiPort);
    void DownloadAndInstall(bool doInstall);

    // ---- IPP 免驱安装 ----
    // 勾选后不下载、不安装厂商驱动，直接用系统自带 Microsoft IPP Class Driver
    // 建一个指向 IPP 地址的打印队列。
    bool IsIppInstallSelected() const;
    void StartIppInstall(int selectedIndex);
    void ContinueInstallAfterDownload(const std::string& zipPath, const std::string& cachePath,
                                      const std::string& tempDir, int selectedIndex,
                                      bool doInstall);
    void FinishDownloadAndInstall();
    std::string ShowAddressSelector(const std::string& hostname, const std::string& ip);
    std::string CleanHostname(const std::string& hostname) const;

    // ---- 状态 ----
    ServerPrinterInfo m_printerInfo;
    bool m_hasPrinterInfo = false;
    bool m_isScanning = false;
    bool m_busy = false;

    CListCtrl m_printerList;
    CImageList m_rowImages;

    CBrush m_formBrush;
    CBrush m_headerBrush;
    CBrush m_logBrush;
    CBrush m_whiteBrush;
};

} // namespace ps

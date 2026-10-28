#pragma once

// 主界面（对应 C# PrintServerForm.cs）
//
// 标题栏、授权信息、打印机设置（列表 + 端口自动分配）、服务器信息、服务管理、
// 运行状态、运行日志；并支持最小化到系统托盘、3 秒轮询服务状态与日志。

#include <afxwin.h>
#include <string>
#include <vector>
#include <memory>

#include "resource.h"
#include "PrintService.h"
#include "HttpApiService.h"
#include "IppService.h"
#include "UiControls.h"

namespace ps
{

class CPrintServerDlg : public CDialogEx
{
    DECLARE_DYNAMIC(CPrintServerDlg)

public:
    explicit CPrintServerDlg(CWnd* pParent = nullptr);
    virtual ~CPrintServerDlg();

    // 打印机列表悬停行高亮（由列表子类化过程回调）
    void SetHoverRow(int row);

    enum { IDD = IDD_MAIN_DIALOG };

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual BOOL PreTranslateMessage(MSG* pMsg);

    afx_msg void OnPaint();
    afx_msg void OnNcCalcSize(BOOL bCalcValidRects, NCCALCSIZE_PARAMS* lpncsp);
    afx_msg void OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct);
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg void OnClose();
    afx_msg LRESULT OnTrayCallback(WPARAM wParam, LPARAM lParam);
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnStartStop();
    afx_msg void OnInstallToggle();
    afx_msg void OnServiceStartToggle();
    afx_msg void OnClearLog();
    afx_msg void OnRefreshPrinters();
    afx_msg void OnSelectAll();
    afx_msg void OnDeselectAll();
    afx_msg void OnPrinterListChanging(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnPrinterListChanged(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnPrinterListDblClk(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnPrinterListClick(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnPrinterListCustomDraw(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg LRESULT OnGuiLog(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnNavSelectionChanged(WPARAM wParam, LPARAM lParam);
    DECLARE_MESSAGE_MAP()

private:
    void BuildControls();
    void BuildNavBar();
    void ShowPage(int page);
    void LayoutPrinterCard(int top, int height);
    void LayoutLogCard(int top, int height);
    void DrawLicenseInfo(HDC hdc, const CRect& card);
    void DrawServerInfo(HDC hdc, const CRect& card);
    void LayoutPageHead();
    void SetHeadPill(UINT id, const wchar_t* text, COLORREF ink);
    void UpdatePrinterCountPill();
    void RegisterCardSurfaces();
    bool IsCardVisible(int index) const;
    void ShowGroup(const UINT* ids, size_t count, bool show);
    void DrawPrinterRow(const NMLVCUSTOMDRAW* draw);
    void LoadPrinters();
    void RefreshPrinterList();
    // 按当前分页把数据集渲染到列表：设备列表页显示全部，服务器页只显示已共享的
    void RenderPrinterList();
    int FindPrinterEntry(const std::string& name) const;
    static bool IsVirtualPrinter(const std::string& printerName, const std::string& portName,
                                 const std::string& driverName = std::string());
    void LoadSavedSettings();
    void UpdateNetworkInfo();
    void UpdateServiceStatus();
    void UpdateLicenseStatus();
    void LoadExistingLog();
    void AutoRefreshServiceLog();

    // 服务安装/卸载与启动/停止（对应 C# 的四个按钮处理函数）
    void BtnInstallService();
    void BtnUninstallService();
    void BtnStartService();
    void BtnStopService();

    // 勾选状态变化时重新分配端口（对应 C# DgvPrinters_CellValueChanged）
    void ReallocatePorts();
    std::vector<PrinterConfig> GetCheckedPrinterConfigs();
    void SaveSettings();

    std::string RunCmd(const std::wstring& fileName, const std::wstring& arguments, bool showLog);
    void StartServer();
    void StopServer();

    void LogToTextBox(const std::string& message);
    void AppendLogLine(const std::string& message);
    void WriteLogToFile(const std::string& message);

    void ShowTrayIcon(const std::vector<PrinterConfig>& configs);
    void HideTrayIcon();
    bool EnsureTrayIcon();
    void ShowMainWindow();

    void BeginPortEdit(int row);
    void CommitPortEdit(bool cancel);

    std::wstring GetListText(int row, int column);
    void SetListText(int row, int column, const std::wstring& text);
    bool GetRowChecked(int row);
    void SetRowChecked(int row, bool checked);
    std::string GetApiPortText();
    void SetApiPortText(const std::string& port);

    std::vector<std::unique_ptr<TcpPrintServer>> m_servers;
    std::unique_ptr<HttpApiServer> m_apiServer;
    std::shared_ptr<IppServer> m_ippServer;   // IPP 631（GUI 模式同样启用）
    bool m_isRunning;

    // GUI 模式托盘图标
    bool m_trayIconVisible;
    bool m_isReallyClosing;
    HICON m_trayIcon;

    // 产品名/版本（显示在左侧导航栏的品牌区）
    CString m_version;
    CString m_productTitle;
    CString m_productSubtitle;
    HICON m_logoIcon;

    // 左侧导航栏与分页
    Ui::CNavBar m_navBar;
    CString m_pageTitle;
    int m_page;
    CRect m_logBoxRect;     // 运行日志卡片内的深色日志框（OnPaint 铺底）
    CRect m_cardRects[5];       // 当前分页的卡片矩形
                                // 0 服务器控制 / 1 打印机列表 / 2 运行日志 /
                                // 3 授权信息 / 4 服务器信息（主机名·本机 IP）
    int m_printerCardTop;       // 打印机列表卡片当前几何（计数胶囊重排时复用）
    int m_printerCardHeight;

    // 授权信息页显示的本机信息（原先放在导航栏底部信息块）
    std::wstring m_infoHost;
    std::wstring m_infoIp;
    std::wstring m_infoMode;
    std::wstring m_licStatusText;
    std::wstring m_licExpiryText;
    std::wstring m_licKeyText;      // 授权码（未授权/读取失败时为空）
    COLORREF m_licStatusInk;

    // 打印机列表：当前悬停行（-1 表示无）
    int m_hoverRow;
    CImageList m_checkImages;

    // 上次读取到的日志文件全文（对应 C# 的 lastLogFileContent）
    std::string m_lastLogFileContent;

    // 刷新列表时抑制勾选变化回调（对应 C# 的事件解绑）
    bool m_suppressListEvents;

    // 本机所有打印机的数据集（列表控件只是它的视图）：
    // 设备列表页显示全部，服务器页只显示 shared 的打印机且不可编辑。
    struct PrinterEntry
    {
        std::string name;
        std::string port;
        bool shared = false;
        // 配置文件中 Printer<slot>_* 的稳定槽位（-1 表示尚未分配）。
        // 槽位只增不减：取消勾选不会让其它打印机的键名/端口/ipp 端点整体前移。
        int slot = -1;
    };
    std::vector<PrinterEntry> m_printerEntries;

    // 端口就地编辑
    CEdit m_portEdit;
    int m_editRow;

    CBrush m_formBrush;
    CBrush m_headerBrush;
    CBrush m_logBrush;
    CBrush m_whiteBrush;

    CListCtrl m_printerList;
    CImageList m_rowImages;
    CSpinButtonCtrl m_apiPortSpin;
};

} // namespace ps

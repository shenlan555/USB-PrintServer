#include "pch.h"
#include "MainDlg.h"

#include <string>
#include <vector>
#include <thread>

#include "BuildConfig.h"
#include "FileUtil.h"
#include "LanScanner.h"
#include "PrinterDriverInstaller.h"
#include "Program.h"
#include "UiTheme.h"
#include "Wmi.h"
#include "AddressDlg.h"

namespace ps
{

namespace
{

// 后台线程 → UI 线程的消息
const UINT kMsgScanProgress = WM_APP + 1;
const UINT kMsgScanDone = WM_APP + 2;
const UINT kMsgConnectTested = WM_APP + 3;
const UINT kMsgConnectInfo = WM_APP + 4;
const UINT kMsgDownloadProgress = WM_APP + 5;
const UINT kMsgDownloadDone = WM_APP + 6;
const UINT kMsgInstallProgress = WM_APP + 7;
const UINT kMsgInstallDone = WM_APP + 8;

// 对应 C# MainForm 的 Size
const int kFormWidth = 560;
const int kFormHeight = 670;

// 对应 WinForms 的颜色常量
const COLORREF kColorWhite = RGB(255, 255, 255);
const COLORREF kColorBlack = RGB(0, 0, 0);

const COLORREF kFormBack = RGB(245, 247, 250);
const COLORREF kHeaderBack = RGB(30, 60, 120);
const COLORREF kHeaderSubText = RGB(180, 200, 230);
const COLORREF kGroupText = RGB(50, 50, 80);
const COLORREF kLabelMuted = RGB(120, 120, 140);
const COLORREF kLogBack = RGB(30, 32, 38);
const COLORREF kLogText = RGB(180, 220, 180);
const COLORREF kGridHeaderBack = RGB(240, 242, 245);
const COLORREF kSelectionBack = RGB(210, 228, 255);
const COLORREF kServerInfoText = RGB(100, 120, 160);

const COLORREF kStatusGreen = RGB(40, 140, 60);
// 对应 C# 的 Color.Gray / Color.Green（连接相关状态用色，与"检测到网段"用的
// FromArgb(40,140,60) 不是同一个值）
const COLORREF kColorGray = RGB(128, 128, 128);
const COLORREF kColorGreen = RGB(0, 128, 0);
const COLORREF kStatusOrange = RGB(200, 120, 40);
const COLORREF kStatusRed = RGB(255, 0, 0);

const COLORREF kBtnScan = RGB(100, 140, 200);
const COLORREF kBtnConnect = RGB(60, 160, 80);
const COLORREF kBtnDownload = RGB(60, 160, 80);
const COLORREF kBtnClearLog = RGB(180, 120, 110);

const wchar_t* kYahei = L"微软雅黑";

// ===========================================================================
// 线程回传的数据包
// ===========================================================================

struct ScanProgressMsg
{
    std::string message;
};

struct ScanDoneMsg
{
    std::vector<ScanResult> results;
};

struct ConnectTestMsg
{
    bool connected = false;
    std::string server;
    int apiPort = 0;
};

struct ConnectInfoMsg
{
    bool ok = false;
    std::string error;
    ServerPrinterInfo info;
};

struct ProgressMsg
{
    int percent = 0;
};

struct DownloadDoneMsg
{
    bool ok = false;
    bool doInstall = true;
    std::string error;
    std::string zipPath;
    std::string tempDir;
    std::string cachePath;
    int selectedIndex = 0;
};

struct InstallProgressMsg
{
    std::string message;
};

struct InstallDoneMsg
{
    InstallResult result;
};

// ===========================================================================
// 小工具
// ===========================================================================

HWND CreateChildWnd(HWND parent, const wchar_t* className, const wchar_t* text,
                    DWORD style, DWORD exStyle, int x, int y, int w, int h,
                    UINT id, HFONT font)
{
    HWND child = ::CreateWindowExW(exStyle, className, text, WS_CHILD | WS_VISIBLE | style,
                                   x, y, w, h, parent, (HMENU)(UINT_PTR)id,
                                   ::AfxGetInstanceHandle(), nullptr);
    if (child != nullptr && font != nullptr)
        ::SendMessageW(child, WM_SETFONT, (WPARAM)font, MAKELPARAM(TRUE, 0));

    return child;
}

// 8.5pt 的 Consolas（GetFont 只接受整数点值）
HFONT CreateConsolasFont(int pointTimes10)
{
    HDC screen = ::GetDC(nullptr);
    int dpi = ::GetDeviceCaps(screen, LOGPIXELSY);
    ::ReleaseDC(nullptr, screen);

    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -(int)((pointTimes10 / 10.0) * dpi / 72.0 + 0.5);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy_s(lf.lfFaceName, L"Consolas", _TRUNCATE);

    return ::CreateFontIndirectW(&lf);
}

std::wstring GetWindowTextString(HWND window)
{
    if (window == nullptr)
        return std::wstring();

    int length = ::GetWindowTextLengthW(window);
    std::wstring text((size_t)length + 1, L'\0');
    int copied = ::GetWindowTextW(window, &text[0], length + 1);
    text.resize((size_t)copied);
    return text;
}

void ShowControl(HWND window, bool visible)
{
    if (window != nullptr)
        ::ShowWindow(window, visible ? SW_SHOW : SW_HIDE);
}

} // namespace

IMPLEMENT_DYNAMIC(CMainDlg, CDialogEx)

BEGIN_MESSAGE_MAP(CMainDlg, CDialogEx)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
    ON_WM_DRAWITEM()
    ON_WM_CTLCOLOR()
    ON_WM_GETMINMAXINFO()
    ON_BN_CLICKED(IDC_PC_BTN_SCAN, &CMainDlg::OnScanClicked)
    ON_BN_CLICKED(IDC_PC_BTN_CONNECT, &CMainDlg::OnConnectClicked)
    ON_BN_CLICKED(IDC_PC_BTN_DOWNLOAD_INSTALL, &CMainDlg::OnDownloadInstallClicked)
    ON_BN_CLICKED(IDC_PC_BTN_CLEAR_LOG, &CMainDlg::OnClearLogClicked)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_PC_DGV_PRINTERS, &CMainDlg::OnPrinterListChanged)
    ON_NOTIFY(NM_DBLCLK, IDC_PC_DGV_PRINTERS, &CMainDlg::OnPrinterListDoubleClick)
    ON_NOTIFY(NM_CUSTOMDRAW, IDC_PC_DGV_PRINTERS, &CMainDlg::OnPrinterListCustomDraw)
    ON_MESSAGE(kMsgScanProgress, &CMainDlg::OnScanProgress)
    ON_MESSAGE(kMsgScanDone, &CMainDlg::OnScanDone)
    ON_MESSAGE(kMsgConnectTested, &CMainDlg::OnConnectTested)
    ON_MESSAGE(kMsgConnectInfo, &CMainDlg::OnConnectInfo)
    ON_MESSAGE(kMsgDownloadProgress, &CMainDlg::OnDownloadProgress)
    ON_MESSAGE(kMsgDownloadDone, &CMainDlg::OnDownloadDone)
    ON_MESSAGE(kMsgInstallProgress, &CMainDlg::OnInstallProgress)
    ON_MESSAGE(kMsgInstallDone, &CMainDlg::OnInstallDone)
END_MESSAGE_MAP()

CMainDlg::CMainDlg(CWnd* pParent)
    : CDialogEx(IDD_MAIN_DIALOG, pParent)
{
}

void CMainDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
}

BOOL CMainDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    m_formBrush.CreateSolidBrush(kFormBack);
    m_headerBrush.CreateSolidBrush(kHeaderBack);
    m_logBrush.CreateSolidBrush(kLogBack);
    m_whiteBrush.CreateSolidBrush(kColorWhite);

    // 对应 C#：Size 560x670 + FixedSingle
    SetWindowPos(nullptr, 0, 0, kFormWidth, kFormHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);

    HICON icon = (HICON)::LoadImageW(::AfxGetInstanceHandle(),
                                     MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 0, 0,
                                     LR_DEFAULTSIZE | LR_SHARED);
    if (icon != nullptr)
    {
        SetIcon(icon, TRUE);
        SetIcon(icon, FALSE);
    }

    BuildControls();

    // 对应 StartPosition = FormStartPosition.CenterScreen
    POINT cursor;
    ::GetCursorPos(&cursor);
    HMONITOR monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info;
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info))
    {
        CRect window;
        GetWindowRect(&window);

        int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - window.Width()) / 2;
        int y = info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - window.Height()) / 2;
        SetWindowPos(nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
    }

    // 对应构造函数里的 LoadLocalSubnets()
    LoadLocalSubnets();

    return TRUE;
}

void CMainDlg::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
    // 对应 FixedSingle + MaximizeBox=false：尺寸完全固定
    lpMMI->ptMinTrackSize.x = kFormWidth;
    lpMMI->ptMinTrackSize.y = kFormHeight;
    lpMMI->ptMaxTrackSize.x = kFormWidth;
    lpMMI->ptMaxTrackSize.y = kFormHeight;

    CDialogEx::OnGetMinMaxInfo(lpMMI);
}

void CMainDlg::BuildControls()
{
    HWND self = GetSafeHwnd();

    HFONT baseFont = Ui::GetFont(kYahei, 9, false);
    HFONT boldFont = Ui::GetFont(kYahei, 9, true);
    HFONT smallFont = Ui::GetFont(kYahei, 8, false);

    // 对应 this.Text = "{Product} - v{Version} - By：{Company}"
    std::string version = ProgramHost::GetDisplayVersion();
    std::wstring caption = Utf8ToWide(BuildConfig::ProductName) + L" - v" +
                           Utf8ToWide(version) + L" - By：" +
                           Utf8ToWide(BuildConfig::CompanyName);
    SetWindowTextW(caption.c_str());

    // ========== 顶部标题栏（Panel 背景在 OnPaint 中绘制） ==========
    HWND title = CreateChildWnd(self, L"Static", L"🖨️  网络打印客户端", SS_LEFT, 0,
                                18, 10, 400, 24, IDC_PC_TITLE,
                                Ui::GetFont(kYahei, 12, true));
    Ui::SetTextColor(title, kColorWhite);

    HWND subtitle = CreateChildWnd(self, L"Static", L"自动发现 · 一键安装 · 即装即用", SS_LEFT, 0,
                                   18, 32, 500, 16, IDC_PC_SUBTITLE, smallFont);
    Ui::SetTextColor(subtitle, kHeaderSubText);

    // ========== 服务器连接组 ==========
    CreateChildWnd(self, L"Button", L"  🔍  服务器连接", BS_GROUPBOX, 0,
                   12, 60, 520, 105, IDC_PC_GB_CONNECT, boldFont);

    HWND serverLabel = CreateChildWnd(self, L"Static", L"地址/IP:", SS_LEFT, 0,
                                      26, 86, 100, 16, IDC_PC_LBL_SERVER, baseFont);
    Ui::SetTextColor(serverLabel, ::GetSysColor(COLOR_WINDOWTEXT));

    CreateChildWnd(self, WC_COMBOBOXW, L"",
                   CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, 0,
                   82, 83, 340, 106, IDC_PC_CBO_SERVER, baseFont);

    HWND portLabel = CreateChildWnd(self, L"Static", L"端口:", SS_LEFT, 0,
                                    430, 86, 60, 16, IDC_PC_LBL_PORT, baseFont);
    Ui::SetTextColor(portLabel, ::GetSysColor(COLOR_WINDOWTEXT));

    CreateChildWnd(self, L"Edit", L"9180", ES_RIGHT | ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
                   WS_EX_CLIENTEDGE, 468, 83, 60, 24, IDC_PC_NUD_API_PORT, baseFont);

    HWND spin = CreateChildWnd(self, UPDOWN_CLASSW, L"",
                               UDS_ALIGNRIGHT | UDS_SETBUDDYINT | UDS_ARROWKEYS | UDS_NOTHOUSANDS
                               | UDS_WRAP, 0,
                               468, 83, 60, 24, IDC_PC_NUD_API_SPIN, baseFont);
    if (spin != nullptr)
    {
        ::SendMessageW(spin, UDM_SETBUDDY, (WPARAM)::GetDlgItem(self, IDC_PC_NUD_API_PORT), 0);
        ::SendMessageW(spin, UDM_SETRANGE32, 1, 65535);
        ::SendMessageW(spin, UDM_SETPOS32, 0, BuildConfig::DefaultApiPort);
    }

    CreateChildWnd(self, L"Button", L"📡 扫描局域网", BS_OWNERDRAW | WS_TABSTOP, 0,
                   26, 114, 110, 28, IDC_PC_BTN_SCAN, baseFont);
    Ui::MakeFlatButton(this, IDC_PC_BTN_SCAN, kBtnScan, kColorWhite);

    CreateChildWnd(self, L"Button", L"🔗 连接", BS_OWNERDRAW | WS_TABSTOP, 0,
                   144, 114, 90, 28, IDC_PC_BTN_CONNECT, boldFont);
    Ui::MakeFlatButton(this, IDC_PC_BTN_CONNECT, kBtnConnect, kColorWhite);

    HWND connectStatus = CreateChildWnd(self, L"Static", L"就绪 - 请扫描或输入服务器地址",
                                        SS_LEFT, 0, 26, 146, 534, 14,
                                        IDC_PC_LBL_CONNECT_STATUS, smallFont);
    Ui::SetTextColor(connectStatus, kLabelMuted);

    // ========== 打印机列表组 ==========
    CreateChildWnd(self, L"Button", L"  📋  打印机列表（单击选中，双击安装）", BS_GROUPBOX, 0,
                   12, 173, 520, 208, IDC_PC_GB_PRINTER_INFO, boldFont);

    HWND serverInfo = CreateChildWnd(self, L"Static", L"", SS_LEFT | SS_ENDELLIPSIS, 0,
                                     26, 190, 492, 16, IDC_PC_LBL_SERVER_INFO, smallFont);
    Ui::SetTextColor(serverInfo, kServerInfoText);

    HWND list = CreateChildWnd(self, WC_LISTVIEWW, L"",
                               LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                               WS_EX_CLIENTEDGE, 26, 209, 492, 136,
                               IDC_PC_DGV_PRINTERS, baseFont);
    m_printerList.SubclassWindow(list);

    m_printerList.SetExtendedStyle(LVS_EX_GRIDLINES | LVS_EX_FULLROWSELECT);

    // 行高 22（对应 C# RowTemplate.Height = 22）：用 1x22 的图片列表强制行高
    // ILC_COLOR32 要求位图为 32 位 DIB，这里用 CreateDIBSection 创建全透明位图。
    m_rowImages.Create(1, 22, ILC_COLOR32, 1, 1);
    {
        BITMAPINFO bitmapInfo;
        memset(&bitmapInfo, 0, sizeof(bitmapInfo));
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = 1;
        bitmapInfo.bmiHeader.biHeight = 22;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP dib = ::CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib != nullptr)
        {
            if (bits != nullptr)
                memset(bits, 0, 1 * 22 * 4);

            m_rowImages.Add(CBitmap::FromHandle(dib), (COLORREF)CLR_NONE);
            ::DeleteObject(dib);
        }
    }
    m_printerList.SetImageList(&m_rowImages, LVSIL_SMALL);

    // 列宽：对应 DataGridView 的 Fill 权重（名称 38 / 端口 8 / 大小 8 / 状态 12）
    {
        CRect client;
        m_printerList.GetClientRect(&client);
        int total = client.Width();

        int nameWeight = 38, portWeight = 8, sizeWeight = 8, statusWeight = 12;
        int weightSum = nameWeight + portWeight + sizeWeight + statusWeight;

        int nameWidth = total * nameWeight / weightSum;
        int portWidth = total * portWeight / weightSum;
        int sizeColWidth = total * sizeWeight / weightSum;
        int statusWidth = total * statusWeight / weightSum;

        int minimums[4] = { 130, 50, 55, 80 };
        int widths[4] = { nameWidth, portWidth, sizeColWidth, statusWidth };

        for (int i = 0; i < 4; ++i)
        {
            if (widths[i] < minimums[i])
                widths[i] = minimums[i];
        }

        // 让总和精确等于表格宽度
        int sum = widths[0] + widths[1] + widths[2] + widths[3];
        widths[0] += total - sum;

        m_printerList.InsertColumn(0, L"打印机名称", LVCFMT_LEFT, widths[0]);
        m_printerList.InsertColumn(1, L"端口", LVCFMT_CENTER, widths[1]);
        m_printerList.InsertColumn(2, L"大小", LVCFMT_CENTER, widths[2]);
        m_printerList.InsertColumn(3, L"状态", LVCFMT_CENTER, widths[3]);
    }

    // 表头样式（对应 ColumnHeadersDefaultCellStyle：雅黑 9pt 粗体 + 居中）
    CHeaderCtrl* header = m_printerList.GetHeaderCtrl();
    if (header != nullptr)
    {
        ::SendMessageW(header->GetSafeHwnd(), WM_SETFONT,
                       (WPARAM)Ui::GetFont(kYahei, 9, true), MAKELPARAM(TRUE, 0));
        header->Invalidate(TRUE);
    }

    HWND installNameLabel = CreateChildWnd(self, L"Static", L"安装名称:", SS_LEFT, 0,
                                           26, 349, 70, 16, IDC_PC_LBL_INSTALL_NAME, baseFont);
    Ui::SetTextColor(installNameLabel, ::GetSysColor(COLOR_WINDOWTEXT));

    CreateChildWnd(self, L"Edit", L"", ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP,
                   WS_EX_CLIENTEDGE, 96, 347, 280, 22, IDC_PC_TXT_PRINTER_NAME, baseFont);

    // IPP 免驱安装开关：勾上后不下载/不安装厂商驱动
    HWND ippCheck = CreateChildWnd(self, L"Button", L"IPP 免驱安装(免装驱动)",
                                   BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                                   380, 349, 152, 18, IDC_PC_CHK_IPP, smallFont);
    ::SendMessageW(ippCheck, BM_SETCHECK, BST_UNCHECKED, 0);

    // ========== 操作组 ==========
    CreateChildWnd(self, L"Button", L"  ⚡  操作", BS_GROUPBOX, 0,
                   12, 389, 520, 55, IDC_PC_GB_ACTION, boldFont);

    CreateChildWnd(self, L"Button", L"📥 下载并安装选中打印机", BS_OWNERDRAW | WS_TABSTOP, 0,
                   26, 407, 180, 28, IDC_PC_BTN_DOWNLOAD_INSTALL, boldFont);
    Ui::MakeFlatButton(this, IDC_PC_BTN_DOWNLOAD_INSTALL, kBtnDownload, kColorWhite);
    ::EnableWindow(::GetDlgItem(self, IDC_PC_BTN_DOWNLOAD_INSTALL), FALSE);

    CreateChildWnd(self, PROGRESS_CLASSW, L"", PBS_SMOOTH, 0,
                   214, 409, 110, 24, IDC_PC_PROGRESSBAR, baseFont);
    ::SendMessageW(::GetDlgItem(self, IDC_PC_PROGRESSBAR), PBM_SETRANGE32, 0, 100);
    ShowControl(::GetDlgItem(self, IDC_PC_PROGRESSBAR), false);

    HWND progressLabel = CreateChildWnd(self, L"Static", L"", SS_RIGHT, 0,
                                        328, 411, 80, 20, IDC_PC_LBL_PROGRESS, smallFont);
    Ui::SetTextColor(progressLabel, kLabelMuted);

    CreateChildWnd(self, L"Button", L"🗑 清空", BS_OWNERDRAW | WS_TABSTOP, 0,
                   412, 407, 80, 28, IDC_PC_BTN_CLEAR_LOG, smallFont);
    Ui::MakeFlatButton(this, IDC_PC_BTN_CLEAR_LOG, kBtnClearLog, kColorWhite);

    // ========== 运行日志 ==========
    CreateChildWnd(self, L"Button", L"  📝  运行日志", BS_GROUPBOX, 0,
                   12, 452, 520, 160, IDC_PC_GB_LOG, boldFont);

    CreateChildWnd(self, L"Edit", L"",
                   ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | ES_LEFT,
                   0, 26, 474, 492, 128, IDC_PC_TXT_LOG, CreateConsolasFont(85));

    // 对应 gbPrinterInfo.Visible = false
    ShowControl(::GetDlgItem(self, IDC_PC_GB_PRINTER_INFO), false);
    ShowControl(::GetDlgItem(self, IDC_PC_LBL_SERVER_INFO), false);
    ShowControl(::GetDlgItem(self, IDC_PC_DGV_PRINTERS), false);
    ShowControl(::GetDlgItem(self, IDC_PC_LBL_INSTALL_NAME), false);
    ShowControl(::GetDlgItem(self, IDC_PC_TXT_PRINTER_NAME), false);
    ShowControl(::GetDlgItem(self, IDC_PC_CHK_IPP), false);
}

// ===========================================================================
// 绘制
// ===========================================================================

void CMainDlg::OnPaint()
{
    CPaintDC dc(this);
    HDC hdc = dc.GetSafeHdc();

    CRect client;
    GetClientRect(&client);

    // 对应 pnlHeader（Location 0,0 / Size 560x52 / BackColor FromArgb(30,60,120)）
    RECT header = { 0, 0, client.right, 52 };
    HBRUSH brush = ::CreateSolidBrush(kHeaderBack);
    ::FillRect(hdc, &header, brush);
    ::DeleteObject(brush);
}

BOOL CMainDlg::OnEraseBkgnd(CDC* pDC)
{
    CRect client;
    GetClientRect(&client);
    pDC->FillSolidRect(&client, kFormBack);
    return TRUE;
}

void CMainDlg::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct)
{
    if (!Ui::DrawFlatButton(lpDrawItemStruct))
        CDialogEx::OnDrawItem(nIDCtl, lpDrawItemStruct);
}

HBRUSH CMainDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
    HWND control = pWnd->GetSafeHwnd();
    HWND self = GetSafeHwnd();

    if (control == ::GetDlgItem(self, IDC_PC_TXT_LOG))
    {
        // 日志框：深色背景 + 浅绿文字
        pDC->SetBkColor(kLogBack);
        pDC->SetTextColor(kLogText);
        return (HBRUSH)m_logBrush.GetSafeHandle();
    }

    if (nCtlColor == CTLCOLOR_STATIC)
    {
        pDC->SetBkMode(TRANSPARENT);

        COLORREF color = ::GetSysColor(COLOR_WINDOWTEXT);
        Ui::TryGetTextColor(control, color);
        pDC->SetTextColor(color);

        if (control == ::GetDlgItem(self, IDC_PC_TITLE) ||
            control == ::GetDlgItem(self, IDC_PC_SUBTITLE))
        {
            return (HBRUSH)m_headerBrush.GetSafeHandle();
        }

        return (HBRUSH)m_formBrush.GetSafeHandle();
    }

    if (nCtlColor == CTLCOLOR_EDIT || nCtlColor == CTLCOLOR_LISTBOX)
    {
        // 文本框 / 下拉框 / 数字框：WinForms 默认为白底
        pDC->SetBkColor(kColorWhite);
        pDC->SetTextColor(::GetSysColor(COLOR_WINDOWTEXT));
        return (HBRUSH)m_whiteBrush.GetSafeHandle();
    }

    if (nCtlColor == CTLCOLOR_DLG || nCtlColor == CTLCOLOR_BTN)
    {
        // 分组框与其上的文字使用窗体底色，保持 WinForms 的透明观感
        pDC->SetTextColor(kGroupText);
        return (HBRUSH)m_formBrush.GetSafeHandle();
    }

    return CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);
}

// ===========================================================================
// 列表
// ===========================================================================

void CMainDlg::OnPrinterListChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
    LPNMLISTVIEW info = (LPNMLISTVIEW)pNMHDR;

    // 只处理"选中项变为当前行"的情况（对应 DgvPrinters_SelectionChanged）
    if ((info->uNewState & LVIS_SELECTED) != 0 && (info->uOldState & LVIS_SELECTED) == 0)
        ApplySelection(info->iItem);

    *pResult = 0;
}

void CMainDlg::OnPrinterListDoubleClick(NMHDR* pNMHDR, LRESULT* pResult)
{
    LPNMITEMACTIVATE info = (LPNMITEMACTIVATE)pNMHDR;

    if (info->iItem >= 0)
    {
        // 确保当前行被选中（对应 dgvPrinters.Rows[e.RowIndex].Selected = true）
        m_printerList.SetItemState(info->iItem,
                                   LVIS_SELECTED | LVIS_FOCUSED,
                                   LVIS_SELECTED | LVIS_FOCUSED);
        m_printerList.EnsureVisible(info->iItem, FALSE);

        if (m_hasPrinterInfo &&
            info->iItem < (int)m_printerInfo.printers.size() &&
            m_printerInfo.printers[(size_t)info->iItem].hasDriver &&
            ::IsWindowEnabled(::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_DOWNLOAD_INSTALL)))
        {
            DownloadAndInstall(true);
        }
    }

    *pResult = 0;
}

void CMainDlg::OnPrinterListCustomDraw(NMHDR* pNMHDR, LRESULT* pResult)
{
    // 列表与表头的 NM_CUSTOMDRAW 都会送到这里：表头是列表的子控件，
    // 其通知由列表转发给父窗口，用 hwndFrom 区分。
    if (m_printerList.GetSafeHwnd() != nullptr &&
        pNMHDR->hwndFrom != m_printerList.GetSafeHwnd())
    {
        OnPrinterHeaderCustomDraw(pNMHDR, pResult);
        return;
    }

    LPNMLVCUSTOMDRAW draw = (LPNMLVCUSTOMDRAW)pNMHDR;

    switch (draw->nmcd.dwDrawStage)
    {
    case CDDS_PREPAINT:
        *pResult = CDRF_NOTIFYITEMDRAW;
        return;

    case CDDS_ITEMPREPAINT:
        if ((draw->nmcd.uItemState & CDIS_SELECTED) != 0)
        {
            // 对应 DefaultCellStyle.SelectionBackColor / SelectionForeColor
            draw->clrTextBk = kSelectionBack;
            draw->clrText = kColorBlack;
        }
        else
        {
            draw->clrTextBk = kColorWhite;
        }

        *pResult = CDRF_NEWFONT;
        return;

    default:
        break;
    }

    *pResult = CDRF_DODEFAULT;
}

// ===========================================================================
// 状态
// ===========================================================================

void CMainDlg::SetConnectStatus(const std::wstring& text, COLORREF color)
{
    HWND label = ::GetDlgItem(GetSafeHwnd(), IDC_PC_LBL_CONNECT_STATUS);
    if (label == nullptr)
        return;

    ::SetWindowTextW(label, text.c_str());
    Ui::SetTextColor(label, color);
}

void CMainDlg::SetProgress(int percent, const std::wstring& label)
{
    HWND bar = ::GetDlgItem(GetSafeHwnd(), IDC_PC_PROGRESSBAR);
    if (bar != nullptr)
        ::SendMessageW(bar, PBM_SETPOS, (WPARAM)percent, 0);

    HWND progressLabel = ::GetDlgItem(GetSafeHwnd(), IDC_PC_LBL_PROGRESS);
    if (progressLabel != nullptr)
        ::SetWindowTextW(progressLabel, label.c_str());
}

void CMainDlg::FillPrinterList()
{
    m_printerList.DeleteAllItems();

    for (size_t i = 0; i < m_printerInfo.printers.size(); ++i)
    {
        const PrinterItemInfo& item = m_printerInfo.printers[i];

        std::wstring status = item.hasDriver ? L"✅ 驱动可用" : L"⚠ 未找到驱动";

        int row = m_printerList.InsertItem((int)i, Utf8ToWide(item.name).c_str());
        m_printerList.SetItemText(row, 1, std::to_wstring(item.port).c_str());
        m_printerList.SetItemText(row, 2,
                                  Utf8ToWide(item.driverSize.empty() ? std::string("-")
                                                                     : item.driverSize).c_str());
        m_printerList.SetItemText(row, 3, status.c_str());
    }
}

void CMainDlg::ApplySelection(int index)
{
    if (!m_hasPrinterInfo || index < 0 || index >= (int)m_printerInfo.printers.size())
        return;

    const PrinterItemInfo& selected = m_printerInfo.printers[(size_t)index];

    // 更新 _printerInfo 默认信息（用于下载安装）
    m_printerInfo.printerName = selected.name;
    m_printerInfo.tcpPort = selected.port;
    m_printerInfo.driverName = selected.driverName;
    m_printerInfo.driverVersion = selected.driverVersion;
    m_printerInfo.driverDate = selected.driverDate;
    m_printerInfo.manufacturer = selected.manufacturer;
    m_printerInfo.hasDriver = selected.hasDriver;
    m_printerInfo.driverFileCount = selected.driverFileCount;
    m_printerInfo.driverSize = selected.driverSize;

    // 对应 selected.Name.Trim() + "(共享)"
    std::wstring name = Utf8ToWide(TrimAscii(selected.name));
    ::SetWindowTextW(::GetDlgItem(GetSafeHwnd(), IDC_PC_TXT_PRINTER_NAME),
                     (name + L"(共享)").c_str());

    UpdateButtonState();
}

void CMainDlg::UpdateButtonState()
{
    HWND button = ::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_DOWNLOAD_INSTALL);
    if (button == nullptr)
        return;

    int index = SelectedPrinterIndex();
    if (!m_hasPrinterInfo || index < 0 || index >= (int)m_printerInfo.printers.size())
    {
        ::EnableWindow(button, FALSE);
        return;
    }

    ::EnableWindow(button, m_printerInfo.printers[(size_t)index].hasDriver ? TRUE : FALSE);
}

int CMainDlg::SelectedPrinterIndex() const
{
    if (m_printerList.GetSafeHwnd() == nullptr)
        return -1;

    POSITION position = m_printerList.GetFirstSelectedItemPosition();
    if (position == nullptr)
        return -1;

    return m_printerList.GetNextSelectedItem(position);
}

// ===========================================================================
// 日志
// ===========================================================================

void CMainDlg::AppendLogLineInternal(const std::string& message)
{
    HWND log = ::GetDlgItem(GetSafeHwnd(), IDC_PC_TXT_LOG);
    if (log == nullptr)
        return;

    std::wstring line = Utf8ToWide(message) + L"\r\n";

    int length = ::GetWindowTextLengthW(log);
    ::SendMessageW(log, EM_SETSEL, length, length);
    ::SendMessageW(log, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());

    // 自动滚动到底部
    ::SendMessageW(log, EM_SCROLLCARET, 0, 0);
}

void CMainDlg::TrimLogLines()
{
    HWND log = ::GetDlgItem(GetSafeHwnd(), IDC_PC_TXT_LOG);
    if (log == nullptr)
        return;

    std::wstring text = GetWindowTextString(log);
    std::vector<std::wstring> lines;

    {
        std::wstring current;
        for (size_t i = 0; i < text.size(); ++i)
        {
            wchar_t ch = text[i];

            if (ch == L'\r' || ch == L'\n')
            {
                lines.push_back(current);
                current.clear();

                if (ch == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n')
                    ++i;
            }
            else
            {
                current.push_back(ch);
            }
        }

        // 对应 TextBoxBase.Lines：结尾的换行不产生额外的空行
        if (!current.empty())
            lines.push_back(current);
    }

    // 对应 `if (txtLog.Lines.Length > 500)` 时保留最后 200 行
    if (lines.size() <= 500)
        return;

    size_t start = lines.size() - 200;
    std::wstring rebuilt;
    for (size_t i = start; i < lines.size(); ++i)
    {
        if (!rebuilt.empty())
            rebuilt += L"\r\n";
        rebuilt += lines[i];
    }

    ::SetWindowTextW(log, rebuilt.c_str());

    int length = ::GetWindowTextLengthW(log);
    ::SendMessageW(log, EM_SETSEL, length, length);
    ::SendMessageW(log, EM_SCROLLCARET, 0, 0);
}

void CMainDlg::Log(const std::string& message)
{
    SYSTEMTIME now;
    ::GetLocalTime(&now);

    char stamp[64] = { 0 };
    _snprintf_s(stamp, sizeof(stamp), _TRUNCATE, "[%02d:%02d:%02d] ",
                now.wHour, now.wMinute, now.wSecond);

    AppendLogLineInternal(std::string(stamp) + message);
    TrimLogLines();
}

// ===========================================================================
// 本地网段预填（对应 MainForm.LoadLocalSubnets）
// ===========================================================================

void CMainDlg::LoadLocalSubnets()
{
    HWND combo = ::GetDlgItem(GetSafeHwnd(), IDC_PC_CBO_SERVER);

    try
    {
        std::vector<std::string> prefixes = GetLocalSubnetPrefixes();

        ::SendMessageW(combo, CB_RESETCONTENT, 0, 0);

        if (!prefixes.empty())
        {
            for (const std::string& prefix : prefixes)
            {
                std::wstring widePrefix = Utf8ToWide(prefix);
                std::wstring first = widePrefix + L"1";
                std::wstring last = widePrefix + L"254";

                ::SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)first.c_str());
                ::SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)last.c_str());
            }

            // 不自动选中，仅填充建议列表
            ::SendMessageW(combo, CB_SETCURSEL, (WPARAM)-1, 0);
            ::SetWindowTextW(combo, L"");

            std::wstring joined;
            for (size_t i = 0; i < prefixes.size(); ++i)
            {
                if (i > 0)
                    joined += L", ";
                joined += Utf8ToWide(prefixes[i]);
            }

            SetConnectStatus(L"就绪 - 已检测到本地网段 " + joined + L"*", kStatusGreen);
        }
        else
        {
            ::SetWindowTextW(combo, L"");
            SetConnectStatus(L"就绪 - 未检测到局域网，请手动输入服务器地址", kStatusOrange);
        }
    }
    catch (...)
    {
        ::SetWindowTextW(combo, L"");
        SetConnectStatus(L"就绪 - 请扫描或输入服务器地址", kLabelMuted);
    }
}

// ===========================================================================
// 服务器地址
// ===========================================================================

int CMainDlg::ReadApiPort() const
{
    HWND edit = ::GetDlgItem(GetSafeHwnd(), IDC_PC_NUD_API_PORT);
    int port = _wtoi(GetWindowTextString(edit).c_str());

    if (port < 1)
        port = 1;
    if (port > 65535)
        port = 65535;

    return port;
}

std::string CMainDlg::ReadServerAddress() const
{
    std::string raw = TrimAscii(WideToUtf8(
        GetWindowTextString(::GetDlgItem(GetSafeHwnd(), IDC_PC_CBO_SERVER))));

    size_t space = raw.find(' ');
    if (space != std::string::npos && space > 0)
        return TrimAscii(raw.substr(0, space));

    return raw;
}

std::string CMainDlg::CleanHostname(const std::string& hostname) const
{
    // 对应 string.IsNullOrEmpty 时原样返回
    if (hostname.empty())
        return hostname;

    size_t dot = hostname.find('.');
    if (dot != std::string::npos && dot > 0)
        return hostname.substr(0, dot);

    return hostname;
}

// ===========================================================================
// 扫描
// ===========================================================================

void CMainDlg::OnScanClicked()
{
    if (m_isScanning)
        return;

    HWND scanButton = ::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_SCAN);
    HWND connectButton = ::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_CONNECT);
    HWND combo = ::GetDlgItem(GetSafeHwnd(), IDC_PC_CBO_SERVER);

    m_isScanning = true;
    ::EnableWindow(scanButton, FALSE);
    ::EnableWindow(connectButton, FALSE);
    Ui::SetButtonText(this, IDC_PC_BTN_SCAN, L"⏳ 扫描中...");

    ::SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    ::SetWindowTextW(combo, L"");

    SetConnectStatus(L"正在扫描局域网...", kLabelMuted);

    int apiPort = ReadApiPort();

    {
        char message[128] = { 0 };
        _snprintf_s(message, sizeof(message), _TRUNCATE,
                    "📡 开始扫描局域网 (端口 %d)...", apiPort);
        Log(message);
    }

    HWND self = GetSafeHwnd();

    std::thread([self, apiPort]() {
        std::vector<ScanResult> results = ScanAsync(apiPort, [self](const std::string& message) {
            ScanProgressMsg* progress = new ScanProgressMsg();
            progress->message = message;
            ::PostMessageW(self, kMsgScanProgress, 0, (LPARAM)progress);
        });

        ScanDoneMsg* done = new ScanDoneMsg();
        done->results = results;
        ::PostMessageW(self, kMsgScanDone, 0, (LPARAM)done);
    }).detach();
}

LRESULT CMainDlg::OnScanProgress(WPARAM, LPARAM lParam)
{
    ScanProgressMsg* progress = (ScanProgressMsg*)lParam;
    if (progress == nullptr)
        return 0;

    Log("  " + progress->message);

    HWND label = ::GetDlgItem(GetSafeHwnd(), IDC_PC_LBL_CONNECT_STATUS);
    ::SetWindowTextW(label, Utf8ToWide(progress->message).c_str());

    delete progress;
    return 0;
}

LRESULT CMainDlg::OnScanDone(WPARAM, LPARAM lParam)
{
    ScanDoneMsg* done = (ScanDoneMsg*)lParam;
    if (done == nullptr)
        return 0;

    HWND scanButton = ::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_SCAN);
    HWND connectButton = ::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_CONNECT);
    HWND combo = ::GetDlgItem(GetSafeHwnd(), IDC_PC_CBO_SERVER);

    try
    {
        m_isScanning = false;

        for (const ScanResult& result : done->results)
        {
            std::string hostname = CleanHostname(result.hostname);
            std::string display = result.ipAddress;

            if (!hostname.empty() && hostname != result.ipAddress)
                display += "  (" + hostname + ")";

            ::SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)Utf8ToWide(display).c_str());
        }

        // 对应 cboServerAddress.Items.Count
        int itemCount = (int)::SendMessageW(combo, CB_GETCOUNT, 0, 0);

        if (itemCount > 0)
        {
            ::SendMessageW(combo, CB_SETCURSEL, 0, 0);

            wchar_t message[256] = { 0 };
            _snwprintf_s(message, _countof(message), _TRUNCATE,
                         L"✅ 扫描完成，发现 %d 台服务器，自动连接中...",
                         (int)done->results.size());
            SetConnectStatus(message, kStatusGreen);

            // 扫描到后自动连接第一台服务器，显示打印机信息
            ConnectToServer(done->results[0].ipAddress, ReadApiPort());
        }
        else
        {
            SetConnectStatus(L"未发现打印服务器，请手动输入地址", kStatusOrange);
        }
    }
    catch (const std::exception& ex)
    {
        SetConnectStatus(L"扫描出错: " + Utf8ToWide(ex.what()), kStatusRed);
        Log(std::string("❌ 扫描错误: ") + ex.what());
    }
    catch (...)
    {
        SetConnectStatus(L"扫描出错: 未知错误", kStatusRed);
        Log("❌ 扫描错误: 未知错误");
    }

    m_isScanning = false;
    ::EnableWindow(scanButton, TRUE);
    ::EnableWindow(connectButton, TRUE);
    Ui::SetButtonText(this, IDC_PC_BTN_SCAN, L"📡 扫描局域网");

    delete done;
    return 0;
}

// ===========================================================================
// 连接
// ===========================================================================

void CMainDlg::OnConnectClicked()
{
    std::string server = ReadServerAddress();

    if (server.empty())
    {
        ::MessageBoxW(GetSafeHwnd(), L"请输入或选择服务器地址！", L"提示",
                      MB_OK | MB_ICONWARNING);
        return;
    }

    ConnectToServer(server, ReadApiPort());
}

void CMainDlg::ConnectToServer(const std::string& server, int apiPort)
{
    HWND connectButton = ::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_CONNECT);
    HWND printerGroup = ::GetDlgItem(GetSafeHwnd(), IDC_PC_GB_PRINTER_INFO);

    ::EnableWindow(connectButton, FALSE);
    Ui::SetButtonText(this, IDC_PC_BTN_CONNECT, L"⏳ 连接中...");
    SetConnectStatus(L"正在连接...", kColorGray);

    ShowControl(printerGroup, false);
    ShowControl(::GetDlgItem(GetSafeHwnd(), IDC_PC_LBL_SERVER_INFO), false);
    ShowControl(::GetDlgItem(GetSafeHwnd(), IDC_PC_DGV_PRINTERS), false);
    ShowControl(::GetDlgItem(GetSafeHwnd(), IDC_PC_LBL_INSTALL_NAME), false);
    ShowControl(::GetDlgItem(GetSafeHwnd(), IDC_PC_TXT_PRINTER_NAME), false);
    ShowControl(::GetDlgItem(GetSafeHwnd(), IDC_PC_CHK_IPP), false);

    m_hasPrinterInfo = false;
    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_DOWNLOAD_INSTALL), FALSE);

    Log("正在连接 " + server + ":" + std::to_string(apiPort) + " ...");

    HWND self = GetSafeHwnd();

    std::thread([self, server, apiPort]() {
        DriverDownloader downloader(server, apiPort);
        bool connected = downloader.TestConnection();

        ConnectTestMsg* message = new ConnectTestMsg();
        message->connected = connected;
        message->server = server;
        message->apiPort = apiPort;
        ::PostMessageW(self, kMsgConnectTested, 0, (LPARAM)message);
    }).detach();
}

LRESULT CMainDlg::OnConnectTested(WPARAM, LPARAM lParam)
{
    ConnectTestMsg* message = (ConnectTestMsg*)lParam;
    if (message == nullptr)
        return 0;

    bool connected = message->connected;
    std::string server = message->server;
    int apiPort = message->apiPort;
    delete message;

    HWND connectButton = ::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_CONNECT);

    if (!connected)
    {
        SetConnectStatus(L"❌ 无法连接到服务器，请检查地址和端口", kStatusRed);
        Log("❌ 连接失败");

        ::EnableWindow(connectButton, TRUE);
        Ui::SetButtonText(this, IDC_PC_BTN_CONNECT, L"🔗 连接");
        return 0;
    }

    Log("✅ 服务器连接成功，正在获取打印机信息...");

    HWND self = GetSafeHwnd();

    std::thread([self, server, apiPort]() {
        DriverDownloader downloader(server, apiPort);

        ConnectInfoMsg* info = new ConnectInfoMsg();
        info->ok = downloader.GetPrinterInfo(info->info, info->error);
        ::PostMessageW(self, kMsgConnectInfo, 0, (LPARAM)info);
    }).detach();

    return 0;
}

LRESULT CMainDlg::OnConnectInfo(WPARAM, LPARAM lParam)
{
    ConnectInfoMsg* message = (ConnectInfoMsg*)lParam;
    if (message == nullptr)
        return 0;

    HWND connectButton = ::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_CONNECT);

    try
    {
        if (!message->ok)
        {
            SetConnectStatus(L"❌ 连接错误: " + Utf8ToWide(message->error), kStatusRed);
            Log("❌ 错误: " + message->error);
        }
        else
        {
            m_printerInfo = message->info;
            m_hasPrinterInfo = true;

            // 更新服务器信息
            std::wstring hostname = Utf8ToWide(CleanHostname(m_printerInfo.hostname));
            std::wstring version = m_printerInfo.version.empty()
                                       ? L"?"
                                       : Utf8ToWide(m_printerInfo.version);

            wchar_t info[512] = { 0 };
            _snwprintf_s(info, _countof(info), _TRUNCATE,
                         L"🖥️ %s  |  v%s  |  %d 台打印机",
                         hostname.c_str(), version.c_str(),
                         (int)m_printerInfo.printers.size());
            Ui::SetLabelText(this, IDC_PC_LBL_SERVER_INFO, info);

            FillPrinterList();

            // 对应 WinForms：DataGridView 加入首行后会自动成为 CurrentRow 并选中，
            // 从而触发 DgvPrinters_SelectionChanged（填写"安装名称"、启用下载按钮）。
            if (!m_printerInfo.printers.empty())
            {
                m_printerList.SetItemState(0, LVIS_SELECTED | LVIS_FOCUSED,
                                           LVIS_SELECTED | LVIS_FOCUSED);
                ApplySelection(0);
            }

            ShowControl(::GetDlgItem(GetSafeHwnd(), IDC_PC_GB_PRINTER_INFO), true);
            ShowControl(::GetDlgItem(GetSafeHwnd(), IDC_PC_LBL_SERVER_INFO), true);
            ShowControl(::GetDlgItem(GetSafeHwnd(), IDC_PC_DGV_PRINTERS), true);
            ShowControl(::GetDlgItem(GetSafeHwnd(), IDC_PC_LBL_INSTALL_NAME), true);
            ShowControl(::GetDlgItem(GetSafeHwnd(), IDC_PC_TXT_PRINTER_NAME), true);
            ShowControl(::GetDlgItem(GetSafeHwnd(), IDC_PC_CHK_IPP), true);

            UpdateButtonState();

            wchar_t status[256] = { 0 };
            _snwprintf_s(status, _countof(status), _TRUNCATE,
                         L"✅ 已连接 - %d 台打印机可用",
                         (int)m_printerInfo.printers.size());
            SetConnectStatus(status, kColorGreen);

            char logMessage[256] = { 0 };
            _snprintf_s(logMessage, sizeof(logMessage), _TRUNCATE,
                        "✅ 打印机信息获取成功 - %d 台打印机",
                        (int)m_printerInfo.printers.size());
            Log(logMessage);
        }
    }
    catch (const std::exception& ex)
    {
        SetConnectStatus(L"❌ 连接错误: " + Utf8ToWide(ex.what()), kStatusRed);
        Log(std::string("❌ 错误: ") + ex.what());
    }
    catch (...)
    {
        SetConnectStatus(L"❌ 连接错误: 未知错误", kStatusRed);
        Log("❌ 错误: 未知错误");
    }

    ::EnableWindow(connectButton, TRUE);
    Ui::SetButtonText(this, IDC_PC_BTN_CONNECT, L"🔗 连接");

    delete message;
    return 0;
}

// ===========================================================================
// 下载并安装
// ===========================================================================

void CMainDlg::OnDownloadInstallClicked()
{
    DownloadAndInstall(true);
}

void CMainDlg::OnClearLogClicked()
{
    HWND log = ::GetDlgItem(GetSafeHwnd(), IDC_PC_TXT_LOG);
    ::SetWindowTextW(log, L"");
    Log("日志已清空");
}

std::string CMainDlg::ShowAddressSelector(const std::string& hostname, const std::string& ip)
{
    CAddressDlg dialog(hostname, ip, this);
    if (dialog.DoModal() == IDYES)
        return hostname;

    return ip;
}

// ===========================================================================
// IPP 免驱安装
// ===========================================================================

bool CMainDlg::IsIppInstallSelected() const
{
    HWND box = ::GetDlgItem(GetSafeHwnd(), IDC_PC_CHK_IPP);
    if (box == nullptr)
        return false;

    return ::SendMessageW(box, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void CMainDlg::StartIppInstall(int selectedIndex)
{
    m_busy = true;

    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_DOWNLOAD_INSTALL), FALSE);
    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_CONNECT), FALSE);
    ShowControl(::GetDlgItem(GetSafeHwnd(), IDC_PC_PROGRESSBAR), true);
    SetProgress(50, L"正在安装(IPP 免驱)...");

    // 与常规安装保持一致：主机名与 IP 不同时让用户选一个
    std::string server = ReadServerAddress();
    std::string connectAddress = server;
    std::string cleanHost = CleanHostname(m_printerInfo.hostname);

    if (!cleanHost.empty() && _stricmp(cleanHost.c_str(), server.c_str()) != 0)
        connectAddress = ShowAddressSelector(cleanHost, server);

    std::wstring printerNameText =
        GetWindowTextString(::GetDlgItem(GetSafeHwnd(), IDC_PC_TXT_PRINTER_NAME));
    std::string printerName = WideToUtf8(TrimWide(printerNameText));

    if (printerName.empty())
    {
        Log("❌ 安装名称不能为空");
        SetProgress(0, L"❌ 失败");
        m_busy = false;
        ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_DOWNLOAD_INSTALL), TRUE);
        ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_CONNECT), TRUE);
        ShowControl(::GetDlgItem(GetSafeHwnd(), IDC_PC_PROGRESSBAR), false);
        return;
    }

    Log("⚡ 使用 IPP 免驱安装：不下载驱动包、不安装厂商驱动");
    Log("   服务端打印机序号: #" + std::to_string(selectedIndex));
    Log("   （文档由服务端渲染后交给本机驱动输出，客户端只需系统自带的 IPP 类驱动）");

    Ui::SetLabelText(this, IDC_PC_LBL_PROGRESS, L"正在安装(IPP 免驱)...");

    HWND self = GetSafeHwnd();

    std::thread([self, connectAddress, printerName, selectedIndex]() {
        InstallDoneMsg* done = new InstallDoneMsg();

        done->result = PrinterDriverInstaller::InstallPrinterViaIpp(
            connectAddress, 631, selectedIndex, printerName,
            [self](const std::string& message) {
                InstallProgressMsg* progress = new InstallProgressMsg();
                progress->message = message;
                ::PostMessageW(self, kMsgInstallProgress, 0, (LPARAM)progress);
            });

        ::PostMessageW(self, kMsgInstallDone, 0, (LPARAM)done);
    }).detach();
}

void CMainDlg::DownloadAndInstall(bool doInstall)
{
    if (m_busy)
        return;

    if (!m_hasPrinterInfo)
    {
        ::MessageBoxW(GetSafeHwnd(), L"请先连接服务器！", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    int selectedIndex = SelectedPrinterIndex();
    if (selectedIndex < 0)
    {
        ::MessageBoxW(GetSafeHwnd(), L"请先在打印机列表中选中一台打印机！", L"提示",
                      MB_OK | MB_ICONWARNING);
        return;
    }

    if (selectedIndex >= (int)m_printerInfo.printers.size())
    {
        ::MessageBoxW(GetSafeHwnd(), L"选中的打印机无效，请重新选择！", L"错误",
                      MB_OK | MB_ICONERROR);
        return;
    }

    // 服务端返回的 index 是"稳定槽位"，IPP 端点 /ipp/print/N 与
    // /api/driver?id=N 都用它；列表行号只是界面顺序，不能直接当编号用。
    const int serverIndex = m_printerInfo.printers[(size_t)selectedIndex].index;

    // 勾选了「IPP 免驱安装」时，完全跳过"下载驱动包 → 装驱动 → 建端口"这条链路，
    // 直接用系统自带的 IPP 类驱动建队列（也因此不需要为装驱动提权）。
    if (IsIppInstallSelected())
    {
        StartIppInstall(serverIndex);
        return;
    }

    m_busy = true;

    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_DOWNLOAD_INSTALL), FALSE);
    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_CONNECT), FALSE);
    ShowControl(::GetDlgItem(GetSafeHwnd(), IDC_PC_PROGRESSBAR), true);
    SetProgress(0, L"");

    try
    {
        std::string server = ReadServerAddress();
        int apiPort = ReadApiPort();

        // 驱动缓存目录：程序目录\drivers
        std::wstring cacheDir = PathCombine(ProgramHost::GetExecutableDirectory(), L"drivers");
        EnsureDirectory(cacheDir);

        // 缓存文件名：服务器IP_端口_打印机名.zip
        std::string cacheFileName = server + "_" + std::to_string(apiPort) + "_" +
                                    SanitizeFileName(m_printerInfo.printerName) + ".zip";
        std::wstring cachePath = PathCombine(cacheDir, Utf8ToWide(cacheFileName));

        // 创建临时解压目录
        std::wstring tempDir = PathCombine(GetTempDirectory(), L"PrintClient_Driver");
        if (DirectoryExists(tempDir))
            RemoveDirectoryTree(tempDir);
        EnsureDirectory(tempDir);

        std::wstring expectedZipPath = PathCombine(tempDir, L"printer_driver.zip");

        if (FileExists(cachePath))
        {
            long long size = GetFileSize(cachePath);

            Log("📦 发现已缓存的驱动文件，跳过下载 (" + FormatSize(size) + ")");
            Log("  缓存位置: " + WideToUtf8(cachePath));

            CopyFileOverwrite(cachePath, expectedZipPath);

            SetProgress(100, L"已使用缓存");

            ContinueInstallAfterDownload(WideToUtf8(expectedZipPath), WideToUtf8(cachePath),
                                         WideToUtf8(tempDir), serverIndex, doInstall);
        }
        else
        {
            Log("📥 开始下载驱动文件...");

            HWND self = GetSafeHwnd();
            std::wstring wideTempDir = tempDir;
            std::wstring wideCachePath = cachePath;

            std::thread([self, server, apiPort, wideTempDir, wideCachePath, serverIndex, doInstall]() {
                DriverDownloader downloader(server, apiPort);

                DownloadDoneMsg* done = new DownloadDoneMsg();
                done->tempDir = WideToUtf8(wideTempDir);
                done->cachePath = WideToUtf8(wideCachePath);
                done->selectedIndex = serverIndex;
                done->doInstall = doInstall;

                done->ok = downloader.DownloadDriver(
                    done->tempDir, serverIndex,
                    [self](int percent) {
                        ProgressMsg* progress = new ProgressMsg();
                        progress->percent = percent;
                        ::PostMessageW(self, kMsgDownloadProgress, 0, (LPARAM)progress);
                    },
                    done->zipPath, done->error);

                ::PostMessageW(self, kMsgDownloadDone, 0, (LPARAM)done);
            }).detach();
        }
    }
    catch (const std::exception& ex)
    {
        Log(std::string("❌ 操作失败: ") + ex.what());
        SetProgress(0, L"❌ 失败");
        ::MessageBoxW(GetSafeHwnd(), (L"操作失败: " + Utf8ToWide(ex.what())).c_str(), L"错误",
                      MB_OK | MB_ICONERROR);
        FinishDownloadAndInstall();
    }
    catch (...)
    {
        Log("❌ 操作失败: 未知错误");
        SetProgress(0, L"❌ 失败");
        ::MessageBoxW(GetSafeHwnd(), L"操作失败: 未知错误", L"错误", MB_OK | MB_ICONERROR);
        FinishDownloadAndInstall();
    }
}

LRESULT CMainDlg::OnDownloadProgress(WPARAM, LPARAM lParam)
{
    ProgressMsg* progress = (ProgressMsg*)lParam;
    if (progress == nullptr)
        return 0;

    int percent = progress->percent;
    delete progress;

    wchar_t label[64] = { 0 };
    _snwprintf_s(label, _countof(label), _TRUNCATE, L"下载中: %d%%", percent);
    SetProgress(percent, label);

    if (percent % 20 == 0)
    {
        char message[64] = { 0 };
        _snprintf_s(message, sizeof(message), _TRUNCATE, "  下载进度: %d%%", percent);
        Log(message);
    }

    return 0;
}

LRESULT CMainDlg::OnDownloadDone(WPARAM, LPARAM lParam)
{
    DownloadDoneMsg* done = (DownloadDoneMsg*)lParam;
    if (done == nullptr)
        return 0;

    if (!done->ok)
    {
        Log("❌ 操作失败: " + done->error);
        SetProgress(0, L"❌ 失败");
        ::MessageBoxW(GetSafeHwnd(), (L"操作失败: " + Utf8ToWide(done->error)).c_str(), L"错误",
                      MB_OK | MB_ICONERROR);
        delete done;
        FinishDownloadAndInstall();
        return 0;
    }

    Log("✅ 驱动下载完成: " + FormatSize(GetFileSize(Utf8ToWide(done->zipPath))));

    // 保存到缓存
    if (CopyFileOverwrite(Utf8ToWide(done->zipPath), Utf8ToWide(done->cachePath)))
    {
        Log("💾 驱动已缓存到: " + done->cachePath);
    }
    else
    {
        DWORD error = ::GetLastError();
        Log("⚠ 缓存保存失败: 错误码 " + std::to_string(error));
    }

    std::string zipPath = done->zipPath;
    std::string cachePath = done->cachePath;
    std::string tempDir = done->tempDir;
    int selectedIndex = done->selectedIndex;
    bool doInstall = done->doInstall;
    delete done;

    ContinueInstallAfterDownload(zipPath, cachePath, tempDir, selectedIndex, doInstall);
    return 0;
}

void CMainDlg::ContinueInstallAfterDownload(const std::string& zipPath,
                                            const std::string& cachePath,
                                            const std::string& tempDir,
                                            int selectedIndex,
                                            bool doInstall)
{
    UNREFERENCED_PARAMETER(selectedIndex);

    if (!doInstall)
    {
        // 仅下载：驱动已在上面保存到缓存目录
        Log("💾 驱动已保存到: " + cachePath);
        Ui::SetLabelText(this, IDC_PC_LBL_PROGRESS, L"驱动已保存");

        std::wstring text = L"驱动已保存！\n\n" + Utf8ToWide(cachePath);
        ::MessageBoxW(GetSafeHwnd(), text.c_str(), L"下载完成", MB_OK | MB_ICONINFORMATION);

        FinishDownloadAndInstall();
        return;
    }

    {
        // 解压驱动
        Log("📂 正在解压驱动文件...");
        std::wstring extractDir = PathCombine(Utf8ToWide(tempDir), L"driver");

        std::string error;
        if (!DriverDownloader::ExtractDriver(zipPath, WideToUtf8(extractDir), error))
        {
            Log("❌ 操作失败: " + error);
            SetProgress(0, L"❌ 失败");
            ::MessageBoxW(GetSafeHwnd(), (L"操作失败: " + Utf8ToWide(error)).c_str(), L"错误",
                          MB_OK | MB_ICONERROR);
            FinishDownloadAndInstall();
            return;
        }

        Log("✅ 解压完成");

        // 安装驱动和打印机
        Log("🔧 开始安装打印机驱动...");

        // 让用户选择用主机名还是IP地址创建打印机
        std::string server = ReadServerAddress();
        std::string connectAddress = server;
        std::string cleanHost = CleanHostname(m_printerInfo.hostname);

        if (!cleanHost.empty() && _stricmp(cleanHost.c_str(), server.c_str()) != 0)
            connectAddress = ShowAddressSelector(cleanHost, server);

        Ui::SetLabelText(this, IDC_PC_LBL_PROGRESS, L"正在安装...");

        std::string driverDirectory = WideToUtf8(extractDir);
        int tcpPort = m_printerInfo.tcpPort;
        std::string driverName = m_printerInfo.driverName;

        std::wstring printerNameText = GetWindowTextString(
            ::GetDlgItem(GetSafeHwnd(), IDC_PC_TXT_PRINTER_NAME));
        std::string printerName = WideToUtf8(TrimWide(printerNameText));

        HWND self = GetSafeHwnd();

        std::thread([self, driverDirectory, connectAddress, tcpPort, printerName, driverName]() {
            InstallDoneMsg* done = new InstallDoneMsg();

            done->result = PrinterDriverInstaller::InstallPrinter(
                driverDirectory, connectAddress, tcpPort, printerName, driverName,
                [self](const std::string& message) {
                    InstallProgressMsg* progress = new InstallProgressMsg();
                    progress->message = message;
                    ::PostMessageW(self, kMsgInstallProgress, 0, (LPARAM)progress);
                });

            ::PostMessageW(self, kMsgInstallDone, 0, (LPARAM)done);
        }).detach();
    }
}

LRESULT CMainDlg::OnInstallProgress(WPARAM, LPARAM lParam)
{
    InstallProgressMsg* progress = (InstallProgressMsg*)lParam;
    if (progress == nullptr)
        return 0;

    Log("  " + progress->message);
    Ui::SetLabelText(this, IDC_PC_LBL_PROGRESS, Utf8ToWide(progress->message).c_str());

    delete progress;
    return 0;
}

LRESULT CMainDlg::OnInstallDone(WPARAM, LPARAM lParam)
{
    InstallDoneMsg* done = (InstallDoneMsg*)lParam;
    if (done == nullptr)
        return 0;

    InstallResult result = done->result;
    delete done;

    // 显示安装步骤
    for (const std::string& step : result.steps)
        Log("  " + step);

    if (result.success)
    {
        Log("🎉 " + result.message);
        Ui::SetLabelText(this, IDC_PC_LBL_PROGRESS, L"✅ 安装成功！");
        SetProgress(100, L"✅ 安装成功！");

        std::wstring text = Utf8ToWide(result.message) + L"\n\n打印机名称: " +
                            Utf8ToWide(result.printerName) + L"\n端口: " +
                            Utf8ToWide(result.portName);
        ::MessageBoxW(GetSafeHwnd(), text.c_str(), L"安装成功", MB_OK | MB_ICONINFORMATION);
    }
    else
    {
        Log("❌ " + result.message);
        Ui::SetLabelText(this, IDC_PC_LBL_PROGRESS, L"❌ 安装失败");

        std::wstring text = Utf8ToWide(result.message) + L"\n\n请确保以管理员身份运行此程序！";
        ::MessageBoxW(GetSafeHwnd(), text.c_str(), L"安装失败", MB_OK | MB_ICONERROR);
    }

    FinishDownloadAndInstall();
    return 0;
}

void CMainDlg::FinishDownloadAndInstall()
{
    m_busy = false;
    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_DOWNLOAD_INSTALL), TRUE);
    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PC_BTN_CONNECT), TRUE);
    ShowControl(::GetDlgItem(GetSafeHwnd(), IDC_PC_PROGRESSBAR), false);
}

// ===========================================================================
// 表头自绘（单独注册，避免与列表的 NM_CUSTOMDRAW 冲突）
// ===========================================================================

void CMainDlg::OnPrinterHeaderCustomDraw(NMHDR* pNMHDR, LRESULT* pResult)
{
    // 对应 ColumnHeadersDefaultCellStyle：
    //   BackColor FromArgb(240,242,245)、ForeColor FromArgb(50,50,80)、雅黑 9pt 粗体、居中
    //
    // 表头控件在 NM_CUSTOMDRAW 中不支持 clrTextBk，因此这里自绘整个表头单元格。
    LPNMCUSTOMDRAW draw = (LPNMCUSTOMDRAW)pNMHDR;

    switch (draw->dwDrawStage)
    {
    case CDDS_PREPAINT:
        *pResult = CDRF_NOTIFYITEMDRAW;
        return;

    case CDDS_ITEMPREPAINT:
    {
        CHeaderCtrl* header = m_printerList.GetHeaderCtrl();
        if (header == nullptr)
        {
            *pResult = CDRF_DODEFAULT;
            return;
        }

        // 底色
        HBRUSH brush = ::CreateSolidBrush(kGridHeaderBack);
        ::FillRect(draw->hdc, &draw->rc, brush);
        ::DeleteObject(brush);

        // 单元格右下边框
        HBRUSH border = ::CreateSolidBrush(RGB(220, 222, 226));
        RECT bottom = draw->rc;
        bottom.top = (bottom.bottom > bottom.top) ? bottom.bottom - 1 : bottom.top;
        ::FillRect(draw->hdc, &bottom, border);

        RECT right = draw->rc;
        right.left = (right.right > right.left) ? right.right - 1 : right.left;
        ::FillRect(draw->hdc, &right, border);
        ::DeleteObject(border);

        // 文本
        wchar_t text[256] = { 0 };
        HDITEMW item;
        memset(&item, 0, sizeof(item));
        item.mask = HDI_TEXT | HDI_FORMAT;
        item.pszText = text;
        item.cchTextMax = _countof(text);

        ::SendMessageW(header->GetSafeHwnd(), HDM_GETITEMW, (WPARAM)draw->dwItemSpec, (LPARAM)&item);

        int previousMode = ::SetBkMode(draw->hdc, TRANSPARENT);
        COLORREF previousColor = ::SetTextColor(draw->hdc, kGroupText);

        HFONT font = (HFONT)::SendMessageW(header->GetSafeHwnd(), WM_GETFONT, 0, 0);
        HGDIOBJ oldFont = nullptr;
        if (font != nullptr)
            oldFont = ::SelectObject(draw->hdc, font);

        UINT format = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS;
        if ((item.fmt & HDF_CENTER) != 0)
            format |= DT_CENTER;
        else if ((item.fmt & HDF_RIGHT) != 0)
            format |= DT_RIGHT;
        else
            format |= DT_LEFT;

        RECT textRect = draw->rc;
        textRect.left += 4;
        textRect.right -= 4;

        ::DrawTextW(draw->hdc, text, -1, &textRect, format);

        if (oldFont != nullptr)
            ::SelectObject(draw->hdc, oldFont);

        ::SetTextColor(draw->hdc, previousColor);
        ::SetBkMode(draw->hdc, previousMode);

        *pResult = CDRF_SKIPDEFAULT;
        return;
    }

    default:
        break;
    }

    *pResult = CDRF_DODEFAULT;
}

} // namespace ps

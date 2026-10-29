#include "pch.h"
#include "PrintServerDlg.h"

#include <string>
#include <vector>
#include <thread>
#include <set>
#include <map>

#include <winspool.h>
#include <uxtheme.h>

#pragma comment(lib, "uxtheme.lib")

#include "BuildConfig.h"
#include "Config.h"
#include "LicenseHelper.h"
#include "LogHelper.h"
#include "Program.h"
#include "UiGdiPlus.h"
#include "UiTheme.h"
#include "Wmi.h"

namespace ps
{

namespace
{

const UINT kTrayCallbackMessage = WM_APP + 20;
const UINT kGuiLogMessage = WM_APP + 21;
const UINT_PTR kStatusTimerId = 1;
const UINT kTrayIconId = 1;
const UINT kTrayMenuShow = 41001;
const UINT kTrayMenuStop = 41002;
const UINT kTrayMenuExit = 41003;

// 对应 C# PrintServerForm 的 Size
const int kFormWidth = 708;
const int kFormHeight = 670;

// 对应 WinForms 表单边框：WinForms 会把非客户区按自己的边框常量收缩，
// FixedSingle 表单的客户区是 554x641（非客户区 3,26），而系统给对话框的默认
// 非客户区是 8,31（客户区 544x631），故这里把客户区向外扩展 5px 以对齐
const int kFrameShave = 5;

// 对应 WinForms 的颜色常量（现代扁平化配色）
const COLORREF kColorWhite = RGB(255, 255, 255);
const COLORREF kColorBlack = RGB(0, 0, 0);
const COLORREF kColorGray = RGB(128, 128, 128);
const COLORREF kColorRed = Ui::Design::kDanger;
const COLORREF kColorGreen = Ui::Design::kSuccess;
const COLORREF kColorOrange = Ui::Design::kWarning;
const COLORREF kColorOrangeRed = RGB(234, 88, 12);
const COLORREF kColorDarkBlue = Ui::Design::kAccent;
const COLORREF kColorDarkGreen = Ui::Design::kSuccess;

const COLORREF kFormBack = Ui::Design::kPageBack;
const COLORREF kHeaderBack = Ui::Design::kBrandTop;
const COLORREF kHeaderSubText = Ui::Design::kTextOnBrandSub;
const COLORREF kGroupText = Ui::Design::kTextBody;
const COLORREF kLabelMuted = Ui::Design::kTextMuted;
const COLORREF kLogBack = Ui::Design::kLogBack;
const COLORREF kLogText = Ui::Design::kLogText;
const COLORREF kLogCard = RGB(20, 26, 42);
const COLORREF kGridHeaderBack = Ui::Design::kCardBack;

// 按钮语义色（动态状态切换时也使用这一套）
const COLORREF kBtnInstall = RGB(100, 116, 139);        // 安装服务：中性
const COLORREF kBtnInstallHoverRed = Ui::Design::kWarning;   // 已安装 → 卸载服务：琥珀
const COLORREF kBtnServiceStart = Ui::Design::kSuccess;       // 启动服务：成功绿
const COLORREF kBtnServiceStop = Ui::Design::kDanger;         // 停止服务：危险红
const COLORREF kBtnServerStart = Ui::Design::kAccent;         // 启动服务器：品牌蓝（区内唯一实底主按钮）
const COLORREF kBtnOutline = Ui::Design::kTextMuted;          // 次级按钮：中性描边
const COLORREF kBtnSelect = Ui::Design::kTextMuted;           // 兼容旧引用

// 主界面布局（客户区 702x641 = 左侧 148 导航栏 + 右侧 554 工作区）
// 结构：页面标题行（标题 + 服务状态胶囊 + 刷新）→ 服务器控制卡 →
//       服务器信息卡（主机名 / 本机 IP / 已共享打印机）→ 打印机共享列表卡
const int kNavWidth = 148;          // 左侧导航栏宽度
const int kWorkLeft = kNavWidth;    // 工作区左边界
const int kWorkPadX = 20;           // 工作区左右留白
const int kCardLeft = kWorkLeft + kWorkPadX;
const int kCardWidth = 554 - kWorkPadX * 2;
const int kPageHeadTop = 20;        // 页面标题行
const int kPageHeadHeight = 36;
const int kPageHeadBottomMargin = kWorkPadX;   // 工作区底部留白（原型 padding 20）
const int kColumnGap = 8;

const int kCardServerTop = kPageHeadTop + kPageHeadHeight + 16;      // 72
const int kCardServerHeight = 100;   // 16 + 22 + 10 + 36 + 16
// 服务器信息卡：16 + 22 + 10 + 5 行 ×30 + 16
// 行 1 主机名 / 行 2 本机 IP / 行 3-5 IPP 免驱地址（每台已共享打印机一行，最多 3 台）
const int kCardInfoTop = kCardServerTop + kCardServerHeight + 16;    // 188
const int kCardInfoHeight = 214;
const int kCardPrinterTop = kCardInfoTop + kCardInfoHeight + 16;     // 358
const int kCardPrinterHeight = 244;  // 16 + 22 + 10 + 表头30 + 5×30 + 16
const int kCardLogTop = kCardPrinterTop + kCardPrinterHeight + 16;
const int kCardLogHeight = 173;      // 余量
const int kCardGap = 16;

const wchar_t* kYahei = L"微软雅黑";

// 列表列宽合计 482（勾选 28 + 名称 240 + 端口 104 + 状态 110）
const int kColCheckWidth = 28;
const int kColNameWidth = 240;
const int kColPortWidth = 104;
const int kColStatusWidth = 110;
const int kRowHeight = 30;

// 打印机列表卡片（含卡内控件）：服务器页（只读、仅已共享）与设备列表页（全部、可勾选）都显示
const UINT kPrinterGroup[] = {
    IDC_PS_GB_PRINTER, IDC_PS_PRINTER_LIST, IDC_PS_PRINTER_COUNT
};

// 仅设备列表页出现：勾选是可编辑的，服务器页的共享列表为只读展示
const UINT kPrinterEditGroup[] = {
    IDC_PS_BTN_SELECT_ALL, IDC_PS_BTN_DESELECT_ALL
};

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

std::wstring TrimWide(const std::wstring& text)
{
    size_t begin = 0;
    while (begin < text.size() && iswspace(text[begin]))
        ++begin;

    size_t end = text.size();
    while (end > begin && iswspace(text[end - 1]))
        --end;

    return text.substr(begin, end - begin);
}

std::string TrimAscii(const std::string& text)
{
    size_t begin = 0;
    while (begin < text.size() && (unsigned char)text[begin] <= ' ')
        ++begin;

    size_t end = text.size();
    while (end > begin && (unsigned char)text[end - 1] <= ' ')
        --end;

    return text.substr(begin, end - begin);
}

// 槽位与 RAW 端口一一对应：9100 -> 0，9101 -> 1（对应 /ipp/print[/N]）
int PortToSlotIndex(const std::string& portText)
{
    const std::string text = TrimAscii(portText);
    if (text.empty())
        return -1;

    char* end = nullptr;
    const long port = strtol(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || port < 9100 || port > 9100 + 63)
        return -1;

    return (int)(port - 9100);
}

bool StartsWithIgnoreCaseAscii(const std::string& text, const char* prefix)
{
    size_t length = strlen(prefix);
    if (text.size() < length)
        return false;

    return _strnicmp(text.c_str(), prefix, length) == 0;
}

bool EqualsIgnoreCaseAscii(const std::string& text, const char* value)
{
    return _stricmp(text.c_str(), value) == 0;
}

void AppendUtf8FromAnsi(const std::vector<char>& bytes, std::string& out)
{
    if (bytes.empty())
        return;

    // 对应 .NET RedirectStandardOutput 的默认解码（系统 ANSI 代码页）
    int wideLength = ::MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (wideLength <= 0)
        return;

    std::wstring wide((size_t)wideLength, L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), &wide[0], wideLength);

    out += WideToUtf8(wide);
}

// ---- 列表自绘：复选框状态图与悬停行 ----
HBITMAP CreateCheckBitmap(bool checked)
{
    const int size = 16;

    HDC screen = ::GetDC(nullptr);
    HDC dc = ::CreateCompatibleDC(screen);
    HBITMAP bitmap = ::CreateCompatibleBitmap(screen, size, size);
    HBITMAP oldBitmap = (HBITMAP)::SelectObject(dc, bitmap);

    RECT rc = { 0, 0, size, size };
    ::FillRect(dc, &rc, Ui::GetSurfaceBrush(Ui::Design::kCardBack));

    RECT box = { 2, 2, size - 2, size - 2 };
    if (checked)
    {
        Ui::Gfx::FillRoundRect(dc, box, 3, Ui::Design::kAccent);
        RECT mark = { 4, 4, size - 4, size - 4 };
        Ui::Gfx::DrawIcon(dc, Ui::Gfx::Icon::Check, mark, RGB(255, 255, 255));
    }
    else
    {
        Ui::Gfx::FillRoundRect(dc, box, 3, RGB(255, 255, 255));
        Ui::Gfx::DrawRoundRect(dc, box, 3, Ui::Design::kFieldBorder);
    }

    ::SelectObject(dc, oldBitmap);
    ::DeleteDC(dc);
    ::ReleaseDC(nullptr, screen);
    return bitmap;
}

void BuildCheckImages(CImageList& images)
{
    images.Create(16, 16, ILC_COLOR32 | ILC_MASK, 2, 2);

    HBITMAP unchecked = CreateCheckBitmap(false);
    HBITMAP checked = CreateCheckBitmap(true);

    images.Add(CBitmap::FromHandle(unchecked), Ui::Design::kCardBack);
    images.Add(CBitmap::FromHandle(checked), Ui::Design::kCardBack);

    ::DeleteObject(unchecked);
    ::DeleteObject(checked);
}

// 列表表头自绘（对应设计稿：浅底表头 + 粗体标题 + 底部分隔线，替换系统灰色表头）
// 注意：表头控件把 NM_CUSTOMDRAW 发给它的父窗口（即列表控件），因此由列表的子类过程处理。
LRESULT HandleHeaderCustomDraw(HWND header, LPARAM lParam)
{
    LPNMCUSTOMDRAW draw = reinterpret_cast<LPNMCUSTOMDRAW>(lParam);

    if (draw->dwDrawStage == CDDS_PREPAINT)
        return CDRF_NOTIFYITEMDRAW;

    if (draw->dwDrawStage != CDDS_ITEMPREPAINT)
        return CDRF_DODEFAULT;

    HDC hdc = draw->hdc;
    const RECT rc = draw->rc;

    ::FillRect(hdc, &rc, Ui::GetSurfaceBrush(RGB(245, 248, 252)));

    RECT line = { rc.left, rc.bottom - 1, rc.right, rc.bottom };
    ::FillRect(hdc, &line, Ui::GetSurfaceBrush(RGB(230, 236, 243)));

    wchar_t text[128] = { 0 };
    HDITEMW item;
    memset(&item, 0, sizeof(item));
    item.mask = HDI_TEXT;
    item.pszText = text;
    item.cchTextMax = _countof(text);
    ::SendMessageW(header, HDM_GETITEMW, draw->dwItemSpec, (LPARAM)&item);

    if (text[0] != 0)
    {
        RECT textRect = rc;
        textRect.left += 10;
        textRect.right -= 6;

        ::SetBkMode(hdc, TRANSPARENT);
        ::SetTextColor(hdc, RGB(71, 85, 105));

        HGDIOBJ oldFont = ::SelectObject(hdc, Ui::GetFont(kYahei, 9, true));
        ::DrawTextW(hdc, text, -1, &textRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        ::SelectObject(hdc, oldFont);
    }

    return CDRF_SKIPDEFAULT;
}

// 列表悬停行追踪：把当前行号回传给对话框
LRESULT CALLBACK PrinterListSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR idSubclass, DWORD_PTR refData)
{
    CPrintServerDlg* dialog = reinterpret_cast<CPrintServerDlg*>(refData);

    switch (message)
    {
    case WM_MOUSEMOVE:
    {
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd, 0 };
        ::TrackMouseEvent(&track);

        if (dialog != nullptr)
        {
            POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            LVHITTESTINFO hit = {};
            hit.pt = pt;
            hit.iItem = -1;
            ::SendMessageW(hwnd, LVM_HITTEST, 0, (LPARAM)&hit);
            dialog->SetHoverRow((hit.iItem >= 0) ? hit.iItem : -1);
        }
        break;
    }

    case WM_MOUSELEAVE:
        if (dialog != nullptr)
            dialog->SetHoverRow(-1);
        break;

    case WM_NOTIFY:
    {
        // 表头控件的 NM_CUSTOMDRAW 会发到列表（表头的父窗口）这里
        const NMHDR* notify = reinterpret_cast<const NMHDR*>(lParam);
        const HWND header = reinterpret_cast<HWND>(::SendMessageW(hwnd, LVM_GETHEADER, 0, 0));

        if (notify != nullptr && header != nullptr && notify->hwndFrom == header &&
            notify->code == NM_CUSTOMDRAW)
        {
            return HandleHeaderCustomDraw(header, lParam);
        }
        break;
    }

    case WM_NCDESTROY:
        ::RemoveWindowSubclass(hwnd, PrinterListSubclassProc, 1);
        break;

    default:
        break;
    }

    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

} // namespace

IMPLEMENT_DYNAMIC(CPrintServerDlg, CDialogEx)

BEGIN_MESSAGE_MAP(CPrintServerDlg, CDialogEx)
    ON_WM_PAINT()
    ON_WM_NCCALCSIZE()
    ON_WM_DRAWITEM()
    ON_WM_CTLCOLOR()
    ON_WM_ERASEBKGND()
    ON_WM_TIMER()
    ON_WM_CLOSE()
    ON_WM_SIZE()
    ON_MESSAGE(kTrayCallbackMessage, &CPrintServerDlg::OnTrayCallback)
    ON_MESSAGE(kGuiLogMessage, &CPrintServerDlg::OnGuiLog)
    ON_MESSAGE(Ui::kNavSelectionChanged, &CPrintServerDlg::OnNavSelectionChanged)
    ON_BN_CLICKED(IDC_PS_BTN_STARTSTOP, &CPrintServerDlg::OnStartStop)
    ON_BN_CLICKED(IDC_PS_BTN_INSTALL, &CPrintServerDlg::OnInstallToggle)
    ON_BN_CLICKED(IDC_PS_BTN_SERVICE_START, &CPrintServerDlg::OnServiceStartToggle)
    ON_BN_CLICKED(IDC_PS_BTN_CLEARLOG, &CPrintServerDlg::OnClearLog)
    ON_BN_CLICKED(IDC_PS_BTN_REFRESH, &CPrintServerDlg::OnRefreshPrinters)
    ON_BN_CLICKED(IDC_PS_BTN_SELECT_ALL, &CPrintServerDlg::OnSelectAll)
    ON_BN_CLICKED(IDC_PS_BTN_DESELECT_ALL, &CPrintServerDlg::OnDeselectAll)
    ON_NOTIFY(LVN_ITEMCHANGING, IDC_PS_PRINTER_LIST, &CPrintServerDlg::OnPrinterListChanging)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_PS_PRINTER_LIST, &CPrintServerDlg::OnPrinterListChanged)
    ON_NOTIFY(NM_DBLCLK, IDC_PS_PRINTER_LIST, &CPrintServerDlg::OnPrinterListDblClk)
    ON_NOTIFY(NM_CLICK, IDC_PS_PRINTER_LIST, &CPrintServerDlg::OnPrinterListClick)
    ON_NOTIFY(NM_CUSTOMDRAW, IDC_PS_PRINTER_LIST, &CPrintServerDlg::OnPrinterListCustomDraw)
END_MESSAGE_MAP()

CPrintServerDlg::CPrintServerDlg(CWnd* pParent)
    : CDialogEx(IDD_MAIN_DIALOG, pParent)
    , m_isRunning(false)
    , m_trayIconVisible(false)
    , m_isReallyClosing(false)
    , m_trayIcon(nullptr)
    , m_logoIcon(nullptr)
    , m_page(0)
    , m_hoverRow(-1)
    , m_editRow(-1)
    , m_suppressListEvents(false)
{
}
CPrintServerDlg::~CPrintServerDlg()
{
}

void CPrintServerDlg::DoDataExchange(CDataExchange* pDX)
{
    // 列表与微调控件在 BuildControls() 中显式 SubclassWindow，不再走 DDX
    CDialogEx::DoDataExchange(pDX);
}

BOOL CPrintServerDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    m_formBrush.CreateSolidBrush(kFormBack);
    m_headerBrush.CreateSolidBrush(kHeaderBack);
    m_logBrush.CreateSolidBrush(kLogBack);
    m_whiteBrush.CreateSolidBrush(kColorWhite);

    // 对应 C#：Size 560x670 + FixedSingle
    // MFC 创建对话框时会附加 WS_EX_DLGMODALFRAME（经典粗模态边框），必须去掉：
    // 它让非客户区变成 (8,31)、客户区 544x631，而 WinForms 的 FixedSingle 是
    // (3,26)、客户区 554x641。去掉后用 SWP_FRAMECHANGED 让系统重算边框
    LONG exStyle = ::GetWindowLongW(GetSafeHwnd(), GWL_EXSTYLE);
    if ((exStyle & WS_EX_DLGMODALFRAME) != 0)
    {
        ::SetWindowLongW(GetSafeHwnd(), GWL_EXSTYLE, exStyle & ~WS_EX_DLGMODALFRAME);
        SetWindowPos(nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

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

    // 对应 CenterToScreen()
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

    // 对应 C# 构造函数中的初始化顺序
    LoadPrinters();
    LoadSavedSettings();
    UpdateNetworkInfo();
    UpdateServiceStatus();
    LoadExistingLog();

    // 对应 C# 的 Timer(3000ms)：更新服务状态 + 自动刷新服务日志
    SetTimer(kStatusTimerId, 3000, nullptr);

    return TRUE;
}

void CPrintServerDlg::BuildControls()
{
    HWND self = GetSafeHwnd();

    Ui::ClearSurfaces();

    HFONT baseFont = Ui::GetFont(kYahei, 9, false);
    HFONT cardFont = Ui::GetFont(kYahei, 10, true);
    HFONT statusFont = Ui::GetFont(kYahei, 10, true);
    HFONT chipFont = Ui::GetFont(kYahei, 9, false);

    // ========== 顶部渐变头部（标题/副标题/版本胶囊均由 OnPaint 绘制） ==========
    m_version = (L"v" + Utf8ToWide(ProgramHost::GetDisplayVersion())).c_str();
    m_productTitle = Utf8ToWide(BuildConfig::ProductName).c_str();
    m_productSubtitle = Utf8ToWide(BuildConfig::Description).c_str();

    // 对应 C#：this.Text = "{Product} - v{Version} - By：{Company}"
    std::wstring caption = std::wstring(m_productTitle.GetString()) + L" - " +
                           std::wstring(m_version.GetString()) + L" - By：" +
                           Utf8ToWide(BuildConfig::CompanyName);
    SetWindowTextW(caption.c_str());

    m_logoIcon = ::LoadIconW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDR_MAINFRAME));

    // ========== 左侧导航栏（自有设计元素：品牌区 + 导航项 + 本机 IP） ==========
    BuildNavBar();

    // 页面标题（文字由 OnPaint 绘制在工作区左上角）
    m_pageTitle = L"服务器";

    // 胶囊文字用 8pt 粗体（原型 .pill font:600 11px）
    HFONT pillFont = Ui::GetFont(kYahei, 8, true);

    // ========== 页面标题行右侧：服务状态胶囊 + 刷新图标按钮 ==========
    // 这一行直接落在工作区页面底色上（不在白卡片上），圆角外四角必须铺页面底色，
    // 否则胶囊/图标按钮的圆角会露出白方角
    HWND status = CreateChildWnd(self, L"Static", L"已停止", SS_LEFT, 0,
                                 kCardLeft + 366, kPageHeadTop + 7, 76,
                                 Ui::Design::kPillHeight, IDC_PS_STATUS, pillFont);
    Ui::SetTextColor(status, kBtnServiceStop);
    Ui::MakePill(this, IDC_PS_STATUS, Ui::Design::kCardBack, kFormBack);

    CreateChildWnd(self, L"Button", L"", BS_OWNERDRAW, 0,
                   kCardLeft + 486, kPageHeadTop + 4, Ui::Design::kIconButtonSize,
                   Ui::Design::kIconButtonSize, IDC_PS_BTN_REFRESH, baseFont);
    Ui::MakeOutlineButton(this, IDC_PS_BTN_REFRESH, Ui::Design::kTextDisabled,
                          Ui::Gfx::Icon::Refresh, Ui::Design::kCardBack,
                          Ui::Design::kCardBorder, kFormBack);

    // 胶囊宽度随文字自适应，整行从右向左重排（对应原型 flex 右对齐）
    LayoutPageHead();

    // ========== 打印机共享列表卡片（卡片底色由 OnPaint 绘制，列表铺满卡片内区） ==========
    HWND list = CreateChildWnd(self, WC_LISTVIEWW, L"",
                               LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                               0, kCardLeft + 16,
                               kCardPrinterTop + Ui::Design::kCardContentTop,
                               kCardWidth - 32, 180, IDC_PS_PRINTER_LIST, baseFont);
    m_printerList.SubclassWindow(list);

    m_printerList.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
    ::SetWindowTheme(list, L"", L"");
    m_printerList.SetBkColor(Ui::Design::kCardBack);
    m_printerList.SetTextBkColor(Ui::Design::kCardBack);

    // 行高 30：用 1x30 的图片列表强制行高（与设计稿表格行高一致）
    m_rowImages.Create(1, kRowHeight, ILC_COLOR32 | ILC_MASK, 1, 1);
    {
        CBitmap bitmap;
        bitmap.CreateCompatibleBitmap(GetDC(), 1, kRowHeight);
        m_rowImages.Add(&bitmap, Ui::Design::kCardBack);
    }
    m_printerList.SetImageList(&m_rowImages, LVSIL_SMALL);

    // 自绘复选框（勾选状态图），替换系统默认位图
    BuildCheckImages(m_checkImages);
    m_printerList.SetImageList(&m_checkImages, LVSIL_STATE);

    m_printerList.InsertColumn(0, L"", LVCFMT_CENTER, kColCheckWidth);
    m_printerList.InsertColumn(1, L"打印机名称", LVCFMT_LEFT, kColNameWidth);
    m_printerList.InsertColumn(2, L"端口", LVCFMT_CENTER, kColPortWidth);
    m_printerList.InsertColumn(3, L"状态", LVCFMT_CENTER, kColStatusWidth);

    // 表头样式（对应 ColumnHeadersDefaultCellStyle + EnableHeadersVisualStyles=false）
    CHeaderCtrl* header = m_printerList.GetHeaderCtrl();
    if (header != nullptr)
    {
        // 关掉系统主题；表头由 HandleHeaderCustomDraw 自绘（浅底 + 粗体 + 分隔线）
        ::SetWindowTheme(header->GetSafeHwnd(), L"", L"");
        header->Invalidate(TRUE);
    }

    // 列表悬停高亮
    ::SetWindowSubclass(list, PrinterListSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    // 卡片标题行右侧：全选 / 取消（小按钮）+ 共 N 台（中性胶囊）
    CreateChildWnd(self, L"Button", L"全选", BS_OWNERDRAW, 0,
                   482, kCardPrinterTop + 13, 52, Ui::Design::kBtnHeightSmall,
                   IDC_PS_BTN_SELECT_ALL, Ui::GetFont(kYahei, 8, false));
    Ui::MakeOutlineButton(this, IDC_PS_BTN_SELECT_ALL, kGroupText, Ui::Gfx::Icon::None,
                          Ui::Design::kCardBack, Ui::Design::kFieldBorder);

    CreateChildWnd(self, L"Button", L"取消", BS_OWNERDRAW, 0,
                   542, kCardPrinterTop + 13, 52, Ui::Design::kBtnHeightSmall,
                   IDC_PS_BTN_DESELECT_ALL, Ui::GetFont(kYahei, 8, false));
    Ui::MakeOutlineButton(this, IDC_PS_BTN_DESELECT_ALL, kGroupText, Ui::Gfx::Icon::None,
                          Ui::Design::kCardBack, Ui::Design::kFieldBorder);

    HWND printerCount = CreateChildWnd(self, L"Static", L"共 0 台", SS_LEFT, 0,
                                       602, kCardPrinterTop + 16, 64, Ui::Design::kPillHeight,
                                       IDC_PS_PRINTER_COUNT, pillFont);
    Ui::SetTextColor(printerCount, kLabelMuted);
    Ui::MakePill(this, IDC_PS_PRINTER_COUNT);

    // 卡内所有控件的位置集中在这里计算
    LayoutPrinterCard(kCardPrinterTop, kCardPrinterHeight);

    // ========== 服务器控制卡片（卡片底色由 OnPaint 绘制） ==========
    // 行基线：卡片内首个内容行（60 内边距 16 + 标题行 22 + 间距 10）
    const int rowTop = kCardServerTop + Ui::Design::kCardContentTop;
    const int rowCenterTop = rowTop + (Ui::Design::kBtnHeight - Ui::Design::kInputHeight) / 2;

    // 标签与输入框共用同一条垂直中线：Static 用 SS_LEFT 时文字贴控件上沿，
    // 而 Edit 的文字在框内垂直居中，同高摆放会明显错位（标签偏高约 5px）。
    // 这里让标签占满输入框高度并加 SS_CENTERIMAGE（单行文本垂直居中）对齐。
    HWND apiPortLabel = CreateChildWnd(self, L"Static", L"API 端口", SS_LEFT | SS_CENTERIMAGE, 0,
                                       kCardLeft + 16, rowCenterTop, 60, Ui::Design::kInputHeight,
                                       IDC_PS_API_PORT_LABEL, Ui::GetFont(kYahei, 9, false));
    Ui::SetTextColor(apiPortLabel, kGroupText);

    // 输入框底板（绘制圆角描边）；创建后紧接着由下方显式压到 Edit/微调按钮之下
    HWND portField = CreateChildWnd(self, L"Static", L"", SS_OWNERDRAW, 0,
                                    kCardLeft + 84, rowCenterTop, 84,
                                    Ui::Design::kInputHeight, IDC_PS_API_PORT_FIELD, nullptr);
    Ui::MakeField(this, IDC_PS_API_PORT_FIELD);

    // 单行 Edit 的文字是贴着控件上沿绘制的：把 Edit 按输入框居中摆放，数字会整体偏高，
    // 与左侧标签、右侧微调箭头都对不齐。这里把 Edit 下移，使数字正好落在输入框中线上。
    const int portEditTop = rowCenterTop + 10;
    HWND apiPortEdit = CreateChildWnd(self, L"Edit", L"9180",
                                      ES_LEFT | ES_AUTOHSCROLL | ES_NUMBER, 0,
                                      kCardLeft + 92, portEditTop, 68, 20,
                                      IDC_PS_API_PORT_EDIT, Ui::GetFont(L"Consolas", 9, true));
    ::SendMessageW(apiPortEdit, EM_SETLIMITTEXT, 5, 0);

    HWND apiPortSpin = CreateChildWnd(self, UPDOWN_CLASSW, L"",
                                      UDS_ALIGNRIGHT | UDS_SETBUDDYINT | UDS_ARROWKEYS |
                                      UDS_NOTHOUSANDS, 0,
                                      kCardLeft + 92, rowCenterTop + 5, 14, 24,
                                      IDC_PS_API_PORT_SPIN, baseFont);
    m_apiPortSpin.SubclassWindow(apiPortSpin);
    m_apiPortSpin.SetBuddy(GetDlgItem(IDC_PS_API_PORT_EDIT));
    m_apiPortSpin.SetRange32(1, 65535);
    m_apiPortSpin.SetPos32(9180);

    // UDS_ALIGNRIGHT 会令微调按钮与 buddy 顶部对齐，Edit 下移后箭头会跟着偏下；
    // 这里（Edit 定位之后）把它的纵向位置显式摆回输入框中线
    {
        RECT spinRect = { 0, 0, 0, 0 };
        ::GetWindowRect(apiPortSpin, &spinRect);
        ::MapWindowPoints(HWND_DESKTOP, self, reinterpret_cast<LPPOINT>(&spinRect), 2);
        ::SetWindowPos(apiPortSpin, nullptr, spinRect.left, rowCenterTop + 5,
                       spinRect.right - spinRect.left, 24, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // 底板必须始终排在端口 Edit / 微调按钮之后（= 位于两者下方）：否则 WindowFromPoint
    // 命中底板，端口输入框与微调按钮都点不动
    ::SetWindowPos(portField, apiPortSpin, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    {
        // 底板表面：Edit 内部取字段底色（与底板一致）
        RECT fieldRect = { kCardLeft + 84, rowCenterTop,
                           kCardLeft + 168, rowCenterTop + Ui::Design::kInputHeight };
        Ui::RegisterSurface(fieldRect, Ui::Design::kFieldBack, self);
    }

    // 次级按钮 = 白底 + #CBD5E1 描边 + #334155 文字；主按钮 = 实底品牌蓝
    CreateChildWnd(self, L"Button", L"安装服务", BS_OWNERDRAW, 0,
                   kCardLeft + 210, rowTop, 88, Ui::Design::kBtnHeight,
                   IDC_PS_BTN_INSTALL, Ui::GetFont(kYahei, 9, true));
    Ui::MakeOutlineButton(this, IDC_PS_BTN_INSTALL, kGroupText,
                          Ui::Gfx::Icon::None, Ui::Design::kCardBack,
                          Ui::Design::kFieldBorder);

    CreateChildWnd(self, L"Button", L"启动服务", BS_OWNERDRAW, 0,
                   kCardLeft + 306, rowTop, 88, Ui::Design::kBtnHeight,
                   IDC_PS_BTN_SERVICE_START, Ui::GetFont(kYahei, 9, true));
    Ui::MakeOutlineButton(this, IDC_PS_BTN_SERVICE_START, kGroupText,
                          Ui::Gfx::Icon::None, Ui::Design::kCardBack,
                          Ui::Design::kFieldBorder);

    CreateChildWnd(self, L"Button", L"启动服务器", BS_OWNERDRAW, 0,
                   kCardLeft + 402, rowTop, 96, Ui::Design::kBtnHeight,
                   IDC_PS_BTN_STARTSTOP, Ui::GetFont(kYahei, 9, true));
    Ui::MakeFlatButton(this, IDC_PS_BTN_STARTSTOP, kBtnServerStart, kColorWhite);

    // ========== 运行日志卡片（卡片底色由 OnPaint 绘制，卡内是深色日志框） ==========
    CreateChildWnd(self, L"Button", L"清空日志", BS_OWNERDRAW, 0,
                   602, kCardLogTop + 13, 64, Ui::Design::kBtnHeightSmall,
                   IDC_PS_BTN_CLEARLOG, Ui::GetFont(kYahei, 8, false));
    Ui::MakeOutlineButton(this, IDC_PS_BTN_CLEARLOG, kGroupText, Ui::Gfx::Icon::None,
                          Ui::Design::kCardBack, Ui::Design::kFieldBorder);

    // 日志框底板：后创建 → 位于卡片之上；日志编辑框再叠在其上（露出顶部实时条）
    CreateChildWnd(self, L"Static", L"", SS_OWNERDRAW, 0,
                   kCardLeft + 16, kCardLogTop + Ui::Design::kCardContentTop,
                   kCardWidth - 32, 109, IDC_PS_LOG_BOX, nullptr);
    Ui::MakeLogBox(this, IDC_PS_LOG_BOX);

    // 顶部条右侧显示日志文件名（对应原型 log/printserver-YYYY-MM-DD.log）
    {
        SYSTEMTIME now;
        ::GetLocalTime(&now);
        CString note;
        note.Format(L"log/printserver-%04d-%02d-%02d.log",
                    now.wYear, now.wMonth, now.wDay);
        Ui::SetLogBoxNote(::GetDlgItem(self, IDC_PS_LOG_BOX), note);
    }

    HWND logEdit = CreateChildWnd(self, L"Edit", L"",
                                  ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | ES_LEFT,
                                  0, 179, 500, 492, 101, IDC_PS_LOG,
                                  Ui::GetFont(L"Consolas", 9, false));

    // 日志框底板同样显式排在日志编辑框之后，避免盖在编辑框上（滚轮/选中会失效）
    ::SetWindowPos(::GetDlgItem(self, IDC_PS_LOG_BOX), logEdit, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // 日志框（深色）与卡内控件的最终位置
    LayoutLogCard(kCardLogTop, kCardLogHeight);

    // ========== 软件信息页 ==========
    // 页面内容全部由 OnPaint 直接绘制，页内不放任何子窗口按钮

    // ========== 服务器信息卡（主机名 / 本机 IP / 已共享打印机） ==========
    // 内容同样由 OnPaint 绘制（不建子窗口，避免遮挡、z 序与文字基线问题）

    // 初始页 = 服务器页：应用分页可见性与布局（服务器控制 + 服务器信息 + 打印机列表）
    ShowPage(0);
}

// ===========================================================================
// 导航栏与分页布局
// ===========================================================================

void CPrintServerDlg::BuildNavBar()
{
    CRect client;
    GetClientRect(&client);

    HWND nav = CreateChildWnd(GetSafeHwnd(), L"Static", L"", SS_LEFT, 0,
                              0, 0, Ui::CNavBar::kWidth, client.Height(),
                              IDC_PS_NAV, nullptr);
    if (nav == nullptr)
        return;

    m_navBar.SubclassWindow(nav);
    m_navBar.SetBrand(m_productTitle.GetString(), m_version.GetString());
}

void CPrintServerDlg::ShowGroup(const UINT* ids, size_t count, bool show)
{
    for (size_t i = 0; i < count; ++i)
    {
        HWND control = ::GetDlgItem(GetSafeHwnd(), ids[i]);
        if (control != nullptr)
            ::ShowWindow(control, show ? SW_SHOW : SW_HIDE);
    }
}

// 页面标题行右侧：服务胶囊 + 刷新图标按钮，从右向左贴合排布
// （原型 flex 右对齐 + gap 8；胶囊宽度随文字自适应）
void CPrintServerDlg::LayoutPageHead()
{
    HWND self = GetSafeHwnd();

    const int right = kCardLeft + kCardWidth - 16;
    const int pillTop = kPageHeadTop + (kPageHeadHeight - Ui::Design::kPillHeight) / 2;
    const int iconTop = kPageHeadTop + (kPageHeadHeight - Ui::Design::kIconButtonSize) / 2;
    HFONT pillFont = Ui::GetFont(kYahei, 8, true);

    const int refreshLeft = right - Ui::Design::kIconButtonSize;

    // 胶囊宽度随文字自适应，但设上限：长文案不能挤占页面标题
    int statusWidth = Ui::MeasurePillWidth(::GetDlgItem(self, IDC_PS_STATUS), pillFont);
    if (statusWidth > 170)
        statusWidth = 170;

    const int statusLeft = refreshLeft - kColumnGap - statusWidth;

    ::SetWindowPos(::GetDlgItem(self, IDC_PS_STATUS), nullptr, statusLeft, pillTop,
                   statusWidth, Ui::Design::kPillHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(::GetDlgItem(self, IDC_PS_BTN_REFRESH), nullptr, refreshLeft, iconTop,
                   Ui::Design::kIconButtonSize, Ui::Design::kIconButtonSize,
                   SWP_NOZORDER | SWP_NOACTIVATE);
}

// 设置页面标题行胶囊的文字与语义色（宽度按文字重算并重排整行）
void CPrintServerDlg::SetHeadPill(UINT id, const wchar_t* text, COLORREF ink)
{
    HWND pill = ::GetDlgItem(GetSafeHwnd(), id);
    if (pill == nullptr)
        return;

    Ui::SetLabelText(this, id, text);
    Ui::SetTextColor(pill, ink);

    LayoutPageHead();
    ::InvalidateRect(pill, nullptr, TRUE);
}

// 打印机共享列表卡片：列表铺满卡片；「全选 / 取消 / 共 N 台」在卡片标题行右侧。
// 卡片几何随分页变化（服务器页从固定行高起铺到页面底部），故卡内控件位置统一由这里计算。
void CPrintServerDlg::LayoutPrinterCard(int top, int height)
{
    HWND self = GetSafeHwnd();
    const int right = kCardLeft + kCardWidth - 16;

    m_printerCardTop = top;
    m_printerCardHeight = height;

    ::SetWindowPos(::GetDlgItem(self, IDC_PS_PRINTER_LIST), nullptr,
                   kCardLeft + 16, top + Ui::Design::kCardContentTop,
                   kCardWidth - 32, height - Ui::Design::kCardContentTop - 16,
                   SWP_NOZORDER | SWP_NOACTIVATE);

    // 卡片标题行右侧（自右向左）：共 N 台胶囊 → 取消 → 全选，间距 8
    int countWidth = Ui::MeasurePillWidth(::GetDlgItem(self, IDC_PS_PRINTER_COUNT),
                                          Ui::GetFont(kYahei, 8, true));
    if (countWidth > 120)
        countWidth = 120;

    const int countLeft = right - countWidth;
    const int deselectLeft = countLeft - kColumnGap - 52;
    const int selectLeft = deselectLeft - kColumnGap - 52;

    ::SetWindowPos(::GetDlgItem(self, IDC_PS_PRINTER_COUNT), nullptr,
                   countLeft, top + 16, countWidth, Ui::Design::kPillHeight,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(::GetDlgItem(self, IDC_PS_BTN_DESELECT_ALL), nullptr,
                   deselectLeft, top + 13, 52,
                   Ui::Design::kBtnHeightSmall, SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(::GetDlgItem(self, IDC_PS_BTN_SELECT_ALL), nullptr,
                   selectLeft, top + 13, 52,
                   Ui::Design::kBtnHeightSmall, SWP_NOZORDER | SWP_NOACTIVATE);
}

// 运行日志卡片：卡内深色日志框（含顶部实时条）+ 叠在其上的日志编辑框。
void CPrintServerDlg::LayoutLogCard(int top, int height)
{
    HWND self = GetSafeHwnd();

    ::SetWindowPos(::GetDlgItem(self, IDC_PS_BTN_CLEARLOG), nullptr,
                   kCardLeft + kCardWidth - 16 - 64, top + 13, 64,
                   Ui::Design::kBtnHeightSmall, SWP_NOZORDER | SWP_NOACTIVATE);

    m_logBoxRect = CRect(kCardLeft + 16, top + Ui::Design::kCardContentTop,
                         kCardLeft + kCardWidth - 16, top + height - 16);

    ::SetWindowPos(::GetDlgItem(self, IDC_PS_LOG_BOX), nullptr,
                   m_logBoxRect.left, m_logBoxRect.top,
                   m_logBoxRect.Width(), m_logBoxRect.Height(),
                   SWP_NOZORDER | SWP_NOACTIVATE);

    // 编辑框与日志框同色，叠在上面只露出顶部实时条与底部圆角
    ::SetWindowPos(::GetDlgItem(self, IDC_PS_LOG), nullptr,
                   m_logBoxRect.left + 1, m_logBoxRect.top + Ui::Design::kLogStripHeight,
                   m_logBoxRect.Width() - 2,
                   m_logBoxRect.Height() - Ui::Design::kLogStripHeight - 8,
                   SWP_NOZORDER | SWP_NOACTIVATE);
}

// 卡内控件的背景刷按「落在哪张卡片上」取色，卡片矩形随分页变化，
// 故每次换页后按当前分页重新登记（不能依赖窗口可见性：初始化时对话框尚未显示，
// 所有子窗口的 IsWindowVisible 都是 FALSE）
void CPrintServerDlg::RegisterCardSurfaces()
{
    const HWND self = GetSafeHwnd();
    Ui::ClearSurfaces(self);

    for (int i = 0; i < 5; ++i)
    {
        if (IsCardVisible(i))
            Ui::RegisterSurface(m_cardRects[i], Ui::Design::kCardBack, self);
    }
}

// 卡片 0 服务器控制 / 1 打印机共享列表 / 2 运行日志 / 3 软件信息 / 4 服务器信息
// （服务器页显示 0 + 4 + 1；设备列表页把 1 铺满）
bool CPrintServerDlg::IsCardVisible(int index) const
{
    switch (index)
    {
    case 0:
    case 4:
        return m_page == 0;
    case 1:
        return m_page == 0 || m_page == 1;
    case 2:
        return m_page == 2;
    default:
        return m_page == 3;
    }
}

// page: 0 服务器（控制卡 + 信息卡 + 列表卡）/ 1 设备列表（列表铺满）/
//       2 运行日志（日志铺满）/ 3 软件信息（铺满）
void CPrintServerDlg::ShowPage(int page)
{
    if (page < 0 || page > 3)
        return;

    // 换页前先落定尚未提交的端口编辑（列表行号在换页后会变化）
    CommitPortEdit(false);

    static const UINT kServerGroup[] = {
        IDC_PS_GB_SERVICE, IDC_PS_API_PORT_LABEL, IDC_PS_API_PORT_EDIT,
        IDC_PS_API_PORT_FIELD, IDC_PS_API_PORT_SPIN,
        IDC_PS_BTN_INSTALL, IDC_PS_BTN_SERVICE_START, IDC_PS_BTN_STARTSTOP
    };
    static const UINT kLogGroup[] = {
        IDC_PS_GB_LOG, IDC_PS_LOG_BOX, IDC_PS_BTN_CLEARLOG, IDC_PS_LOG
    };

    m_page = page;

    const bool serverPage = (page == 0);
    const bool devicesPage = (page == 1);
    const bool logsPage = (page == 2);
    const bool licensePage = (page == 3);

    ShowGroup(kServerGroup, _countof(kServerGroup), serverPage);
    ShowGroup(kPrinterGroup, _countof(kPrinterGroup), serverPage || devicesPage);
    // 「全选 / 取消」只在可编辑的设备列表页出现（服务器页的共享列表为只读展示）
    ShowGroup(kPrinterEditGroup, _countof(kPrinterEditGroup), devicesPage);
    // 运行日志只在「运行日志」页显示
    ShowGroup(kLogGroup, _countof(kLogGroup), logsPage);

    // 单页展示时对应卡片铺满工作区（顶到页面标题行下方）
    const int fillTop = kCardServerTop;

    CRect client;
    GetClientRect(&client);
    const int fillHeight = client.Height() - fillTop - kPageHeadBottomMargin;

    // 卡片矩形（OnPaint 按当前分页绘制卡片底色与标题）
    m_cardRects[0].SetRect(kCardLeft, kCardServerTop,
                           kCardLeft + kCardWidth, kCardServerTop + kCardServerHeight);
    m_cardRects[4].SetRect(kCardLeft, kCardInfoTop,
                           kCardLeft + kCardWidth, kCardInfoTop + kCardInfoHeight);

    if (devicesPage)
    {
        m_cardRects[1].SetRect(kCardLeft, fillTop, kCardLeft + kCardWidth, fillTop + fillHeight);
        LayoutPrinterCard(m_cardRects[1].top, m_cardRects[1].Height());
    }
    else if (serverPage)
    {
        m_cardRects[1].SetRect(kCardLeft, kCardPrinterTop,
                               kCardLeft + kCardWidth, client.Height() - kPageHeadBottomMargin);
        LayoutPrinterCard(m_cardRects[1].top, m_cardRects[1].Height());
    }

    if (logsPage)
    {
        m_cardRects[2].SetRect(kCardLeft, fillTop, kCardLeft + kCardWidth, fillTop + fillHeight);
        LayoutLogCard(m_cardRects[2].top, m_cardRects[2].Height());
    }

    if (licensePage)
        m_cardRects[3].SetRect(kCardLeft, fillTop, kCardLeft + kCardWidth, fillTop + fillHeight);

    switch (page)
    {
    case 1:
        m_pageTitle = L"设备列表";
        break;
    case 2:
        m_pageTitle = L"运行日志";
        break;
    case 3:
        m_pageTitle = L"软件信息";
        break;
    default:
        m_pageTitle = L"服务器";
        break;
    }

    // 列表内容随分页变化：服务器页只显示已共享的打印机，设备列表页显示全部
    RenderPrinterList();

    RegisterCardSurfaces();
    Invalidate(FALSE);
}

LRESULT CPrintServerDlg::OnNavSelectionChanged(WPARAM wParam, LPARAM /*lParam*/)
{
    const int index = (int)wParam;

    // 四个导航项都是分页：0 服务器 / 1 设备列表 / 2 运行日志 / 3 软件信息
    m_navBar.SetSelection(index);
    ShowPage(index);
    return 0;
}

// ===========================================================================
// 绘制
// ===========================================================================

void CPrintServerDlg::OnNcCalcSize(BOOL bCalcValidRects, NCCALCSIZE_PARAMS* lpncsp)
{
    CDialogEx::OnNcCalcSize(bCalcValidRects, lpncsp);

    // WinForms 的 FixedSingle 表单客户区为 554x641（非客户区 3,26），系统给
    // 对话框的默认非客户区是 8,31（客户区 544x631）；这里把客户区向外扩展
    // kFrameShave，使窗口尺寸、内容位置与 .NET 版一致（最大化时边框不同，不扩展）
    if (!IsZoomed())
    {
        lpncsp->rgrc[0].left -= kFrameShave;
        lpncsp->rgrc[0].top -= kFrameShave;
        lpncsp->rgrc[0].right += kFrameShave;
        lpncsp->rgrc[0].bottom += kFrameShave;
    }
}

void CPrintServerDlg::OnPaint()
{
    CPaintDC dc(this);
    HDC hdc = dc.GetSafeHdc();

    CRect client;
    GetClientRect(&client);

    // ---- 工作区页面底色（左侧导航栏有独立背景，由 CNavBar 自行绘制） ----
    RECT work = { kWorkLeft, 0, client.right, client.bottom };
    ::FillRect(hdc, &work, Ui::GetSurfaceBrush(kFormBack));

    // ---- 页面标题（随导航栏切换） ----
    ::SetBkMode(hdc, TRANSPARENT);

    HGDIOBJ oldFont = ::SelectObject(hdc, Ui::GetFont(kYahei, 13, true));
    ::SetTextColor(hdc, Ui::Design::kTextTitle);
    RECT titleRect = { kCardLeft, kPageHeadTop, kCardLeft + 320, kPageHeadTop + kPageHeadHeight };
    ::DrawTextW(hdc, m_pageTitle, -1, &titleRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    ::SelectObject(hdc, oldFont);

    // ---- 卡片底色与标题（装饰层画在对话框 DC 上，卡内控件随后绘制在其上） ----
    HFONT cardFont = Ui::GetFont(kYahei, 10, true);

    if (IsCardVisible(0))
        Ui::DrawCardFace(hdc, m_cardRects[0], L"服务器控制", cardFont);

    if (IsCardVisible(4))
    {
        Ui::DrawCardFace(hdc, m_cardRects[4], L"服务器信息", cardFont);
        DrawServerInfo(hdc, m_cardRects[4]);
    }

    if (IsCardVisible(1))
    {
        // 服务器页只列出已共享的打印机；设备列表页列出本机全部打印机
        Ui::DrawCardFace(hdc, m_cardRects[1],
                         (m_page == 1) ? L"本机打印机" : L"打印机共享列表", cardFont);
    }

    if (IsCardVisible(2))
        Ui::DrawCardFace(hdc, m_cardRects[2], L"运行日志", cardFont);

    if (IsCardVisible(3))
    {
        Ui::DrawCardFace(hdc, m_cardRects[3], L"软件信息", cardFont);
        DrawSoftwareInfo(hdc, m_cardRects[3]);
    }
}

// 服务器信息卡内容：主机名 / 本机 IP
// 与「软件信息」页一样直接画在对话框 DC 上（不建子窗口：避免遮挡与 z 序问题，
// 文字也能精确按行居中，不受 Static 贴顶绘制的影响）
void CPrintServerDlg::DrawServerInfo(HDC hdc, const CRect& card)
{
    struct Row
    {
        std::wstring key;
        std::wstring value;
        bool mono;
        bool compact;   // 用小一号字体（IPP 地址较长）
    };

    std::vector<Row> rows;

    rows.push_back({ L"主机名", m_infoHost.empty() ? std::wstring(L"-") : m_infoHost, false, false });
    rows.push_back({ L"本机 IP", m_infoIp.empty() ? std::wstring(L"-") : m_infoIp, true, false });

    // ---- IPP 免驱地址：每台已共享打印机一行（端口号与下方列表一致）----
    std::string ippHost = TrimAscii(WideToUtf8(m_infoIp));
    const size_t comma = ippHost.find(',');
    if (comma != std::string::npos)
        ippHost = TrimAscii(ippHost.substr(0, comma));

    int ippPort = atoi(ConfigHelper::GetSetting("IppPort", "631").c_str());
    if (ippPort <= 0)
        ippPort = 631;

    const size_t kMaxUrlRows = 3;
    size_t sharedCount = 0;

    for (size_t i = 0; i < m_printerEntries.size(); ++i)
    {
        const PrinterEntry& entry = m_printerEntries[i];
        if (!entry.shared || entry.slot < 0)
            continue;

        ++sharedCount;
        if (sharedCount > kMaxUrlRows)
            continue;

        // 末段用 RAW 端口号（9100/9101…），与列表「端口」列和实际路由一致；
        // 不能用内部槽位 slot（= 端口 - 9100），否则会显示成 /ipp/print/0。
        const std::string url = "http://" + ippHost + ":" + std::to_string(ippPort) +
                                "/ipp/print/" + entry.port;

        rows.push_back({ sharedCount == 1 ? std::wstring(L"IPP 免驱") : std::wstring(),
                         Utf8ToWide(url), true, true });
    }

    if (sharedCount > kMaxUrlRows && rows.size() > 3)
    {
        // 放不下时，最后一行改成折叠提示
        rows.pop_back();
        rows.push_back({ std::wstring(),
                         L"… 共 " + std::to_wstring(sharedCount) + L" 台（地址见下方列表）",
                         false, false });
    }

    const int pad = Ui::Design::kCardPad;
    const int keyWidth = 76;
    const int rowHeight = 30;

    int top = card.top + Ui::Design::kCardContentTop;
    ::SetBkMode(hdc, TRANSPARENT);

    for (size_t i = 0; i < rows.size(); ++i)
    {
        RECT keyRect = { card.left + pad, top, card.left + pad + keyWidth, top + rowHeight };
        HGDIOBJ old = ::SelectObject(hdc, Ui::GetFont(kYahei, 9, false));
        ::SetTextColor(hdc, kLabelMuted);
        ::DrawTextW(hdc, rows[i].key.c_str(), -1, &keyRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        ::SelectObject(hdc, old);

        RECT valueRect = { card.left + pad + keyWidth, top, card.right - pad, top + rowHeight };
        HFONT valueFont = rows[i].compact ? Ui::GetFont(L"Consolas", 8, false)
                                          : (rows[i].mono ? Ui::GetFont(L"Consolas", 9, true)
                                                          : Ui::GetFont(kYahei, 9, true));
        old = ::SelectObject(hdc, valueFont);
        ::SetTextColor(hdc, Ui::Design::kTextBody);
        ::DrawTextW(hdc, rows[i].value.c_str(), -1, &valueRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        ::SelectObject(hdc, old);

        top += rowHeight;
    }
}

// 软件信息页内容：版本 / 运行模式 / 主机名 / 本机 IP
// 全部为只读展示，直接画在对话框 DC 上（不建子窗口，避免遮挡与 z 序问题）
void CPrintServerDlg::DrawSoftwareInfo(HDC hdc, const CRect& card)
{
    struct Row
    {
        const wchar_t* key;
        const wchar_t* value;
        COLORREF ink;
        bool mono;
    };

    const Row rows[] = {
        { L"软件版本", m_version.IsEmpty() ? L"-" : (const wchar_t*)m_version, Ui::Design::kTextBody, false },
        { L"运行模式", m_infoMode.empty() ? L"-" : m_infoMode.c_str(), Ui::Design::kTextBody, false },
        { L"主机名", m_infoHost.empty() ? L"-" : m_infoHost.c_str(), Ui::Design::kTextBody, false },
        { L"本机 IP", m_infoIp.empty() ? L"-" : m_infoIp.c_str(), Ui::Design::kTextBody, false },
    };

    const int pad = Ui::Design::kCardPad;
    const int keyWidth = 76;
    const int rowHeight = 30;

    int top = card.top + Ui::Design::kCardContentTop;
    ::SetBkMode(hdc, TRANSPARENT);

    for (size_t i = 0; i < _countof(rows); ++i)
    {
        RECT keyRect = { card.left + pad, top, card.left + pad + keyWidth, top + rowHeight };
        HGDIOBJ old = ::SelectObject(hdc, Ui::GetFont(kYahei, 9, false));
        ::SetTextColor(hdc, kLabelMuted);
        ::DrawTextW(hdc, rows[i].key, -1, &keyRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        ::SelectObject(hdc, old);

        RECT valueRect = { card.left + pad + keyWidth, top, card.right - pad, top + rowHeight };
        old = ::SelectObject(hdc, rows[i].mono ? Ui::GetFont(L"Consolas", 9, true)
                                               : Ui::GetFont(kYahei, 9, true));
        ::SetTextColor(hdc, rows[i].ink);
        ::DrawTextW(hdc, rows[i].value, -1, &valueRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        ::SelectObject(hdc, old);

        top += rowHeight;
    }
}

void CPrintServerDlg::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct)
{
    if (!Ui::DrawPart(lpDrawItemStruct))
        CDialogEx::OnDrawItem(nIDCtl, lpDrawItemStruct);
}

HBRUSH CPrintServerDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
    HWND control = pWnd->GetSafeHwnd();

    if (control == ::GetDlgItem(GetSafeHwnd(), IDC_PS_LOG) &&
        (nCtlColor == CTLCOLOR_STATIC || nCtlColor == CTLCOLOR_EDIT))
    {
        // 日志框：深色面板
        pDC->SetBkColor(kLogBack);
        pDC->SetTextColor(kLogText);
        return (HBRUSH)m_logBrush.GetSafeHandle();
    }

    if (nCtlColor == CTLCOLOR_STATIC || nCtlColor == CTLCOLOR_EDIT ||
        nCtlColor == CTLCOLOR_BTN)
    {
        pDC->SetBkMode(TRANSPARENT);

        COLORREF color = Ui::Design::kTextBody;
        Ui::TryGetTextColor(control, color);
        pDC->SetTextColor(color);

        // 控件落在哪张卡片上就用哪张卡片的表面色（对应 WinForms 的透明父容器继承）
        COLORREF surface = kFormBack;
        Ui::TryGetSurfaceColor(control, surface);
        return Ui::GetSurfaceBrush(surface);
    }

    if (nCtlColor == CTLCOLOR_DLG)
        return (HBRUSH)m_formBrush.GetSafeHandle();

    return CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);
}

BOOL CPrintServerDlg::OnEraseBkgnd(CDC* pDC)
{
    CRect client;
    GetClientRect(&client);
    pDC->FillSolidRect(&client, kFormBack);
    return TRUE;
}

// ===========================================================================
// 列表自绘
// ===========================================================================

void CPrintServerDlg::SetHoverRow(int row)
{
    if (row == m_hoverRow)
        return;

    const int previous = m_hoverRow;
    m_hoverRow = row;

    if (m_printerList.GetSafeHwnd() != nullptr)
    {
        if (previous >= 0)
            m_printerList.RedrawItems(previous, previous);
        if (row >= 0)
            m_printerList.RedrawItems(row, row);
    }
}

void CPrintServerDlg::OnPrinterListCustomDraw(NMHDR* pNMHDR, LRESULT* pResult)
{
    NMLVCUSTOMDRAW* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(pNMHDR);
    *pResult = CDRF_DODEFAULT;

    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
    {
        *pResult = CDRF_NOTIFYITEMDRAW;
        return;
    }

    if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
    {
        DrawPrinterRow(draw);
        *pResult = CDRF_SKIPDEFAULT;
    }
}

void CPrintServerDlg::DrawPrinterRow(const NMLVCUSTOMDRAW* draw)
{
    const int row = static_cast<int>(draw->nmcd.dwItemSpec);
    HDC hdc = draw->nmcd.hdc;

    // 整行矩形用 LVM_GETITEMRECT(LVIR_BOUNDS)；子项矩形只取其左右边界
    // （LVM_GETSUBITEMRECT 返回的是子项内容矩形，高度小于整行）
    RECT itemRect = { LVIR_BOUNDS, 0, 0, 0 };
    RECT nameRect = { LVIR_BOUNDS, 1, 0, 0 };
    RECT portRect = { LVIR_BOUNDS, 2, 0, 0 };
    RECT statusRect = { LVIR_BOUNDS, 3, 0, 0 };
    const HWND list = m_printerList.GetSafeHwnd();
    ::SendMessageW(list, LVM_GETITEMRECT, row, (LPARAM)&itemRect);
    ::SendMessageW(list, LVM_GETSUBITEMRECT, row, (LPARAM)&nameRect);
    ::SendMessageW(list, LVM_GETSUBITEMRECT, row, (LPARAM)&portRect);
    ::SendMessageW(list, LVM_GETSUBITEMRECT, row, (LPARAM)&statusRect);

    RECT rowRect = itemRect;
    if (rowRect.right <= rowRect.left || rowRect.bottom <= rowRect.top)
        rowRect = draw->nmcd.rc;

    if (nameRect.right <= nameRect.left)
    {
        nameRect = rowRect;
        nameRect.left = rowRect.left + kColCheckWidth;
        nameRect.right = nameRect.left + kColNameWidth;
    }

    if (portRect.right <= portRect.left)
    {
        portRect = rowRect;
        portRect.left = rowRect.left + kColCheckWidth + kColNameWidth;
        portRect.right = portRect.left + kColPortWidth;
    }

    if (statusRect.right <= statusRect.left)
    {
        statusRect = rowRect;
        statusRect.left = rowRect.left + kColCheckWidth + kColNameWidth + kColPortWidth;
        statusRect.right = statusRect.left + kColStatusWidth;
    }

    const int rowHeight = rowRect.bottom - rowRect.top;

    const bool selected = (m_printerList.GetItemState(row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    const bool hovered = (row == m_hoverRow);
    const bool checked = GetRowChecked(row);

    // 行背景（原型 .table tbody tr：斑马纹 #FAFCFE / 悬停 #EFF5FF / 选中 #E3EDFF）
    COLORREF back = (row % 2 == 0) ? RGB(250, 252, 254) : Ui::Design::kCardBack;
    if (selected)
        back = RGB(227, 237, 255);
    else if (hovered)
        back = RGB(239, 245, 255);
    ::FillRect(hdc, &rowRect, Ui::GetSurfaceBrush(back));

    // 行分隔线
    RECT line = { rowRect.left + 12, rowRect.bottom - 1, rowRect.right - 12, rowRect.bottom };
    ::FillRect(hdc, &line, Ui::GetSurfaceBrush(Ui::Design::kDivider));

    // 复选框
    const int boxSize = 16;
    POINT box = { rowRect.left + 6, rowRect.top + (rowHeight - boxSize) / 2 };
    m_checkImages.Draw(CDC::FromHandle(hdc), checked ? 1 : 0, box, ILD_TRANSPARENT);

    ::SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ oldFont = ::SelectObject(hdc, Ui::GetFont(kYahei, 9, false));

    // 打印机名称（原型 td.name：t1 色 + 600；选中行整行变 t1 加粗）
    std::wstring name = GetListText(row, 1);
    RECT nameTextRect = nameRect;
    nameTextRect.left += 8;
    nameTextRect.right -= 6;
    if (nameTextRect.right > nameTextRect.left)
    {
        HFONT nameFont = Ui::GetFont(kYahei, 9, selected || hovered);
        HGDIOBJ oldNameFont = ::SelectObject(hdc, nameFont);
        ::SetTextColor(hdc, Ui::Design::kTextTitle);
        ::DrawTextW(hdc, name.c_str(), -1, &nameTextRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        ::SelectObject(hdc, oldNameFont);
    }

    // 端口胶囊（原型 .tpill：20 高 / 左右各 8 内边距 / 品牌浅底 + 品牌字，无描边）
    std::wstring port = GetListText(row, 2);
    if (!port.empty())
    {
        RECT calc = { 0, 0, 0, 0 };
        ::DrawTextW(hdc, port.c_str(), -1, &calc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);

        const int pillWidth = (calc.right - calc.left) + 16;
        const int pillHeight = 20;
        const int centerX = (portRect.left + portRect.right) / 2;
        RECT pill = { centerX - pillWidth / 2, rowRect.top + (rowHeight - pillHeight) / 2,
                      centerX + pillWidth / 2, rowRect.top + (rowHeight + pillHeight) / 2 };

        Ui::Gfx::FillRoundRect(hdc, pill, pillHeight / 2, Ui::Design::kAccentSoft);
        ::SetTextColor(hdc, Ui::Design::kAccent);
        ::DrawTextW(hdc, port.c_str(), -1, &pill,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // 状态胶囊（由勾选状态派生：已启用=语义绿，已停用=中性灰）
    {
        const wchar_t* statusText = checked ? L"已启用" : L"已停用";
        const COLORREF ink = checked ? Ui::Design::kSuccess : Ui::Design::kTextMuted;
        const COLORREF soft = checked ? Ui::Design::kSuccessSoft : Ui::Design::kNeutralSoft;

        RECT calc = { 0, 0, 0, 0 };
        ::DrawTextW(hdc, statusText, -1, &calc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);

        const int pillWidth = (calc.right - calc.left) + 16;
        const int pillHeight = 20;
        const int centerX = (statusRect.left + statusRect.right) / 2;
        RECT pill = { centerX - pillWidth / 2, rowRect.top + (rowHeight - pillHeight) / 2,
                      centerX + pillWidth / 2, rowRect.top + (rowHeight + pillHeight) / 2 };

        if (pill.left >= rowRect.left && pill.right <= rowRect.right)
        {
            Ui::Gfx::FillRoundRect(hdc, pill, pillHeight / 2, soft);
            ::SetTextColor(hdc, ink);
            ::DrawTextW(hdc, statusText, -1, &pill,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }

    ::SelectObject(hdc, oldFont);
}

// ===========================================================================
// 打印机列表
// ===========================================================================

std::wstring CPrintServerDlg::GetListText(int row, int column)
{
    if (row < 0 || row >= m_printerList.GetItemCount())
        return std::wstring();

    CString text = m_printerList.GetItemText(row, column);
    return std::wstring((LPCWSTR)text);
}

void CPrintServerDlg::SetListText(int row, int column, const std::wstring& text)
{
    if (row < 0 || row >= m_printerList.GetItemCount())
        return;

    m_printerList.SetItemText(row, column, text.c_str());
}

bool CPrintServerDlg::GetRowChecked(int row)
{
    if (row < 0 || row >= m_printerList.GetItemCount())
        return false;

    return (m_printerList.GetItemState(row, LVIS_STATEIMAGEMASK) >> 12) == 2;
}

void CPrintServerDlg::SetRowChecked(int row, bool checked)
{
    if (row < 0 || row >= m_printerList.GetItemCount())
        return;

    m_printerList.SetItemState(row, INDEXTOSTATEIMAGEMASK(checked ? 2 : 1),
                               LVIS_STATEIMAGEMASK);

    // 服务器信息卡里的「已共享打印机」跟着勾选变化，重绘该卡片
    if (m_page == 0)
        ::InvalidateRect(GetSafeHwnd(), &m_cardRects[4], FALSE);
}

void CPrintServerDlg::LoadPrinters()
{
    RefreshPrinterList();
}

void CPrintServerDlg::RefreshPrinterList()
{
    std::vector<std::string> physicalPrinters;

    // ---- 优先 WMI：SELECT Name, PortName, DriverName FROM Win32_Printer ----
    // 驱动名一并取出：不少虚拟打印机（PDF/XPS/传真）名称很普通，
    // 只能靠驱动名识别。
    bool wmiFailed = false;
    {
        WmiQuery query;
        if (!query.IsValid())
        {
            wmiFailed = true;
        }
        else
        {
            wmiFailed = !query.ForEach(L"SELECT Name, PortName, DriverName FROM Win32_Printer",
                [&physicalPrinters](IWbemClassObject* object) {
                    std::wstring name;
                    std::wstring portName;
                    std::wstring driverName;

                    if (WmiQuery::GetString(object, L"Name", name))
                        name = TrimWide(name);

                    WmiQuery::GetString(object, L"PortName", portName);
                    WmiQuery::GetString(object, L"DriverName", driverName);

                    if (!name.empty() &&
                        !IsVirtualPrinter(WideToUtf8(name), WideToUtf8(portName),
                                          WideToUtf8(driverName)))
                    {
                        physicalPrinters.push_back(WideToUtf8(name));
                    }

                    return true;
                });
        }
    }

    // ---- 回退：EnumPrinters 本地打印机 ----
    // 必须用 level 2：只有 PRINTER_INFO_2 同时带端口名与驱动名，否则回退分支
    // 会绕过虚拟打印机过滤（历史上就是这里漏掉的）。
    if (wmiFailed || physicalPrinters.empty())
    {
        physicalPrinters.clear();

        DWORD needed = 0;
        DWORD returned = 0;
        ::EnumPrintersW(PRINTER_ENUM_LOCAL, nullptr, 2, nullptr, 0, &needed, &returned);

        if (needed > 0)
        {
            std::vector<unsigned char> buffer(needed);
            if (::EnumPrintersW(PRINTER_ENUM_LOCAL, nullptr, 2, buffer.data(), needed,
                                &needed, &returned))
            {
                PRINTER_INFO_2W* printers = reinterpret_cast<PRINTER_INFO_2W*>(buffer.data());
                for (DWORD i = 0; i < returned; ++i)
                {
                    std::wstring name = TrimWide(printers[i].pPrinterName != nullptr
                                                     ? printers[i].pPrinterName
                                                     : L"");
                    if (name.empty())
                        continue;

                    std::wstring portName = TrimWide(printers[i].pPortName != nullptr
                                                          ? printers[i].pPortName
                                                          : L"");
                    std::wstring driverName = TrimWide(printers[i].pDriverName != nullptr
                                                            ? printers[i].pDriverName
                                                            : L"");

                    if (!IsVirtualPrinter(WideToUtf8(name), WideToUtf8(portName),
                                          WideToUtf8(driverName)))
                    {
                        physicalPrinters.push_back(WideToUtf8(name));
                    }
                }
            }
        }
    }

    // 重建数据集：已存在的打印机保留内存中的勾选/端口/槽位（用户尚未保存的即时修改），
    // 新出现的打印机按 App.config 的 PrinterN_Name 恢复勾选状态、端口与槽位
    const std::vector<ConfigHelper::PrinterSlot> savedSlots = ConfigHelper::GetPrinterSlots();

    std::vector<PrinterEntry> entries;
    entries.reserve(physicalPrinters.size());

    for (size_t i = 0; i < physicalPrinters.size(); ++i)
    {
        const std::string& name = physicalPrinters[i];

        PrinterEntry entry;
        entry.name = name;

        const int existing = FindPrinterEntry(name);
        if (existing >= 0)
        {
            entry = m_printerEntries[existing];
        }
        else
        {
            for (size_t s = 0; s < savedSlots.size(); ++s)
            {
                if (savedSlots[s].name == name)
                {
                    entry.shared = savedSlots[s].enabled;
                    entry.port = std::to_string(savedSlots[s].port);
                    break;
                }
            }
        }

        // 槽位由端口推导：9100 -> 0（/ipp/print）、9101 -> 1（/ipp/print/1）…
        // 未分配端口的打印机槽位为 -1，勾选时由 ReallocatePorts 统一分配。
        entry.slot = PortToSlotIndex(entry.port);

        entries.push_back(entry);
    }

    m_printerEntries.swap(entries);

    RenderPrinterList();
}

int CPrintServerDlg::FindPrinterEntry(const std::string& name) const
{
    const std::string key = ToLowerAscii(name);

    for (size_t i = 0; i < m_printerEntries.size(); ++i)
    {
        if (ToLowerAscii(m_printerEntries[i].name) == key)
            return (int)i;
    }
    return -1;
}

// 按当前分页把数据集渲染到列表：
//   设备列表页 → 本机全部打印机（可勾选、改端口）
//   服务器页   → 只列出已共享的打印机（只读展示）
// 设备列表页的列表行号与 m_printerEntries 下标一一对应（端口编辑据此回写数据集）
void CPrintServerDlg::RenderPrinterList()
{
    const bool readOnly = (m_page != 1);

    m_suppressListEvents = true;

    m_printerList.DeleteAllItems();
    m_hoverRow = -1;

    for (size_t i = 0; i < m_printerEntries.size(); ++i)
    {
        const PrinterEntry& entry = m_printerEntries[i];

        if (readOnly && !entry.shared)
            continue;

        const int row = m_printerList.InsertItem(m_printerList.GetItemCount(), L"");
        SetListText(row, 1, Utf8ToWide(entry.name));
        SetListText(row, 2, Utf8ToWide(entry.port));
        SetRowChecked(row, entry.shared);
    }

    m_suppressListEvents = false;

    UpdatePrinterCountPill();
}

// 列表卡片标题行右侧的「共 N 台」胶囊（宽度随文字自适应，与全选/取消按钮一并重排）
void CPrintServerDlg::UpdatePrinterCountPill()
{
    // 服务器页显示的是已共享数量，设备列表页显示本机打印机总数
    const int count = m_printerList.GetItemCount();

    std::wstring text = (m_page == 1)
        ? (L"共 " + std::to_wstring(count) + L" 台")
        : (L"已共享 " + std::to_wstring(count) + L" 台");

    Ui::SetLabelText(this, IDC_PS_PRINTER_COUNT, text.c_str());
    LayoutPrinterCard(m_printerCardTop, m_printerCardHeight);

    HWND pill = ::GetDlgItem(GetSafeHwnd(), IDC_PS_PRINTER_COUNT);
    if (pill != nullptr)
        ::InvalidateRect(pill, nullptr, TRUE);
}

bool CPrintServerDlg::IsVirtualPrinter(const std::string& printerName, const std::string& portName,
                                       const std::string& driverName)
{
    // 判定三个维度：端口（无真实设备）→ 名称 → 驱动名。
    // 只排除"输出到文件/屏幕/传真"这类虚拟设备；通过 TCP/IP、WSD 端口安装的
    // 真实打印机仍然保留（现场常用服务端做二级中转，排除会造成功能倒退）。

    // 1. 端口：这些端口没有对应的物理设备
    if (StartsWithIgnoreCaseAscii(portName, "PORTPROMPT:") ||
        StartsWithIgnoreCaseAscii(portName, "FILE:") ||
        EqualsIgnoreCaseAscii(portName, "NUL:") ||
        StartsWithIgnoreCaseAscii(portName, "SHRFAX:") ||
        StartsWithIgnoreCaseAscii(portName, "XPSPORT:") ||
        ContainsIgnoreCaseAscii(portName, "prompt") ||
        EqualsIgnoreCaseAscii(portName, "nul"))
    {
        return true;
    }

    // 2. 名称 / 驱动名关键词（统一小写比较）
    static const char* keywords[] = {
        // Windows 自带虚拟打印机
        "print to pdf", "print to file", "print to xps",
        "microsoft pdf", "microsoft print to pdf",
        "microsoft xps", "xps document",
        "document writer",
        "onenote", "send to",
        // 传真
        "fax", "传真",
        // 第三方 PDF/虚拟打印机
        "virtual", "adobe pdf", "cutepdf", "foxit", "bullzip",
        "dopdf", "pdf24", "nitro pdf", "nitropdf", "pdfcreator",
        "snagit", "anydesk", "teamviewer", "remote desktop", "easy print",
        "hp eprint",
    };

    const std::string nameLower = ToLowerAscii(printerName);
    const std::string driverLower = ToLowerAscii(driverName);

    for (size_t i = 0; i < _countof(keywords); ++i)
    {
        if (nameLower.find(keywords[i]) != std::string::npos)
            return true;
        if (!driverLower.empty() && driverLower.find(keywords[i]) != std::string::npos)
            return true;
    }

    return false;
}

void CPrintServerDlg::ReallocatePorts()
{
    // 端口与打印机绑定，取消勾选只改变"是否共享"，不清空端口：
    // 取消后再勾回来，端口与 IPP 端点仍是原来的（修复了端口错乱）。
    // 槽位由端口推导（端口 - 9100），因此 RAW 9100+i 与 /ipp/print/i 始终对应。
    const int basePort = 9100;

    std::set<int> taken;
    std::vector<size_t> needAssign;

    for (size_t i = 0; i < m_printerEntries.size(); ++i)
    {
        PrinterEntry& entry = m_printerEntries[i];

        int port = 0;
        bool valid = false;

        const std::string text = TrimAscii(entry.port);

        if (!text.empty())
        {
            char* end = nullptr;
            long parsed = strtol(text.c_str(), &end, 10);
            if (end != nullptr && *end == '\0')
            {
                port = (int)parsed;
                valid = (port >= basePort);
            }
        }

        if (valid && taken.find(port) == taken.end())
        {
            taken.insert(port);
            entry.port = std::to_string(port);
            entry.slot = port - basePort;
        }
        else
        {
            // 空/非法/与他人冲突：勾选中的先占位，下面统一分配；
            // 未勾选的保留空值，勾选时再分配
            entry.port.clear();
            entry.slot = -1;
            if (entry.shared)
                needAssign.push_back(i);
        }
    }

    int next = basePort;
    for (size_t i = 0; i < needAssign.size(); ++i)
    {
        while (taken.find(next) != taken.end())
            ++next;

        m_printerEntries[needAssign[i]].port = std::to_string(next);
        m_printerEntries[needAssign[i]].slot = next - basePort;
        taken.insert(next);
    }
}

std::vector<PrinterConfig> CPrintServerDlg::GetCheckedPrinterConfigs()
{
    std::vector<PrinterConfig> configs;

    for (size_t i = 0; i < m_printerEntries.size(); ++i)
    {
        const PrinterEntry& entry = m_printerEntries[i];
        if (!entry.shared)
            continue;

        int port = 0;
        const std::string portText = TrimAscii(entry.port);
        if (!portText.empty())
        {
            char* end = nullptr;
            long parsed = strtol(portText.c_str(), &end, 10);
            if (end != nullptr && *end == '\0' && parsed >= 9100)
                port = (int)parsed;
        }

        if (port <= 0)
            continue;   // ReallocatePorts 已保证有效；防御性跳过

        PrinterConfig config;
        config.name = entry.name;
        config.port = port;
        config.enabled = true;
        // 编号 = RAW 端口号：9100 → /ipp/print/9100，9101 → /ipp/print/9101
        config.index = port;
        configs.push_back(config);
    }

    return configs;
}

void CPrintServerDlg::LoadSavedSettings()
{
    std::string savedApiPort = ConfigHelper::GetSetting("ApiPort", "9180");

    char* end = nullptr;
    long apiPort = strtol(savedApiPort.c_str(), &end, 10);
    if (end != nullptr && *end == '\0' && !savedApiPort.empty())
    {
        SetApiPortText(std::to_string(apiPort));
        m_apiPortSpin.SetPos32((int)apiPort);
    }

    // 打印机的共享状态与端口由 RefreshPrinterList() 依据 App.config 统一恢复
}

void CPrintServerDlg::UpdateNetworkInfo()
{
    std::vector<std::string> addresses = NetworkHelper::GetAllNetworkAddresses();

    char computerName[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
    DWORD computerNameLen = _countof(computerName);
    if (!::GetComputerNameA(computerName, &computerNameLen))
        computerName[0] = '\0';

    std::string joined;
    for (size_t i = 0; i < addresses.size(); ++i)
    {
        if (i > 0)
            joined += ", ";
        joined += addresses[i];
    }

    // 导航栏底部不再放本机信息块（主机名/IP 在「服务器信息」卡与「软件信息」页展示）
    m_infoIp = joined.empty() ? L"-" : Utf8ToWide(joined);
    m_infoHost = Utf8ToWide(computerName);

    if (m_page == 0 || m_page == 3)
        Invalidate(FALSE);
}

void CPrintServerDlg::LoadExistingLog()
{
    std::vector<std::string> lines = Log::GetRecentLogLines(500);
    int lineCount = Log::GetTodayLogLineCount();

    for (size_t i = 0; i < lines.size(); ++i)
        AppendLogLine(lines[i]);

    if (lineCount > 0)
    {
        SYSTEMTIME now;
        ::GetLocalTime(&now);

        char stamp[64] = { 0 };
        _snprintf_s(stamp, sizeof(stamp), _TRUNCATE, "[%04d-%02d-%02d %02d:%02d:%02d]",
                    now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

        AppendLogLine(std::string(stamp) + " --- 已加载今日日志，共 " +
                      std::to_string(lineCount) + " 行 ---");
    }

    m_lastLogFileContent = Log::ReadTodayLog();
}

void CPrintServerDlg::AutoRefreshServiceLog()
{
    if (m_isRunning)
        return;

    std::string result = RunCmd(L"sc", L"query PrintServer", false);
    if (result.find("RUNNING") == std::string::npos)
        return;

    std::string currentContent = Log::ReadTodayLog();

    // 基于文本内容比较，找出真正新增的部分
    if (currentContent.size() > m_lastLogFileContent.size() &&
        currentContent.compare(0, m_lastLogFileContent.size(), m_lastLogFileContent) == 0)
    {
        std::string newContent = TrimAscii(currentContent.substr(m_lastLogFileContent.size()));
        if (!newContent.empty())
            AppendLogLine(newContent);
    }
    else if (!currentContent.empty() && currentContent != m_lastLogFileContent)
    {
        // 文件被重写或清空过，追加全部
        AppendLogLine(currentContent);
    }

    m_lastLogFileContent = currentContent;
}

void CPrintServerDlg::UpdateServiceStatus()
{
    std::string result = RunCmd(L"sc", L"query PrintServer", false);

    if (result.find("RUNNING") != std::string::npos)
    {
        if (!m_isRunning)
        {
            SetHeadPill(IDC_PS_STATUS, L"运行中", kColorGreen);

            m_infoMode = L"Windows 服务";

            ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PS_BTN_STARTSTOP), FALSE);
        }

        Ui::SetButtonText(this, IDC_PS_BTN_INSTALL, L"卸载服务");
        Ui::SetOutlineButtonColor(this, IDC_PS_BTN_INSTALL, kBtnServiceStop, kBtnServiceStop);
        Ui::SetButtonText(this, IDC_PS_BTN_SERVICE_START, L"停止服务");
    }
    else if (result.find("STOPPED") != std::string::npos)
    {
        if (!m_isRunning)
        {
            SetHeadPill(IDC_PS_STATUS, L"已停止", kColorOrangeRed);

            m_infoMode = L"Windows 服务 (未运行)";

            ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PS_BTN_STARTSTOP), TRUE);
        }

        Ui::SetButtonText(this, IDC_PS_BTN_INSTALL, L"卸载服务");
        Ui::SetOutlineButtonColor(this, IDC_PS_BTN_INSTALL, kBtnServiceStop, kBtnServiceStop);
        Ui::SetButtonText(this, IDC_PS_BTN_SERVICE_START, L"启动服务");
    }
    else if (result.find("1060") != std::string::npos)
    {
        if (!m_isRunning)
        {
            SetHeadPill(IDC_PS_STATUS, L"已停止", kColorRed);

            m_infoMode = L"-";

            ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PS_BTN_STARTSTOP), TRUE);
        }

        Ui::SetButtonText(this, IDC_PS_BTN_INSTALL, L"安装服务");
        Ui::SetOutlineButtonColor(this, IDC_PS_BTN_INSTALL, kGroupText, Ui::Design::kFieldBorder);
        Ui::SetButtonText(this, IDC_PS_BTN_SERVICE_START, L"启动服务");
    }
}

std::string CPrintServerDlg::GetApiPortText()
{
    wchar_t buffer[64] = { 0 };
    ::GetWindowTextW(::GetDlgItem(GetSafeHwnd(), IDC_PS_API_PORT_EDIT), buffer, _countof(buffer));
    return WideToUtf8(buffer);
}

void CPrintServerDlg::SetApiPortText(const std::string& port)
{
    ::SetWindowTextW(::GetDlgItem(GetSafeHwnd(), IDC_PS_API_PORT_EDIT), Utf8ToWide(port).c_str());
}

void CPrintServerDlg::SaveSettings()
{
    Config::SetSetting("ApiPort", GetApiPortText());

    // 按稳定槽位落盘：未勾选的打印机保留 Name/Port，只把 Enabled 置 false。
    // 取消勾选再勾回来，端口与 IPP 端点保持不变；其它打印机的键名也不会整体前移。
    int maxSlot = -1;

    for (size_t i = 0; i < m_printerEntries.size(); ++i)
    {
        if (m_printerEntries[i].slot > maxSlot)
            maxSlot = m_printerEntries[i].slot;
    }

    Config::SetSetting("PrinterCount", std::to_string(maxSlot + 1));

    for (size_t i = 0; i < m_printerEntries.size(); ++i)
    {
        const PrinterEntry& entry = m_printerEntries[i];
        if (entry.slot < 0)
            continue;

        const std::string prefix = "Printer" + std::to_string(entry.slot) + "_";
        Config::SetSetting(prefix + "Name", entry.name);
        Config::SetSetting(prefix + "Port", entry.port);
        Config::SetSetting(prefix + "Enabled", entry.shared ? "true" : "false");
    }

    // 清理历史残留的高编号槽位（打印机卸载后遗留的 PrinterN_*）
    for (int i = maxSlot + 1; i < 64; ++i)
    {
        const std::string prefix = "Printer" + std::to_string(i) + "_";
        if (!Config::HasSetting(prefix + "Name"))
            continue;

        Config::SetSetting(prefix + "Name", "");
        Config::SetSetting(prefix + "Port", "");
        Config::SetSetting(prefix + "Enabled", "false");
    }

    // 兼容旧配置（单打印机模式 / 旧版客户端）
    const std::vector<PrinterConfig> configs = GetCheckedPrinterConfigs();
    Config::SetSetting("PrinterName", configs.empty() ? "" : configs[0].name);
    Config::SetSetting("Port", configs.empty() ? "9100" : std::to_string(configs[0].port));
}

std::string CPrintServerDlg::RunCmd(const std::wstring& fileName, const std::wstring& arguments,
                                    bool showLog)
{
    // 对应 C# Process{UseShellExecute=false, RedirectStandardOutput=true, CreateNoWindow=true}
    SECURITY_ATTRIBUTES attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE stdOutRead = nullptr;
    HANDLE stdOutWrite = nullptr;
    HANDLE stdErrRead = nullptr;
    HANDLE stdErrWrite = nullptr;

    if (!::CreatePipe(&stdOutRead, &stdOutWrite, &attributes, 0) ||
        !::CreatePipe(&stdErrRead, &stdErrWrite, &attributes, 0))
    {
        if (stdOutRead != nullptr) ::CloseHandle(stdOutRead);
        if (stdOutWrite != nullptr) ::CloseHandle(stdOutWrite);
        return "异常: 无法创建管道";
    }

    ::SetHandleInformation(stdOutRead, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(stdErrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = stdOutWrite;
    startup.hStdError = stdErrWrite;

    std::wstring commandLine = fileName + L" " + arguments;
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    PROCESS_INFORMATION process;
    memset(&process, 0, sizeof(process));

    BOOL created = ::CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                    CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);

    ::CloseHandle(stdOutWrite);
    ::CloseHandle(stdErrWrite);

    if (!created)
    {
        ::CloseHandle(stdOutRead);
        ::CloseHandle(stdErrRead);
        return "异常: 无法启动进程（错误码 " + std::to_string(::GetLastError()) + "）";
    }

    std::string result;

    auto drain = [](HANDLE pipe, std::string& target) {
        std::vector<char> bytes;
        char buffer[4096];
        DWORD read = 0;

        while (::ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0)
            bytes.insert(bytes.end(), buffer, buffer + read);

        AppendUtf8FromAnsi(bytes, target);
    };

    drain(stdOutRead, result);
    drain(stdErrRead, result);

    ::WaitForSingleObject(process.hProcess, INFINITE);

    ::CloseHandle(stdOutRead);
    ::CloseHandle(stdErrRead);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);

    if (showLog)
    {
        LogToTextBox("执行: " + WideToUtf8(fileName) + " " + WideToUtf8(arguments));

        std::string trimmed = TrimAscii(result);
        if (!trimmed.empty())
            LogToTextBox("返回: " + trimmed);
    }

    return result;
}

void CPrintServerDlg::StartServer()
{
    std::vector<PrinterConfig> configs = GetCheckedPrinterConfigs();
    if (configs.empty())
    {
        ::MessageBoxW(GetSafeHwnd(), L"请至少勾选一台打印机！", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    SaveSettings();

    char hostName[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
    DWORD hostNameLen = _countof(hostName);
    if (!::GetComputerNameA(hostName, &hostNameLen))
        hostName[0] = '\0';

    std::vector<std::string> addresses = NetworkHelper::GetAllNetworkAddresses();
    std::string joined;
    for (size_t i = 0; i < addresses.size(); ++i)
    {
        if (i > 0)
            joined += ", ";
        joined += addresses[i];
    }

    LogToTextBox("打印服务器启动 - 主机:" + std::string(hostName) + "  IP:" + joined);
    LogToTextBox("正在启动 " + std::to_string(configs.size()) + " 台打印机...");

    HWND hwnd = GetSafeHwnd();

    try
    {
        for (size_t i = 0; i < configs.size(); ++i)
        {
            std::unique_ptr<TcpPrintServer> server(
                new TcpPrintServer(configs[i].name, configs[i].port));

            server->OnLog = [hwnd](const std::string& message) {
                // 对应 C# 的 InvokeRequired/Invoke：日志回调整理到界面线程
                std::string* payload = new std::string(message);
                if (!::PostMessageW(hwnd, kGuiLogMessage, 0, (LPARAM)payload))
                    delete payload;
            };

            server->Start();
            m_servers.push_back(std::move(server));
        }

        // 启动 HTTP API 服务
        std::string apiPortText = GetApiPortText();
        int apiPort = atoi(apiPortText.c_str());
        if (apiPort <= 0)
            apiPort = 9180;

        m_apiServer = std::make_unique<HttpApiServer>(configs, apiPort);
        m_apiServer->OnLog = [hwnd](const std::string& message) {
            std::string* payload = new std::string(message);
            if (!::PostMessageW(hwnd, kGuiLogMessage, 0, (LPARAM)payload))
                delete payload;
        };
        m_apiServer->Start();

        // ---- IPP 服务（默认 631）：与 RAW / 控制 API 并行 ----
        // 与 C# 版一致，IPP 启动失败只记日志，不能拖垮已经可用的 RAW 与 API。
        const std::string ippEnabledText =
            ToLowerAscii(ConfigHelper::GetSetting("IppEnabled", "true"));

        const bool ippEnabled = !(ippEnabledText == "false" || ippEnabledText == "0" ||
                                  ippEnabledText == "no" || ippEnabledText == "off");

        if (ippEnabled)
        {
            int ippPort = atoi(ConfigHelper::GetSetting("IppPort", "631").c_str());
            if (ippPort <= 0 || ippPort > 65535)
                ippPort = 631;

            try
            {
                m_ippServer = std::make_shared<IppServer>(configs, ippPort);
                m_ippServer->OnLog = [hwnd](const std::string& message) {
                    std::string* payload = new std::string(message);
                    if (!::PostMessageW(hwnd, kGuiLogMessage, 0, (LPARAM)payload))
                        delete payload;
                };
                m_ippServer->Start();
            }
            catch (const std::exception& ippError)
            {
                LogToTextBox(std::string("IPP 服务启动失败（不影响 RAW/API）：") + ippError.what());
                m_ippServer.reset();
            }
        }
        else
        {
            LogToTextBox("IPP 服务已在配置中关闭 (IppEnabled=false)");
        }
    }
    catch (const std::exception& ex)
    {
        ::MessageBoxW(GetSafeHwnd(),
                      (L"启动失败：" + Utf8ToWide(ex.what())).c_str(),
                      L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    m_isRunning = true;

    Ui::SetButtonText(this, IDC_PS_BTN_STARTSTOP, L"停止服务器");
    Ui::SetFlatButtonColor(this, IDC_PS_BTN_STARTSTOP, kBtnServiceStop, kColorWhite);

    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PS_PRINTER_LIST), FALSE);
    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PS_API_PORT_EDIT), FALSE);
    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PS_BTN_INSTALL), FALSE);
    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PS_BTN_SERVICE_START), FALSE);

    SetHeadPill(IDC_PS_STATUS,
                (L"运行中 (" + std::to_wstring(configs.size()) + L"台打印机)").c_str(),
                kColorGreen);

    m_infoMode = L"应用程序";

    if (m_page == 0 || m_page == 3)
        Invalidate(FALSE);

    // 显示托盘图标 + 气泡提示
    ShowTrayIcon(configs);

    ::MessageBeep(0);
}

void CPrintServerDlg::StopServer()
{
    // 顺序与服务模式一致：先停 IPP，再停控制 API（HttpTerminate 由 API 统一终结）
    if (m_ippServer != nullptr)
    {
        m_ippServer->Stop();
        m_ippServer.reset();
    }

    if (m_apiServer != nullptr)
    {
        m_apiServer->Stop();
        m_apiServer.reset();
    }

    for (size_t i = 0; i < m_servers.size(); ++i)
        m_servers[i]->Stop();

    m_servers.clear();

    m_isRunning = false;

    Ui::SetButtonText(this, IDC_PS_BTN_STARTSTOP, L"启动服务器");
    Ui::SetFlatButtonColor(this, IDC_PS_BTN_STARTSTOP, kBtnServerStart, kColorWhite);

    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PS_PRINTER_LIST), TRUE);
    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PS_API_PORT_EDIT), TRUE);
    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PS_BTN_INSTALL), TRUE);
    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_PS_BTN_SERVICE_START), TRUE);

    SetHeadPill(IDC_PS_STATUS, L"已停止", kColorOrangeRed);

    m_infoMode = L"-";

    if (m_page == 0 || m_page == 3)
        Invalidate(FALSE);

    LogToTextBox("所有打印服务已停止");

    // 隐藏托盘图标
    HideTrayIcon();

    ::MessageBeep(MB_ICONASTERISK);
}

// ===========================================================================
// 日志
// ===========================================================================

void CPrintServerDlg::WriteLogToFile(const std::string& message)
{
    Log::WriteLog(message);
}

void CPrintServerDlg::AppendLogLine(const std::string& message)
{
    HWND log = ::GetDlgItem(GetSafeHwnd(), IDC_PS_LOG);
    if (log == nullptr)
        return;

    std::wstring line = Utf8ToWide(message) + L"\r\n";

    int length = ::GetWindowTextLengthW(log);
    ::SendMessageW(log, EM_SETSEL, length, length);
    ::SendMessageW(log, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
}

void CPrintServerDlg::LogToTextBox(const std::string& message)
{
    SYSTEMTIME now;
    ::GetLocalTime(&now);

    char stamp[64] = { 0 };
    _snprintf_s(stamp, sizeof(stamp), _TRUNCATE, "[%04d-%02d-%02d %02d:%02d:%02d] ",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

    std::string logMessage = std::string(stamp) + message;

    AppendLogLine(logMessage);
    WriteLogToFile(logMessage);
    m_lastLogFileContent = Log::ReadTodayLog();
}

// ===========================================================================
// 托盘图标（GUI 模式）
// ===========================================================================

bool CPrintServerDlg::EnsureTrayIcon()
{
    if (m_trayIcon == nullptr)
    {
        m_trayIcon = (HICON)::LoadImageW(::AfxGetInstanceHandle(),
                                         MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 0, 0,
                                         LR_DEFAULTSIZE | LR_SHARED);
        if (m_trayIcon == nullptr)
            m_trayIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    }

    if (m_trayIconVisible)
        return true;

    NOTIFYICONDATAW data;
    memset(&data, 0, sizeof(data));
    data.cbSize = sizeof(data);
    data.hWnd = GetSafeHwnd();
    data.uID = kTrayIconId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kTrayCallbackMessage;
    data.hIcon = m_trayIcon;
    wcscpy_s(data.szTip, L"网络打印服务器 - 未启动");

    if (!::Shell_NotifyIconW(NIM_ADD, &data))
        return false;

    m_trayIconVisible = true;
    return true;
}

void CPrintServerDlg::ShowTrayIcon(const std::vector<PrinterConfig>& configs)
{
    if (!EnsureTrayIcon())
        return;

    std::wstring printerInfo;
    if (configs.size() == 1)
    {
        printerInfo = Utf8ToWide(configs[0].name) + L" (端口:" +
                      std::to_wstring(configs[0].port) + L")";
    }
    else
    {
        printerInfo = std::to_wstring(configs.size()) + L"台打印机";
    }

    NOTIFYICONDATAW data;
    memset(&data, 0, sizeof(data));
    data.cbSize = sizeof(data);
    data.hWnd = GetSafeHwnd();
    data.uID = kTrayIconId;

    // 更新提示文字
    data.uFlags = NIF_TIP;
    wcsncpy_s(data.szTip, (L"网络打印服务器 - 运行中 | " + printerInfo).c_str(), _TRUNCATE);
    ::Shell_NotifyIconW(NIM_MODIFY, &data);

    // 气泡提示
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO;
    data.uTimeout = 3000;
    wcsncpy_s(data.szInfoTitle, L"网络打印服务器", _TRUNCATE);
    wcsncpy_s(data.szInfo, (L"打印服务器已启动 ✓\n" + printerInfo).c_str(), _TRUNCATE);
    ::Shell_NotifyIconW(NIM_MODIFY, &data);
}

void CPrintServerDlg::HideTrayIcon()
{
    if (!m_trayIconVisible)
        return;

    NOTIFYICONDATAW data;
    memset(&data, 0, sizeof(data));
    data.cbSize = sizeof(data);
    data.hWnd = GetSafeHwnd();
    data.uID = kTrayIconId;

    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_WARNING;
    data.uTimeout = 3000;
    wcsncpy_s(data.szInfoTitle, L"网络打印服务器", _TRUNCATE);
    wcsncpy_s(data.szInfo, L"打印服务器已停止", _TRUNCATE);
    ::Shell_NotifyIconW(NIM_MODIFY, &data);

    data.uFlags = 0;
    ::Shell_NotifyIconW(NIM_DELETE, &data);
    m_trayIconVisible = false;
}

void CPrintServerDlg::ShowMainWindow()
{
    ShowWindow(SW_SHOW);
    ShowWindow(SW_RESTORE);
    SetForegroundWindow();
    ActivateTopParent();
}

// ===========================================================================
// 端口就地编辑
// ===========================================================================

void CPrintServerDlg::BeginPortEdit(int row)
{
    if (m_page != 1)    // 服务器页的共享列表为只读
        return;

    if (row < 0 || row >= m_printerList.GetItemCount())
        return;

    CRect rect;
    if (!m_printerList.GetSubItemRect(row, 2, LVIR_BOUNDS, rect))
        return;

    if (m_portEdit.GetSafeHwnd() != nullptr)
        CommitPortEdit(false);

    if (!m_portEdit.Create(WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL | WS_BORDER,
                           rect, &m_printerList, 1))
    {
        return;
    }

    Ui::ApplyFont(&m_portEdit, Ui::GetFont(kYahei, 9, false));
    m_portEdit.SetWindowTextW(GetListText(row, 2).c_str());
    m_portEdit.SetSel(0, -1);
    m_portEdit.SetFocus();

    m_editRow = row;
}

void CPrintServerDlg::CommitPortEdit(bool cancel)
{
    if (m_portEdit.GetSafeHwnd() == nullptr)
        return;

    // 端口编辑只在设备列表页进行，列表行号即数据集下标
    if (!cancel && m_editRow >= 0 && m_editRow < (int)m_printerEntries.size())
    {
        CString text;
        m_portEdit.GetWindowTextW(text);

        const std::string port = TrimAscii(WideToUtf8(std::wstring((LPCWSTR)text)));
        m_printerEntries[m_editRow].port = port;
        SetListText(m_editRow, 2, Utf8ToWide(port));
    }

    m_portEdit.DestroyWindow();
    m_editRow = -1;
}

// ===========================================================================
// 消息处理
// ===========================================================================

BOOL CPrintServerDlg::PreTranslateMessage(MSG* pMsg)
{
    if (m_portEdit.GetSafeHwnd() != nullptr && pMsg->hwnd == m_portEdit.GetSafeHwnd())
    {
        if (pMsg->message == WM_KEYDOWN && pMsg->wParam == VK_RETURN)
        {
            CommitPortEdit(false);
            return TRUE;
        }

        if (pMsg->message == WM_KEYDOWN && pMsg->wParam == VK_ESCAPE)
        {
            CommitPortEdit(true);
            return TRUE;
        }

        if (pMsg->message == WM_KILLFOCUS)
        {
            CommitPortEdit(false);
        }
    }

    return CDialogEx::PreTranslateMessage(pMsg);
}

void CPrintServerDlg::OnTimer(UINT_PTR nIDEvent)
{
    if (nIDEvent == kStatusTimerId)
    {
        UpdateServiceStatus();
        AutoRefreshServiceLog();
        return;
    }

    CDialogEx::OnTimer(nIDEvent);
}

void CPrintServerDlg::OnSize(UINT nType, int cx, int cy)
{
    CDialogEx::OnSize(nType, cx, cy);

    // 导航栏占满客户区左侧整条高度
    if (m_navBar.GetSafeHwnd() != nullptr && cx > 0 && cy > 0)
        m_navBar.SetWindowPos(nullptr, 0, 0, Ui::CNavBar::kWidth, cy,
                              SWP_NOZORDER | SWP_NOACTIVATE);

    // 对应 C# Resize：最小化且服务器运行中 → 隐藏到托盘
    if (nType == SIZE_MINIMIZED && m_isRunning)
        ShowWindow(SW_HIDE);
}

void CPrintServerDlg::OnClose()
{
    // 对应 C# PrintServerForm_FormClosing
    if (m_isReallyClosing)
    {
        EndDialog(IDOK);
        return;
    }

    if (m_isRunning)
    {
        // 服务器运行中 → 最小化到托盘，不退出
        ShowWindow(SW_HIDE);

        if (m_trayIconVisible)
        {
            NOTIFYICONDATAW data;
            memset(&data, 0, sizeof(data));
            data.cbSize = sizeof(data);
            data.hWnd = GetSafeHwnd();
            data.uID = kTrayIconId;
            data.uFlags = NIF_INFO;
            data.dwInfoFlags = NIIF_INFO;
            data.uTimeout = 2000;
            wcsncpy_s(data.szInfoTitle, L"网络打印服务器", _TRUNCATE);
            wcsncpy_s(data.szInfo, L"服务器仍在运行，已最小化到系统托盘", _TRUNCATE);
            ::Shell_NotifyIconW(NIM_MODIFY, &data);
        }
        return;
    }

    // 服务器未运行 → 确认退出
    int result = ::MessageBoxW(GetSafeHwnd(), L"确定要退出打印服务器吗？", L"确认退出",
                               MB_YESNO | MB_ICONQUESTION);
    if (result == IDNO)
        return;

    EndDialog(IDCANCEL);
}

LRESULT CPrintServerDlg::OnTrayCallback(WPARAM /*wParam*/, LPARAM lParam)
{
    switch (LOWORD(lParam))
    {
    case WM_RBUTTONUP:
    {
        CMenu menu;
        menu.CreatePopupMenu();
        menu.AppendMenuW(MF_STRING, kTrayMenuShow, L"显示控制面板");
        menu.AppendMenuW(MF_STRING, kTrayMenuStop, L"停止服务器");
        menu.AppendMenuW(MF_SEPARATOR);
        menu.AppendMenuW(MF_STRING, kTrayMenuExit, L"退出程序");

        // 对应 C# 的 mnuShow.Font = Bold
        menu.SetDefaultItem(0, TRUE);

        CPoint point;
        ::GetCursorPos(&point);

        SetForegroundWindow();
        UINT command = (UINT)::TrackPopupMenuEx(menu.GetSafeHmenu(),
                                                TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                                point.x, point.y, GetSafeHwnd(), nullptr);
        PostMessage(WM_NULL);

        if (command == kTrayMenuShow)
        {
            ShowMainWindow();
        }
        else if (command == kTrayMenuStop)
        {
            if (m_isRunning)
                StopServer();
        }
        else if (command == kTrayMenuExit)
        {
            if (m_isRunning)
            {
                int confirm = ::MessageBoxW(GetSafeHwnd(),
                    L"服务器正在运行，退出将停止所有服务。\n确定退出吗？",
                    L"确认退出", MB_YESNO | MB_ICONWARNING);
                if (confirm == IDNO)
                    return 0;

                StopServer();
            }

            m_isReallyClosing = true;

            // 对应 C#：_trayIcon.Visible = false; Dispose(); Application.Exit();
            if (m_trayIconVisible)
            {
                NOTIFYICONDATAW data;
                memset(&data, 0, sizeof(data));
                data.cbSize = sizeof(data);
                data.hWnd = GetSafeHwnd();
                data.uID = kTrayIconId;
                ::Shell_NotifyIconW(NIM_DELETE, &data);
                m_trayIconVisible = false;
            }

            EndDialog(IDOK);
        }
        break;
    }

    case WM_LBUTTONDBLCLK:
        ShowMainWindow();
        break;

    default:
        break;
    }

    return 0;
}

// ===========================================================================
// 按钮
// ===========================================================================

void CPrintServerDlg::OnStartStop()
{
    if (!m_isRunning)
        StartServer();
    else
        StopServer();
}

void CPrintServerDlg::OnInstallToggle()
{
    std::string result = RunCmd(L"sc", L"query PrintServer", false);

    if (result.find("1060") != std::string::npos)
        BtnInstallService();
    else
        BtnUninstallService();

    ::Sleep(500);
    UpdateServiceStatus();
}

void CPrintServerDlg::OnServiceStartToggle()
{
    std::string result = RunCmd(L"sc", L"query PrintServer", false);

    if (result.find("RUNNING") != std::string::npos)
        BtnStopService();
    else if (result.find("1060") == std::string::npos)
        BtnStartService();
    else
    {
        ::MessageBoxW(GetSafeHwnd(), L"服务未安装，请先安装服务！", L"提示",
                      MB_OK | MB_ICONWARNING);
        return;
    }

    ::Sleep(500);
    UpdateServiceStatus();
}

void CPrintServerDlg::OnClearLog()
{
    if (m_portEdit.GetSafeHwnd() != nullptr)
        CommitPortEdit(false);

    ::SetWindowTextW(::GetDlgItem(GetSafeHwnd(), IDC_PS_LOG), L"");
}

void CPrintServerDlg::OnRefreshPrinters()
{
    RefreshPrinterList();
}

void CPrintServerDlg::OnSelectAll()
{
    for (size_t i = 0; i < m_printerEntries.size(); ++i)
        m_printerEntries[i].shared = true;

    ReallocatePorts();
    RenderPrinterList();
}

void CPrintServerDlg::OnDeselectAll()
{
    for (size_t i = 0; i < m_printerEntries.size(); ++i)
        m_printerEntries[i].shared = false;

    ReallocatePorts();
    RenderPrinterList();
}

// 服务器页的共享列表为只读：拦截用户对勾选框的操作
// （RenderPrinterList 内部通过 m_suppressListEvents 标记的程序化设置不受影响）
void CPrintServerDlg::OnPrinterListChanging(NMHDR* pNMHDR, LRESULT* pResult)
{
    NMLISTVIEW* info = reinterpret_cast<NMLISTVIEW*>(pNMHDR);
    *pResult = 0;

    const bool stateChanged =
        (info->uOldState & LVIS_STATEIMAGEMASK) != (info->uNewState & LVIS_STATEIMAGEMASK);

    if (stateChanged && m_page != 1 && !m_suppressListEvents)
        *pResult = TRUE;    // 阻止勾选状态变更
}

void CPrintServerDlg::OnPrinterListChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
    NMLISTVIEW* info = reinterpret_cast<NMLISTVIEW*>(pNMHDR);
    *pResult = 0;

    if ((info->uOldState & LVIS_STATEIMAGEMASK) == (info->uNewState & LVIS_STATEIMAGEMASK))
        return;

    // 只有设备列表页可编辑；该页列表行号与数据集下标一一对应
    if (m_suppressListEvents || m_page != 1)
        return;

    const int index = info->iItem;
    if (index < 0 || index >= (int)m_printerEntries.size())
        return;

    m_printerEntries[index].shared = GetRowChecked(index);

    // 端口可能被重新分配：逐行把数据集同步回列表（不重建列表，避免滚动位置丢失）
    ReallocatePorts();

    const int rowCount = m_printerList.GetItemCount();
    for (int row = 0; row < rowCount && row < (int)m_printerEntries.size(); ++row)
        SetListText(row, 2, Utf8ToWide(m_printerEntries[row].port));

    if (rowCount > 0)
        m_printerList.RedrawItems(0, rowCount - 1);

    UpdatePrinterCountPill();
}

void CPrintServerDlg::OnPrinterListDblClk(NMHDR* pNMHDR, LRESULT* pResult)
{
    NMITEMACTIVATE* info = reinterpret_cast<NMITEMACTIVATE*>(pNMHDR);

    // 端口编辑只在可编辑的设备列表页进行（服务器页为只读展示）
    if (m_page == 1 && info->iItem >= 0 && info->iSubItem == 2)
        BeginPortEdit(info->iItem);

    *pResult = 0;
}

void CPrintServerDlg::OnPrinterListClick(NMHDR* pNMHDR, LRESULT* pResult)
{
    NMITEMACTIVATE* info = reinterpret_cast<NMITEMACTIVATE*>(pNMHDR);

    // 对应 DataGridView 的单元格编辑：点击端口列即进入编辑（仅设备列表页）
    if (m_page == 1 && info->iItem >= 0 && info->iSubItem == 2)
        BeginPortEdit(info->iItem);

    *pResult = 0;
}

// ===========================================================================
// 服务安装 / 卸载 / 启动 / 停止（对应 C# 的四个按钮处理函数）
// ===========================================================================

void CPrintServerDlg::BtnInstallService()
{
    if (GetCheckedPrinterConfigs().empty())
    {
        ::MessageBoxW(GetSafeHwnd(), L"请至少勾选一台打印机后再安装服务！", L"提示",
                      MB_OK | MB_ICONWARNING);
        return;
    }

    std::string checkResult = RunCmd(L"sc", L"query PrintServer", false);
    if (checkResult.find("1060") == std::string::npos)
    {
        LogToTextBox("服务已存在，无需重复安装");
        ::MessageBoxW(GetSafeHwnd(),
                      L"PrintServer 服务已存在，无需重复安装。\n如需重新安装请先卸载。",
                      L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    SaveSettings();

    std::wstring exePath = ProgramHost::GetExecutablePath();

    LogToTextBox("执行: sc create PrintServer");
    std::wstring createArgs = L"create PrintServer binPath= \"\\\"" + exePath +
                              L"\\\" /service\" start= auto DisplayName= \"网络打印服务器\"";
    std::string result = RunCmd(L"sc", createArgs, false);

    if (result.find("成功") != std::string::npos ||
        result.find("success") != std::string::npos)
    {
        LogToTextBox("返回: sc create PrintServer 成功");
        LogToTextBox("执行: 设置服务描述和故障恢复");

        RunCmd(L"sc", L"description PrintServer \"将本地打印机共享为网络打印机\"", false);
        RunCmd(L"sc", L"failure PrintServer reset= 86400 actions= restart/60000/restart/60000/restart/60000",
               false);

        LogToTextBox("执行: sc start PrintServer");
        ::Sleep(500);

        std::string startResult = RunCmd(L"sc", L"start PrintServer", false);
        if (startResult.find("START_PENDING") != std::string::npos ||
            startResult.find("RUNNING") != std::string::npos ||
            startResult.find("1056") != std::string::npos)
        {
            LogToTextBox("返回: sc start PrintServer 成功");
        }

        LogToTextBox("Windows服务安装并启动成功");

        // 设置托盘助手开机自启 + 立即启动
        ProgramHost::SetTrayAutoStart(true);
        ProgramHost::LaunchTrayHelper();
        LogToTextBox("托盘助手已启用并启动");

        UpdateServiceStatus();

        std::vector<PrinterConfig> configs = GetCheckedPrinterConfigs();
        std::wstring printerList;
        for (size_t i = 0; i < configs.size(); ++i)
        {
            printerList += Utf8ToWide(configs[i].name) + L" (端口" +
                           std::to_wstring(configs[i].port) + L")\n";
        }
        if (printerList.empty())
            printerList = L"未设置\n";

        std::wstring message = L"服务安装成功已启动！\n\n"
                               L"配置详情\n"
                               L"服务名称: PrintServer\n"
                               L"启动方式: 自动\n"
                               L"显示名称: 网络打印服务器\n"
                               L"打印机:\n" + printerList +
                               L"API端口: " + Utf8ToWide(GetApiPortText());

        ::MessageBoxW(GetSafeHwnd(), message.c_str(), L"成功", MB_OK | MB_ICONINFORMATION);
    }
    else
    {
        LogToTextBox("安装失败");
        ::MessageBoxW(GetSafeHwnd(),
                      (L"安装失败，请以管理员身份运行！\n\n" + Utf8ToWide(result)).c_str(),
                      L"错误", MB_OK | MB_ICONERROR);
    }
}

void CPrintServerDlg::BtnUninstallService()
{
    // 托盘助手长期持有 PrintServer 的服务句柄，而 SCM 只有在「服务已停止 + 所有服务句柄
    // 都已关闭」之后才会把已标记删除的服务真正移除；不先让它退出，sc delete 只能把服务
    // 标记为“待删除”，再次删除就会返回 1072（指定的服务已标记为删除）→ 看起来永远卸载失败。
    LogToTextBox("执行: 关闭托盘助手");
    if (ProgramHost::RequestTrayHelperExit(3000))
        LogToTextBox("返回: 托盘助手已退出");
    else
        LogToTextBox("返回: 未检测到托盘助手");

    LogToTextBox("执行: sc stop PrintServer");
    std::string stopResult = RunCmd(L"sc", L"stop PrintServer", false);

    if (stopResult.find("STOP_PENDING") != std::string::npos ||
        stopResult.find("STOPPED") != std::string::npos ||
        stopResult.find("1062") != std::string::npos)
    {
        LogToTextBox("返回: sc stop PrintServer 成功");
    }

    // sc stop 只是下达停止请求（返回时通常处于 STOP_PENDING），必须等它真正停下，
    // 否则删除同样会被 SCM 挂起
    if (ProgramHost::WaitForServiceStopped(15000))
        LogToTextBox("Windows服务停止成功");
    else
        LogToTextBox("警告: 等待服务停止超时，继续卸载");

    // 移除托盘助手开机自启
    ProgramHost::SetTrayAutoStart(false);
    LogToTextBox("托盘助手已从开机自启移除");

    LogToTextBox("执行: sc delete PrintServer");
    std::string result = RunCmd(L"sc", L"delete PrintServer", false);

    // 1060 = 服务本就不存在；其余情况只要不是明确的失败，都用 SCM 查询确认最终结果
    if (result.find("1060") != std::string::npos)
    {
        LogToTextBox("服务未安装");
        ::MessageBoxW(GetSafeHwnd(), L"服务未安装", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // 1072 = 之前已被标记为删除，这里同样按「已下达删除」处理并轮询确认
    const bool deleteIssued = result.find("成功") != std::string::npos ||
                              result.find("SUCCESS") != std::string::npos ||
                              result.find("1072") != std::string::npos;

    if (deleteIssued && ProgramHost::WaitForServiceRemoved(8000))
    {
        LogToTextBox("返回: sc delete PrintServer 成功");
        UpdateServiceStatus();
        ::MessageBoxW(GetSafeHwnd(), L"服务卸载成功！", L"成功", MB_OK | MB_ICONINFORMATION);
    }
    else if (deleteIssued)
    {
        LogToTextBox("服务已标记为删除，但尚未从服务列表移除");
        ::MessageBoxW(GetSafeHwnd(),
                      L"服务已停止并标记为删除，但尚未从服务列表中移除。\n\n"
                      L"如有其它进程（如托盘监控）仍在使用该服务，关闭后会立即生效；\n"
                      L"也可以重启计算机后确认。",
                      L"提示", MB_OK | MB_ICONINFORMATION);
    }
    else
    {
        LogToTextBox("卸载结果: " + result);
        ::MessageBoxW(GetSafeHwnd(), (L"卸载结果: " + Utf8ToWide(result)).c_str(), L"提示",
                      MB_OK | MB_ICONINFORMATION);
    }
}

void CPrintServerDlg::BtnStartService()
{
    if (GetCheckedPrinterConfigs().empty())
    {
        ::MessageBoxW(GetSafeHwnd(), L"请至少勾选一台打印机后再启动服务！", L"提示",
                      MB_OK | MB_ICONWARNING);
        return;
    }

    // 检查服务是否已在运行
    std::string checkResult = RunCmd(L"sc", L"query PrintServer", false);
    if (checkResult.find("RUNNING") != std::string::npos)
    {
        LogToTextBox("服务已在运行中，无需重复启动");
        ::MessageBoxW(GetSafeHwnd(), L"服务已在运行中！", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    SaveSettings();

    LogToTextBox("执行: sc start PrintServer");
    std::string result = RunCmd(L"sc", L"start PrintServer", false);

    if (result.find("START_PENDING") != std::string::npos ||
        result.find("RUNNING") != std::string::npos ||
        result.find("1056") != std::string::npos)
    {
        LogToTextBox("返回: sc start PrintServer 成功");
        LogToTextBox("Windows服务启动成功");

        // 确保托盘助手正在运行
        ProgramHost::LaunchTrayHelper();

        UpdateServiceStatus();

        ::MessageBoxW(GetSafeHwnd(),
                      L"服务状态：启动成功\n\n"
                      L"服务名称：PrintServer\n"
                      L"启动方式：自动\n"
                      L"显示名称：网络打印服务器",
                      L"成功", MB_OK | MB_ICONINFORMATION);
    }
    else if (result.find("1060") != std::string::npos)
    {
        LogToTextBox("服务未安装，请先安装服务");
        ::MessageBoxW(GetSafeHwnd(), L"服务未安装，请先点击\"安装服务\"！", L"错误",
                      MB_OK | MB_ICONERROR);
    }
    else
    {
        LogToTextBox("启动结果: " + result);
        ::MessageBoxW(GetSafeHwnd(), (L"启动结果: " + Utf8ToWide(result)).c_str(), L"提示",
                      MB_OK | MB_ICONINFORMATION);
    }
}

void CPrintServerDlg::BtnStopService()
{
    LogToTextBox("执行: sc stop PrintServer");
    std::string result = RunCmd(L"sc", L"stop PrintServer", false);

    if (result.find("STOP_PENDING") != std::string::npos ||
        result.find("STOPPED") != std::string::npos)
    {
        LogToTextBox("返回: sc stop PrintServer 成功");
        LogToTextBox("Windows服务停止成功");
        UpdateServiceStatus();

        ::MessageBoxW(GetSafeHwnd(),
                      L"服务状态：停止成功\n\n"
                      L"服务名称：PrintServer\n"
                      L"启动方式：自动\n"
                      L"显示名称：网络打印服务器",
                      L"成功", MB_OK | MB_ICONINFORMATION);
    }
    else if (result.find("1060") != std::string::npos)
    {
        LogToTextBox("服务未安装");
        ::MessageBoxW(GetSafeHwnd(), L"服务未安装！", L"错误", MB_OK | MB_ICONERROR);
    }
    else if (result.find("1062") != std::string::npos)
    {
        LogToTextBox("服务未在运行");
        ::MessageBoxW(GetSafeHwnd(), L"服务未在运行中！", L"提示", MB_OK | MB_ICONINFORMATION);
    }
    else
    {
        LogToTextBox("停止结果: " + result);
        ::MessageBoxW(GetSafeHwnd(), (L"停止结果: " + Utf8ToWide(result)).c_str(), L"提示",
                      MB_OK | MB_ICONINFORMATION);
    }
}

LRESULT CPrintServerDlg::OnGuiLog(WPARAM /*wParam*/, LPARAM lParam)
{
    // 对应 C# Server_OnLog
    std::string* payload = reinterpret_cast<std::string*>(lParam);
    if (payload == nullptr)
        return 0;

    AppendLogLine(*payload);
    WriteLogToFile(*payload);
    m_lastLogFileContent = Log::ReadTodayLog();

    // 超过 1000 行时只保留最后 500 行
    HWND log = ::GetDlgItem(GetSafeHwnd(), IDC_PS_LOG);
    if (log != nullptr)
    {
        int length = ::GetWindowTextLengthW(log);
        int lineCount = (int)::SendMessageW(log, EM_GETLINECOUNT, 0, 0);

        if (lineCount > 1000 && length > 0)
        {
            std::wstring text((size_t)length + 1, L'\0');
            ::GetWindowTextW(log, &text[0], length + 1);
            text.resize((size_t)length);

            std::vector<std::wstring> lines;
            size_t start = 0;
            while (start <= text.size())
            {
                size_t end = text.find(L"\r\n", start);
                if (end == std::wstring::npos)
                {
                    lines.push_back(text.substr(start));
                    break;
                }

                lines.push_back(text.substr(start, end - start));
                start = end + 2;
            }

            std::wstring trimmed;
            size_t from = (lines.size() > 500) ? (lines.size() - 500) : 0;
            for (size_t i = from; i < lines.size(); ++i)
            {
                trimmed += lines[i];
                trimmed += L"\r\n";
            }

            ::SetWindowTextW(log, trimmed.c_str());
        }
    }

    delete payload;
    return 0;
}

} // namespace ps

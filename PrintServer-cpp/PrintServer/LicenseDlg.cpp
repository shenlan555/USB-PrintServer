#include "pch.h"
#include "LicenseDlg.h"

#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "BuildConfig.h"
#include "LogHelper.h"
#include "MachineCode.h"
#include "Program.h"
#include "UiTheme.h"
#include "Wmi.h"

namespace ps
{

namespace
{

const UINT kAsyncDoneMessage = WM_APP + 10;

// 对应 C# LicenseForm 的 Size（WinForms 的 Size 即外框尺寸）
const int kFormWidth = 560;
const int kFormHeight = 510;

// 对应 WinForms 的颜色常量（统一到界面设计令牌）
const COLORREF kColorWhite = RGB(255, 255, 255);
const COLORREF kColorBlack = RGB(0, 0, 0);
const COLORREF kColorGray = Ui::Design::kTextMuted;
const COLORREF kColorRed = Ui::Design::kDanger;
const COLORREF kColorGreen = Ui::Design::kSuccess;
const COLORREF kColorOrange = Ui::Design::kWarning;
const COLORREF kColorTitle = Ui::Design::kTextTitle;
const COLORREF kColorAccent = Ui::Design::kAccent;
const COLORREF kColorValue = Ui::Design::kTextTitle;
const COLORREF kPageBack = Ui::Design::kPageBack;
const COLORREF kPanelBack = Ui::Design::kCardBack;
const COLORREF kPanelBorder = Ui::Design::kCardBorder;

// 授权窗口布局（客户区 544×470，严格对齐原型 mockup.html 授权窗口）
const int kHeaderHeight = 60;         // 头部：顶部 3px 主色条 + 浅蓝白底 + 1px 分隔线
const int kBodyPadX = 20;             // 内容左右留白（原型 .lic-body padding:0 20px）
const int kBodyRight = 544 - kBodyPadX;         // 524
const int kBodyInner = kBodyRight - kBodyPadX;  // 504
const int kKvCardTop = 72;            // 60 + 12（原型 .kvcard margin-top:12px）
const int kKvCardHeight = 66;
const int kSegTop = 150;              // 72 + 66 + 12
const int kStep1Top = 194;            // 150 + 32 + 12
const int kStepHeight = 96;
const int kStep2Top = 306;            // 194 + 96 + 16
const int kStepPadX = 16;
const int kStepPadY = 15;
const int kStepHeadHeight = 24;
const int kStepRowGap = 6;
const int kOffcardTop = 194;
const int kOffcardHeight = 138;
const int kFootHeight = 56;           // 原型 .lic-foot height:56px（含底部 20px 留白）
const int kBtnHeightMain = 36;        // 原型 .btn height:36px
const int kBtnHeightMini = 28;        // 原型 .btn-sm height:28px
const int kInputHeight = 34;          // 原型 .input height:34px
const int kButtonPadX = 16;           // 原型 .btn padding:0 16px
const int kFieldTextPad = 10;         // 输入框内文字左内边距

const wchar_t* kYahei = L"微软雅黑";

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

} // namespace

IMPLEMENT_DYNAMIC(CLicenseDlg, CDialogEx)

BEGIN_MESSAGE_MAP(CLicenseDlg, CDialogEx)
    ON_WM_PAINT()
    ON_WM_DRAWITEM()
    ON_WM_CTLCOLOR()
    ON_WM_ERASEBKGND()
    ON_WM_GETMINMAXINFO()
    ON_WM_CLOSE()
    ON_MESSAGE(kAsyncDoneMessage, &CLicenseDlg::OnAsyncDone)
    ON_BN_CLICKED(IDC_LIC_COPY_MC, &CLicenseDlg::OnCopyMachineCode)
    ON_BN_CLICKED(IDC_LIC_BTN_ACTIVATE_ONLINE, &CLicenseDlg::OnActivateOnline)
    ON_BN_CLICKED(IDC_LIC_BTN_SELECT_LIC, &CLicenseDlg::OnSelectLicFile)
    ON_BN_CLICKED(IDC_LIC_BTN_ACTIVATE_OFFLINE, &CLicenseDlg::OnActivateOffline)
    ON_BN_CLICKED(IDC_LIC_BTN_REPORT, &CLicenseDlg::OnReportMachine)
    ON_BN_CLICKED(IDC_LIC_BTN_CHECK, &CLicenseDlg::OnCheckLicense)
    ON_MESSAGE(Ui::kSegChanged, &CLicenseDlg::OnSegChanged)
END_MESSAGE_MAP()

CLicenseDlg::CLicenseDlg(CWnd* pParent)
    : CDialogEx(IDD_LICENSE_DIALOG, pParent)
    , m_isServerReachable(false)
    , m_offlineTabDisabled(false)
    , m_reachabilityTaskRunning(false)
{
}

CLicenseDlg::~CLicenseDlg()
{
}

void CLicenseDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
}

BOOL CLicenseDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    // 对应 C#：BackColor = Color.White
    m_whiteBrush.CreateSolidBrush(kColorWhite);
    m_faceBrush.CreateSolidBrush(::GetSysColor(COLOR_BTNFACE));

    // 对应 InitializeComponent 中的 appicon
    HICON icon = (HICON)::LoadImageW(::AfxGetInstanceHandle(),
                                     MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 0, 0,
                                     LR_DEFAULTSIZE | LR_SHARED);
    if (icon != nullptr)
    {
        SetIcon(icon, TRUE);
        SetIcon(icon, FALSE);
    }

    // 对应 FormBorderStyle.Sizable + MinimumSize/MaximumSize 同值 → 实际不可缩放
    SetWindowPos(nullptr, 0, 0, kFormWidth, kFormHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);

    // 卡片/输入框底板是沉底的背景窗口，父窗口必须裁剪子窗口区域，
    // 否则父窗口重绘会盖住子窗口且子窗口不会再收到 WM_PAINT（卡片整块消失）
    ModifyStyle(0, WS_CLIPCHILDREN);

    BuildControls();

    LoadMachineCode();

    // 对应 C# Load 事件中的尺寸调整
    Layout();

    // 对应 CenterToScreen()：在鼠标所在显示器的工作区内居中
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

    // 对应 C#：TopMost = true
    SetWindowPos(&CWnd::wndTopMost, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    StartServerReachabilityCheck();

    return TRUE;
}

void CLicenseDlg::BuildControls()
{
    HWND self = GetSafeHwnd();

    // 只清理本对话框的表面登记（主窗口可能同时在后台，登记互不干扰）
    Ui::ClearSurfaces(self);

    // 页脚与 QQ 群行贴合客户区底部（客户区高度随边框/DPI 略有出入）
    CRect clientRect;
    GetClientRect(&clientRect);
    const int footTop = clientRect.Height() - kFootHeight;
    m_footStatusY = footTop + 1;
    m_footHintY = footTop + 19;
    m_footButtonY = footTop;
    m_qqTop = clientRect.Height() - 18;

    // 字号：11px 辅助文字 / 12px 标题与按钮（600）/ 13px 步骤标题（700）/ 等宽值
    HFONT font11 = Ui::GetFont(kYahei, 8, false);
    HFONT font12 = Ui::GetFont(kYahei, 8, true);
    HFONT font13 = Ui::GetFont(kYahei, 10, true);
    HFONT fontMono = Ui::GetFont(L"Consolas", 9, true);
    HFONT fontKey = Ui::GetFont(L"Consolas", 14, true);

    // === 本机信息卡（原型 .kvcard 66 高：主机名 / 机器码 + 复制） ===
    // 卡片底色与描边由对话框 OnPaint 绘制（卡片区域内的控件背景由表面登记取色）
    const int kvKeyX = kBodyPadX + 16;
    const int kvValX = kvKeyX + 56 + 10;                 // 原型 .kvgrid 56px + 10px 列间距
    const int kvRow1Y = kKvCardTop + 12;
    const int kvRow2Y = kvRow1Y + 21;

    HWND hostnameLabel = CreateChildWnd(self, L"Static", L"主机名", SS_LEFT, 0,
                                        kvKeyX, kvRow1Y, 56, 18,
                                        IDC_LIC_HOSTNAME_LABEL, font11);
    Ui::SetTextColor(hostnameLabel, kColorGray);

    wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
    DWORD computerNameLen = _countof(computerName);
    if (!::GetComputerNameW(computerName, &computerNameLen))
        computerName[0] = L'\0';

    HWND hostnameValue = CreateChildWnd(self, L"Static", computerName, SS_LEFT, 0,
                                        kvValX, kvRow1Y, 300, 18,
                                        IDC_LIC_HOSTNAME_VALUE, fontMono);
    Ui::SetTextColor(hostnameValue, kColorValue);

    HWND mcLabel = CreateChildWnd(self, L"Static", L"机器码", SS_LEFT, 0,
                                  kvKeyX, kvRow2Y, 56, 18,
                                  IDC_LIC_MC_LABEL, font11);
    Ui::SetTextColor(mcLabel, kColorGray);

    HWND machineCode = CreateChildWnd(self, L"Static", L"", SS_LEFT | SS_ENDELLIPSIS, 0,
                                      kvValX, kvRow2Y, 320, 18,
                                      IDC_LIC_MACHINE_CODE, fontMono);
    Ui::SetTextColor(machineCode, kColorValue);

    // 复制按钮（次级描边，28 高，右内边距 16）
    const int copyWidth = Ui::MeasureTextWidth(self, font12, L"复制") + kButtonPadX * 2;
    CreateChildWnd(self, L"Button", L"复制", BS_OWNERDRAW, 0,
                   kBodyRight - 16 - copyWidth,
                   kKvCardTop + (kKvCardHeight - kBtnHeightMini) / 2,
                   copyWidth, kBtnHeightMini, IDC_LIC_COPY_MC, font12);
    Ui::MakeOutlineButton(this, IDC_LIC_COPY_MC, kColorGray, Ui::Gfx::Icon::None,
                          Ui::Design::kCardBack, Ui::Design::kFieldBorder);

    // === 分段控件（原型 .seg：浅蓝分组 + 选中项白底主色描边） ===
    std::vector<std::wstring> segItems;
    segItems.push_back(L"在线激活");
    segItems.push_back(L"离线激活");

    int segWidth = Ui::Design::kSegPad * 2 + Ui::Design::kSegGap;
    for (size_t i = 0; i < segItems.size(); ++i)
        segWidth += Ui::MeasureTextWidth(self, font12, segItems[i].c_str()) + kButtonPadX * 2;

    CreateChildWnd(self, L"Static", L"", SS_OWNERDRAW, 0,
                   kBodyPadX, kSegTop, segWidth, Ui::Design::kSegHeight,
                   IDC_LIC_SEG, font12);
    Ui::MakeSeg(this, IDC_LIC_SEG, segItems, 0, Ui::Design::kPageBack);

    // === 步骤卡片 1：获取本机机器码 ===
    const int stepHeadY = kStep1Top + kStepPadY;
    const int stepRowY = stepHeadY + kStepHeadHeight + kStepRowGap;
    const int stepInnerX = kBodyPadX + kStepPadX;
    const int stepTitleX = stepInnerX + kStepHeadHeight + 10;

    m_onlineControls.push_back(CreateChildWnd(self, L"Static", L"1", SS_OWNERDRAW, 0,
        stepInnerX, stepHeadY, kStepHeadHeight, kStepHeadHeight,
        IDC_LIC_ONLINE_BADGE1, font12));
    Ui::MakeBadge(this, IDC_LIC_ONLINE_BADGE1, Ui::Gfx::Icon::None, Ui::Design::kCardBack,
                  kColorAccent, kStepHeadHeight / 2);

    m_onlineControls.push_back(CreateChildWnd(self, L"Static", L"获取本机机器码", SS_LEFT, 0,
        stepTitleX, stepHeadY, 200, kStepHeadHeight, IDC_LIC_ONLINE_HINT, font13));
    Ui::SetTextColor(::GetDlgItem(self, IDC_LIC_ONLINE_HINT), kColorTitle);

    const int step1TitleWidth = Ui::MeasureTextWidth(self, font13, L"获取本机机器码");
    m_onlineControls.push_back(CreateChildWnd(self, L"Static",
        L"上报后由管理员在后台完成授权", SS_LEFT, 0,
        stepTitleX + step1TitleWidth + 10, stepHeadY, 240, kStepHeadHeight,
        IDC_LIC_ONLINE_DESC, font11));
    Ui::SetTextColor(::GetDlgItem(self, IDC_LIC_ONLINE_DESC), kColorGray);

    // 输入行：只放主操作按钮（机器码已在上方 kvcard 中显示，此处不再重复）
    const int reportWidth = Ui::MeasureTextWidth(self, font12, L"上报机器码") + kButtonPadX * 2;

    m_onlineControls.push_back(CreateChildWnd(self, L"Button", L"上报机器码", BS_OWNERDRAW, 0,
        stepInnerX, stepRowY, reportWidth, kBtnHeightMain, IDC_LIC_BTN_REPORT, font12));
    Ui::MakeFlatButton(this, IDC_LIC_BTN_REPORT, kColorAccent, kColorWhite);

    // === 步骤卡片 2：填入授权码 ===
    const int step2HeadY = kStep2Top + kStepPadY;
    const int step2RowY = step2HeadY + kStepHeadHeight + kStepRowGap;

    m_onlineControls.push_back(CreateChildWnd(self, L"Static", L"2", SS_OWNERDRAW, 0,
        stepInnerX, step2HeadY, kStepHeadHeight, kStepHeadHeight,
        IDC_LIC_ONLINE_BADGE2, font12));
    Ui::MakeBadge(this, IDC_LIC_ONLINE_BADGE2, Ui::Gfx::Icon::None, Ui::Design::kCardBack,
                  kColorAccent, kStepHeadHeight / 2);

    m_onlineControls.push_back(CreateChildWnd(self, L"Static", L"填入授权码", SS_LEFT, 0,
        stepTitleX, step2HeadY, 200, kStepHeadHeight, IDC_LIC_ONLINE_HINT2, font13));
    Ui::SetTextColor(::GetDlgItem(self, IDC_LIC_ONLINE_HINT2), kColorTitle);

    const int step2TitleWidth = Ui::MeasureTextWidth(self, font13, L"填入授权码");
    m_onlineControls.push_back(CreateChildWnd(self, L"Static",
        L"把管理员提供的授权码粘贴到下方，点击激活", SS_LEFT, 0,
        stepTitleX + step2TitleWidth + 10, step2HeadY, 240, kStepHeadHeight,
        IDC_LIC_ONLINE_DESC2, font11));
    Ui::SetTextColor(::GetDlgItem(self, IDC_LIC_ONLINE_DESC2), kColorGray);

    const int activateWidth =
        Ui::MeasureTextWidth(self, font12, L"激  活") + kButtonPadX * 2;
    const int keyFieldRight = kBodyRight - kStepPadX - activateWidth - 12;

    m_keyFieldRect.left = stepInnerX;
    m_keyFieldRect.top = step2RowY + 1;
    m_keyFieldRect.right = keyFieldRight;
    m_keyFieldRect.bottom = step2RowY + 1 + kInputHeight;

    m_onlineControls.push_back(CreateChildWnd(self, L"Static", L"", SS_OWNERDRAW, 0,
        stepInnerX, step2RowY + 1, keyFieldRight - stepInnerX, kInputHeight,
        IDC_LIC_KEY_FIELD, nullptr));
    Ui::MakeField(this, IDC_LIC_KEY_FIELD, Ui::Design::kCardBack);

    // C# 侧 TextBox 虽指定 Height=32，但 WinForms 单行文本框会按字体行高自动收缩
    // （Consolas 14pt Bold → 渲染后 29），此处按实际渲染高度创建以保持界面一致
    m_onlineControls.push_back(CreateChildWnd(self, L"Edit", L"",
        ES_AUTOHSCROLL | ES_LEFT, 0,
        stepInnerX + 5, step2RowY + 3, keyFieldRight - stepInnerX - 10, 29,
        IDC_LIC_KEY_EDIT, fontKey));
    if (m_onlineControls.back() != nullptr)
        ::SendMessageW(m_onlineControls.back(), EM_SETLIMITTEXT, 64, 0);

    // 输入框底板显式排在授权码 Edit 之后（= 其下方），保证授权码框可点击输入
    ::SetWindowPos(::GetDlgItem(self, IDC_LIC_KEY_FIELD), m_onlineControls.back(),
                   0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    m_onlineControls.push_back(CreateChildWnd(self, L"Button", L"激  活", BS_OWNERDRAW, 0,
        kBodyRight - kStepPadX - activateWidth, step2RowY, activateWidth, kBtnHeightMain,
        IDC_LIC_BTN_ACTIVATE_ONLINE, font12));
    Ui::MakeFlatButton(this, IDC_LIC_BTN_ACTIVATE_ONLINE, kColorAccent, kColorWhite);

    // === 离线激活卡（原型 .offcard：圆徽标 + 标题 + 说明 + 按钮行 + 状态行） ===
    const int offHeadY = kOffcardTop + 16;
    const int offInnerX = kBodyPadX + 16;
    const int offTitleX = offInnerX + kStepHeadHeight + 10;

    m_offlineControls.push_back(CreateChildWnd(self, L"Static", L"钥", SS_OWNERDRAW, 0,
        offInnerX, offHeadY, kStepHeadHeight, kStepHeadHeight, IDC_LIC_OFFLINE_BADGE, font12));
    Ui::MakeBadge(this, IDC_LIC_OFFLINE_BADGE, Ui::Gfx::Icon::None, Ui::Design::kCardBack,
                  Ui::Design::kInfo, kStepHeadHeight / 2);

    m_offlineControls.push_back(CreateChildWnd(self, L"Static", L"导入 .lic 授权文件", SS_LEFT, 0,
        offTitleX, offHeadY, 220, kStepHeadHeight, IDC_LIC_OFFLINE_HINT, font13));
    Ui::SetTextColor(::GetDlgItem(self, IDC_LIC_OFFLINE_HINT), kColorTitle);

    const int offTitleWidth = Ui::MeasureTextWidth(self, font13, L"导入 .lic 授权文件");
    m_offlineControls.push_back(CreateChildWnd(self, L"Static",
        L"由管理员根据本机机器码生成，仅对本机有效", SS_LEFT, 0,
        offTitleX + offTitleWidth + 10, offHeadY, 240, kStepHeadHeight,
        IDC_LIC_OFFLINE_DESC, font11));
    Ui::SetTextColor(::GetDlgItem(self, IDC_LIC_OFFLINE_DESC), kColorGray);

    const int offRowY = offHeadY + kStepHeadHeight + 12;
    const int selectWidth =
        Ui::MeasureTextWidth(self, font12, L"选择 .lic 文件") + kButtonPadX * 2;
    const int importWidth = Ui::MeasureTextWidth(self, font12, L"导入激活") + kButtonPadX * 2;

    m_offlineControls.push_back(CreateChildWnd(self, L"Button", L"选择 .lic 文件", BS_OWNERDRAW, 0,
        offInnerX, offRowY, selectWidth, kBtnHeightMain, IDC_LIC_BTN_SELECT_LIC, font12));
    Ui::MakeFlatButton(this, IDC_LIC_BTN_SELECT_LIC, kColorAccent, kColorWhite);

    m_offlineControls.push_back(CreateChildWnd(self, L"Button", L"导入激活", BS_OWNERDRAW, 0,
        offInnerX + selectWidth + 12, offRowY, importWidth, kBtnHeightMain,
        IDC_LIC_BTN_ACTIVATE_OFFLINE, font12));
    Ui::MakeOutlineButton(this, IDC_LIC_BTN_ACTIVATE_OFFLINE, kColorGray, Ui::Gfx::Icon::None,
                          Ui::Design::kCardBack, Ui::Design::kFieldBorder);

    // 状态行：语义胶囊（宽度随文字自适应）+ 辅助说明
    const int offStatusY = offRowY + kBtnHeightMain + 12;
    m_offlineControls.push_back(CreateChildWnd(self, L"Static", L"尚未选择授权文件", SS_LEFT, 0,
        offInnerX, offStatusY, 140, Ui::Design::kPillHeight, IDC_LIC_OFFLINE_STATUS, font12));
    Ui::SetTextColor(::GetDlgItem(self, IDC_LIC_OFFLINE_STATUS), kColorGray);
    Ui::MakePill(this, IDC_LIC_OFFLINE_STATUS);

    m_offlineControls.push_back(CreateChildWnd(self, L"Static",
        L"选择后点击「导入激活」完成授权", SS_LEFT, 0,
        offInnerX + 148, offStatusY, 300, Ui::Design::kPillHeight,
        IDC_LIC_OFFLINE_HINT2, font11));
    Ui::SetTextColor(::GetDlgItem(self, IDC_LIC_OFFLINE_HINT2), kColorGray);

    // === 页脚（原型 .lic-foot：状态提示 + 检查授权） ===
    m_onlineControls.push_back(CreateChildWnd(self, L"Static", L"", SS_LEFT, 0,
        kBodyPadX, m_footStatusY, 404, 18, IDC_LIC_ONLINE_STATUS, font11));
    Ui::SetTextColor(::GetDlgItem(self, IDC_LIC_ONLINE_STATUS), kColorGray);

    HWND footHint = CreateChildWnd(self, L"Static",
        L"授权与本机硬件绑定，更换硬件需重新激活", SS_LEFT, 0,
        kBodyPadX, m_footHintY, 404, 18, IDC_LIC_FOOT_HINT, font11);
    Ui::SetTextColor(footHint, kColorGray);

    const int checkWidth = Ui::MeasureTextWidth(self, font12, L"检查授权") + kButtonPadX * 2;
    CreateChildWnd(self, L"Button", L"检查授权", BS_OWNERDRAW, 0,
                   kBodyRight - checkWidth, m_footButtonY, checkWidth, kBtnHeightMain,
                   IDC_LIC_BTN_CHECK, font12);
    // 页脚直接落在对话框页面底色上，圆角外四角必须铺页面底色（否则会露出白方角）
    Ui::MakeOutlineButton(this, IDC_LIC_BTN_CHECK, kColorGray, Ui::Gfx::Icon::None,
                          Ui::Design::kCardBack, Ui::Design::kFieldBorder,
                          Ui::Design::kPageBack);

    // QQ 群（编译期固定，对应 AssemblyQQGroup 特性；放在页脚下方留白里）
    std::wstring qqText = L"官方交流群：" + Utf8ToWide(BuildConfig::QQGroup);
    HWND qq = CreateChildWnd(self, L"Static", qqText.c_str(), SS_CENTER, 0,
                             kBodyPadX, m_qqTop, kBodyInner, 18, IDC_LIC_QQ, font11);
    Ui::SetTextColor(qq, kColorGray);

    // 离线导入不可用（外网可达时）由 OnAsyncDone 关闭按钮，故按钮初始状态与 C# 一致为禁用
    ::EnableWindow(::GetDlgItem(self, IDC_LIC_BTN_ACTIVATE_OFFLINE), FALSE);

    RegisterLicenseSurfaces();
    SelectTabPage(0);
}

// 卡片（本机信息卡 / 步骤卡 / 离线卡）在对话框 DC 上绘制：这些卡片是最底层的装饰层，
// 用子窗口实现反而会因父窗口重绘而丢失，直接画在父窗口 DC 上最稳。
// 卡内控件的背景刷按「落在哪张卡片上」取色，故卡片与输入框底板仍需登记表面
void CLicenseDlg::RegisterLicenseSurfaces()
{
    Ui::ClearSurfaces(GetSafeHwnd());

    const RECT cards[] = { CardRect(0), CardRect(1), CardRect(2), CardRect(3) };
    const RECT fields[] = { m_keyFieldRect };
    const COLORREF fieldColors[] = { Ui::Design::kFieldBack };

    for (size_t i = 0; i < _countof(cards); ++i)
    {
        if (i == 1 || i == 2)
        {
            if (m_page != 0)
                continue;
        }
        else if (i == 3 && m_page != 1)
        {
            continue;
        }

        Ui::RegisterSurface(cards[i], Ui::Design::kCardBack, GetSafeHwnd());
    }

    if (m_page == 0)
    {
        for (size_t i = 0; i < _countof(fields); ++i)
            Ui::RegisterSurface(fields[i], fieldColors[i], GetSafeHwnd());
    }
}

// index: 0 本机信息卡 / 1 步骤 1 / 2 步骤 2 / 3 离线卡
RECT CLicenseDlg::CardRect(int index) const
{
    RECT rc = { kBodyPadX, kKvCardTop, kBodyPadX + kBodyInner, kKvCardTop + kKvCardHeight };

    switch (index)
    {
    case 1:
        rc.top = kStep1Top;
        rc.bottom = kStep1Top + kStepHeight;
        break;
    case 2:
        rc.top = kStep2Top;
        rc.bottom = kStep2Top + kStepHeight;
        break;
    case 3:
        rc.top = kOffcardTop;
        rc.bottom = kOffcardTop + kOffcardHeight;
        break;
    default:
        break;
    }

    return rc;
}

void CLicenseDlg::Layout()
{
    // 对话框尺寸固定（对应 C# MinimumSize = MaximumSize），仅页脚与 QQ 群行贴合客户区底部，
    // 避免不同系统边框厚度下底部留白被裁掉
    CRect client;
    GetClientRect(&client);

    const int footTop = client.Height() - kFootHeight;

    HWND controls[] = { ::GetDlgItem(GetSafeHwnd(), IDC_LIC_ONLINE_STATUS),
                        ::GetDlgItem(GetSafeHwnd(), IDC_LIC_FOOT_HINT),
                        ::GetDlgItem(GetSafeHwnd(), IDC_LIC_BTN_CHECK),
                        ::GetDlgItem(GetSafeHwnd(), IDC_LIC_QQ) };
    const int tops[] = { footTop + 1, footTop + 19, footTop, client.Height() - 18 };

    for (size_t i = 0; i < _countof(controls); ++i)
    {
        if (controls[i] == nullptr)
            continue;

        CRect rc;
        ::GetWindowRect(controls[i], &rc);
        ::MapWindowPoints(HWND_DESKTOP, GetSafeHwnd(), (LPPOINT)&rc, 2);

        ::SetWindowPos(controls[i], nullptr, rc.left, tops[i], rc.Width(), rc.Height(),
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void CLicenseDlg::LoadMachineCode()
{
    // 对应 C# LoadMachineCode()：异常时显示"获取失败"
    try
    {
        m_cachedMachineCode = MachineCode::GenerateMachineCode();

        if (m_cachedMachineCode.empty())
            throw std::runtime_error("machine code unavailable");

        Ui::SetLabelText(this, IDC_LIC_MACHINE_CODE, Utf8ToWide(m_cachedMachineCode).c_str());
    }
    catch (...)
    {
        m_cachedMachineCode = "获取失败";
        Ui::SetLabelText(this, IDC_LIC_MACHINE_CODE, L"获取失败");
    }
}

bool CLicenseDlg::IsPageControl(HWND control) const
{
    for (size_t i = 0; i < m_onlineControls.size(); ++i)
    {
        if (m_onlineControls[i] == control)
            return true;
    }

    for (size_t i = 0; i < m_offlineControls.size(); ++i)
    {
        if (m_offlineControls[i] == control)
            return true;
    }

    return false;
}

// 分段控件宽度随文案变化（「离线激活 (不可用)」比默认文案长）
void CLicenseDlg::ResizeSeg()
{
    HWND seg = ::GetDlgItem(GetSafeHwnd(), IDC_LIC_SEG);
    if (seg == nullptr)
        return;

    ::SetWindowPos(seg, nullptr, 0, 0, Ui::MeasureSegWidth(this, IDC_LIC_SEG),
                   Ui::Design::kSegHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// 分段控件切页：在线 / 离线两组控件互斥显示，选中项用白底主色描边标记
void CLicenseDlg::SelectTabPage(int index)
{
    m_page = (index == 1) ? 1 : 0;

    for (size_t i = 0; i < m_onlineControls.size(); ++i)
        ::ShowWindow(m_onlineControls[i], (m_page == 0) ? SW_SHOW : SW_HIDE);

    for (size_t i = 0; i < m_offlineControls.size(); ++i)
        ::ShowWindow(m_offlineControls[i], (m_page == 1) ? SW_SHOW : SW_HIDE);

    Ui::SetSegSelection(this, IDC_LIC_SEG, m_page);

    RegisterLicenseSurfaces();
    UpdateOfflineStatusLayout();
    Invalidate(FALSE);
}

// 分段控件点击回调（Ui::kSegChanged 由控件在命中条目后发给父窗口）
LRESULT CLicenseDlg::OnSegChanged(WPARAM wParam, LPARAM /*lParam*/)
{
    SelectTabPage((int)wParam);
    return 0;
}

// 离线页状态胶囊：宽度随文字自适应，右侧说明文字跟随其后（对应原型 .statusline）
void CLicenseDlg::UpdateOfflineStatusLayout()
{
    HWND pill = ::GetDlgItem(GetSafeHwnd(), IDC_LIC_OFFLINE_STATUS);
    HWND hint = ::GetDlgItem(GetSafeHwnd(), IDC_LIC_OFFLINE_HINT2);
    if (pill == nullptr || hint == nullptr)
        return;

    const int width = Ui::MeasurePillWidth(pill, Ui::GetFont(kYahei, 8, true));

    CRect pillRect;
    ::GetWindowRect(pill, &pillRect);
    ::MapWindowPoints(HWND_DESKTOP, GetSafeHwnd(), (LPPOINT)&pillRect, 2);
    ::SetWindowPos(pill, nullptr, pillRect.left, pillRect.top, width, pillRect.Height(),
                   SWP_NOZORDER | SWP_NOACTIVATE);

    ::SetWindowPos(hint, nullptr, pillRect.left + width + 8, pillRect.top,
                   kBodyRight - (pillRect.left + width + 8) - 16, pillRect.Height(),
                   SWP_NOZORDER | SWP_NOACTIVATE);

    ::InvalidateRect(pill, nullptr, TRUE);
}

// 离线页状态胶囊的文字与语义色（成功后变绿、失败变红）
void CLicenseDlg::SetOfflineStatus(const wchar_t* text, COLORREF color)
{
    HWND pill = ::GetDlgItem(GetSafeHwnd(), IDC_LIC_OFFLINE_STATUS);
    if (pill == nullptr)
        return;

    ::SetWindowTextW(pill, text);
    Ui::SetTextColor(pill, color);
    UpdateOfflineStatusLayout();
}

void CLicenseDlg::StartServerReachabilityCheck()
{
    if (m_reachabilityTaskRunning)
        return;

    m_reachabilityTaskRunning = true;

    // 对应 C# CheckServerReachabilityAsync()：不阻塞 UI
    HWND hwnd = GetSafeHwnd();
    std::thread([hwnd]() {
        AsyncResult* result = new AsyncResult();
        result->task = TaskServerReachability;
        result->value = License::Internal::IsServerReachable();

        if (!::PostMessageW(hwnd, kAsyncDoneMessage, 0, (LPARAM)result))
            delete result;
    }).detach();
}

void CLicenseDlg::SetStatusLabel(UINT id, const wchar_t* text, COLORREF color)
{
    HWND label = ::GetDlgItem(GetSafeHwnd(), id);
    if (label == nullptr)
        return;

    ::SetWindowTextW(label, text);
    Ui::SetTextColor(label, color);
}

void CLicenseDlg::SetButtonText(UINT id, const wchar_t* text)
{
    CWnd* button = GetDlgItem(id);
    if (button != nullptr)
        button->SetWindowTextW(text);
}

std::string CLicenseDlg::GetKeyTextUtf8() const
{
    HWND edit = ::GetDlgItem(GetSafeHwnd(), IDC_LIC_KEY_EDIT);
    if (edit == nullptr)
        return std::string();

    int length = ::GetWindowTextLengthW(edit);
    if (length <= 0)
        return std::string();

    std::wstring text((size_t)length + 1, L'\0');
    ::GetWindowTextW(edit, &text[0], length + 1);
    text.resize((size_t)length);

    // 对应 C# 的 .Trim()
    size_t begin = 0;
    while (begin < text.size() && iswspace(text[begin]))
        ++begin;

    size_t end = text.size();
    while (end > begin && iswspace(text[end - 1]))
        --end;

    return WideToUtf8(text.substr(begin, end - begin));
}

// ===========================================================================
// 绘制
// ===========================================================================

void CLicenseDlg::OnPaint()
{
    CPaintDC dc(this);
    HDC hdc = dc.GetSafeHdc();

    CRect client;
    GetClientRect(&client);

    // 工作区底色（原型 --page #F1F5FB）
    ::FillRect(hdc, &client, Ui::GetSurfaceBrush(kPageBack));

    // 浅蓝白头部：顶部 3px 主色横条 + 底部 1px 分隔线（对应原型 .lic-head）
    RECT header = { 0, 0, client.right, kHeaderHeight };
    ::FillRect(hdc, &header, Ui::GetSurfaceBrush(Ui::Design::kHeadBack));

    RECT topBar = { 0, 0, client.right, 3 };
    ::FillRect(hdc, &topBar, Ui::GetSurfaceBrush(Ui::Design::kAccent));

    RECT headerLine = { 0, kHeaderHeight - 1, client.right, kHeaderHeight };
    ::FillRect(hdc, &headerLine, Ui::GetSurfaceBrush(Ui::Design::kHeadBorder));

    ::SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ oldFont = ::SelectObject(hdc, Ui::GetFont(kYahei, 12, true));

    // 对应 lblTitle（15px 700，头部内容整体垂直居中）
    ::SetTextColor(hdc, kColorTitle);
    RECT titleRect = { 20, 15, client.right - 120, 33 };
    ::DrawTextW(hdc, L"网络打印服务器", -1, &titleRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    ::SelectObject(hdc, Ui::GetFont(kYahei, 8, false));
    ::SetTextColor(hdc, kColorGray);

    // 副标题（对应 lblSubtitle，11px #64748B）
    RECT subtitleRect = { 20, 36, client.right - 120, 50 };
    ::DrawTextW(hdc, L"本软件需要授权后才能使用", -1, &subtitleRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // 版本胶囊（22 高 / 浅蓝底 + 主色描边 + 主色文字，对应原型 .lic-ver）
    std::wstring version = L"v" + Utf8ToWide(ProgramHost::GetDisplayVersion());
    const int versionWidth =
        Ui::MeasureTextWidth(GetSafeHwnd(), Ui::GetFont(kYahei, 8, true), version.c_str()) + 20;
    RECT versionPill = { client.right - 20 - versionWidth, 19,
                         client.right - 20, 19 + Ui::Design::kPillHeight };
    Ui::Gfx::FillRoundRect(hdc, versionPill, Ui::Design::kPillHeight / 2,
                           Ui::Design::kAccentSoft);
    Ui::Gfx::DrawRoundRect(hdc, versionPill, Ui::Design::kPillHeight / 2, RGB(187, 209, 247));
    ::SelectObject(hdc, Ui::GetFont(kYahei, 8, true));
    ::SetTextColor(hdc, Ui::Design::kAccent);
    ::DrawTextW(hdc, version.c_str(), -1, &versionPill,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    ::SelectObject(hdc, oldFont);

    // 卡片：柔和阴影 + 圆角白底 + 描边（离线页与在线页互斥）
    const int cardIndex[] = { 0, (m_page == 0) ? 1 : 3, (m_page == 0) ? 2 : -1 };
    for (size_t i = 0; i < _countof(cardIndex); ++i)
    {
        if (cardIndex[i] < 0)
            continue;

        RECT card = CardRect(cardIndex[i]);
        Ui::Gfx::DrawSoftShadow(hdc, card, Ui::Design::kRadiusCard, 6, 1, 20, RGB(15, 23, 42));
        Ui::Gfx::FillRoundRect(hdc, card, Ui::Design::kRadiusCard, Ui::Design::kCardBack);
        Ui::Gfx::DrawRoundRect(hdc, card, Ui::Design::kRadiusCard, Ui::Design::kCardBorder);
    }
}

void CLicenseDlg::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct)
{
    if (!Ui::DrawFlatButton(lpDrawItemStruct))
        CDialogEx::OnDrawItem(nIDCtl, lpDrawItemStruct);
}

HBRUSH CLicenseDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
    HBRUSH brush = nullptr;

    if (nCtlColor == CTLCOLOR_STATIC || nCtlColor == CTLCOLOR_EDIT)
    {
        pDC->SetBkMode(TRANSPARENT);

        COLORREF color = Ui::Design::kTextBody;
        Ui::TryGetTextColor(pWnd->GetSafeHwnd(), color);
        pDC->SetTextColor(color);

        // 页签区与对话框主体同为页面底色，卡片/分区内的控件取所在表面色
        COLORREF surface = kPageBack;
        Ui::TryGetSurfaceColor(pWnd->GetSafeHwnd(), surface);
        return Ui::GetSurfaceBrush(surface);
    }
    else if (nCtlColor == CTLCOLOR_DLG)
    {
        brush = Ui::GetSurfaceBrush(kPageBack);
    }
    else if (nCtlColor == CTLCOLOR_BTN)
    {
        brush = (HBRUSH)m_faceBrush.GetSafeHandle();
    }

    if (brush == nullptr)
        return CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);

    return brush;
}

BOOL CLicenseDlg::OnEraseBkgnd(CDC* pDC)
{
    CRect client;
    GetClientRect(&client);
    pDC->FillSolidRect(&client, kPageBack);
    return TRUE;
}

void CLicenseDlg::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
    // 对应 MinimumSize == MaximumSize == Size：窗口尺寸固定
    lpMMI->ptMinTrackSize.x = kFormWidth;
    lpMMI->ptMinTrackSize.y = kFormHeight;
    lpMMI->ptMaxTrackSize = lpMMI->ptMinTrackSize;

    CDialogEx::OnGetMinMaxInfo(lpMMI);
}

void CLicenseDlg::OnClose()
{
    // 对应 C# LicenseForm_FormClosing：
    // 用户手动关闭且当前未授权 → 直接结束进程，防止绕过授权
    if (License::CurrentStatus() != License::Status::Licensed)
    {
        Log::WriteLog("[授权] 用户在未授权状态下关闭授权窗口，进程退出");
        ::TerminateProcess(::GetCurrentProcess(), (UINT)-1);
        return;
    }

    CDialogEx::OnClose();
}

// ===========================================================================
// 按钮
// ===========================================================================

void CLicenseDlg::OnCopyMachineCode()
{
    // 对应 C#：Clipboard.SetText(_cachedMachineCode) 并提示
    std::wstring text = Utf8ToWide(m_cachedMachineCode);

    if (::OpenClipboard(GetSafeHwnd()))
    {
        ::EmptyClipboard();

        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory != nullptr)
        {
            void* target = ::GlobalLock(memory);
            if (target != nullptr)
            {
                memcpy(target, text.c_str(), bytes);
                ::GlobalUnlock(memory);
                ::SetClipboardData(CF_UNICODETEXT, memory);
            }
            else
            {
                ::GlobalFree(memory);
            }
        }

        ::CloseClipboard();
    }

    ::MessageBoxW(GetSafeHwnd(), L"机器码已复制到剪贴板", L"提示", MB_OK | MB_ICONINFORMATION);
}

void CLicenseDlg::OnActivateOnline()
{
    std::string licenseKey = GetKeyTextUtf8();
    if (licenseKey.empty())
    {
        SetStatusLabel(IDC_LIC_ONLINE_STATUS, L"请输入授权码", kColorRed);
        return;
    }

    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_LIC_BTN_ACTIVATE_ONLINE), FALSE);
    SetButtonText(IDC_LIC_BTN_ACTIVATE_ONLINE, L"激活中...");
    SetStatusLabel(IDC_LIC_ONLINE_STATUS, L"正在向授权服务器发送请求...", kColorOrange);

    HWND hwnd = GetSafeHwnd();
    std::thread([hwnd, licenseKey]() {
        AsyncResult* result = new AsyncResult();
        result->task = TaskActivateOnline;
        result->value = false;

        try
        {
            License::ActivationResult activation = License::ActivateOnline(licenseKey);
            result->value = activation.success;
            result->message = activation.message;
        }
        catch (const std::exception& ex)
        {
            result->value = false;
            result->message = "网络错误: " + std::string(ex.what());
        }

        if (!::PostMessageW(hwnd, kAsyncDoneMessage, 0, (LPARAM)result))
            delete result;
    }).detach();
}

void CLicenseDlg::OnSelectLicFile()
{
    CFileDialog dialog(TRUE, L"lic", nullptr,
                       OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST,
                       L"授权文件 (*.lic)|*.lic|所有文件 (*.*)|*.*||", this);

    dialog.m_ofn.lpstrTitle = L"选择离线授权文件";

    if (dialog.DoModal() != IDOK)
        return;

    m_selectedLicFilePath = dialog.GetPathName();

    std::wstring fileName = m_selectedLicFilePath;
    size_t slash = fileName.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        fileName = fileName.substr(slash + 1);

    SetOfflineStatus( (L"已选择: " + fileName).c_str(), kColorGreen);
    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_LIC_BTN_ACTIVATE_OFFLINE), TRUE);
}

void CLicenseDlg::OnActivateOffline()
{
    if (m_selectedLicFilePath.empty() ||
        ::GetFileAttributesW(m_selectedLicFilePath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        ::MessageBoxW(GetSafeHwnd(), L"请先选择有效的 .lic 授权文件", L"提示",
                      MB_OK | MB_ICONWARNING);
        return;
    }

    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_LIC_BTN_ACTIVATE_OFFLINE), FALSE);
    SetButtonText(IDC_LIC_BTN_ACTIVATE_OFFLINE, L"导入中...");

    HWND hwnd = GetSafeHwnd();
    std::wstring filePath = m_selectedLicFilePath;

    std::thread([hwnd, filePath]() {
        AsyncResult* result = new AsyncResult();
        result->task = TaskActivateOffline;
        result->value = false;

        try
        {
            License::ActivationResult activation = License::ActivateOffline(filePath);
            result->value = activation.success;
            result->message = activation.message;
        }
        catch (const std::exception& ex)
        {
            result->value = false;
            result->message = "读取授权文件失败: " + std::string(ex.what());
        }

        if (!::PostMessageW(hwnd, kAsyncDoneMessage, 0, (LPARAM)result))
            delete result;
    }).detach();
}

void CLicenseDlg::OnReportMachine()
{
    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_LIC_BTN_REPORT), FALSE);
    SetButtonText(IDC_LIC_BTN_REPORT, L"上报中...");
    SetStatusLabel(IDC_LIC_ONLINE_STATUS, L"正在向授权服务器上报本机信息...", kColorOrange);

    HWND hwnd = GetSafeHwnd();
    std::thread([hwnd]() {
        AsyncResult* result = new AsyncResult();
        result->task = TaskReportMachine;

        try
        {
            result->value = License::ReportMachine();
        }
        catch (...)
        {
            result->value = false;
        }

        if (!::PostMessageW(hwnd, kAsyncDoneMessage, 0, (LPARAM)result))
            delete result;
    }).detach();
}

void CLicenseDlg::OnCheckLicense()
{
    ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_LIC_BTN_CHECK), FALSE);
    SetButtonText(IDC_LIC_BTN_CHECK, L"验证中...");
    SetStatusLabel(IDC_LIC_ONLINE_STATUS, L"正在联网验证授权...", kColorOrange);

    HWND hwnd = GetSafeHwnd();
    std::thread([hwnd]() {
        AsyncResult* result = new AsyncResult();
        result->task = TaskCheckLicense;

        try
        {
            result->value = License::ManualCheck();
        }
        catch (...)
        {
            result->value = false;
        }

        if (!::PostMessageW(hwnd, kAsyncDoneMessage, 0, (LPARAM)result))
            delete result;
    }).detach();
}

LRESULT CLicenseDlg::OnAsyncDone(WPARAM /*wParam*/, LPARAM lParam)
{
    AsyncResult* result = reinterpret_cast<AsyncResult*>(lParam);
    if (result == nullptr)
        return 0;

    const AsyncTask task = result->task;
    const bool value = result->value;
    const std::string message = result->message;
    delete result;

    switch (task)
    {
    case TaskServerReachability:
        m_reachabilityTaskRunning = false;
        m_isServerReachable = value;

        if (m_isServerReachable)
        {
            // 外网授权服务器可达 → 禁用离线激活
            m_offlineTabDisabled = true;
            ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_LIC_BTN_SELECT_LIC), FALSE);
            ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_LIC_BTN_ACTIVATE_OFFLINE), FALSE);

            // 分段控件条目文字标注「不可用」，与 C# 的页签文案一致
            Ui::SetSegItemText(this, IDC_LIC_SEG, 1, L"离线激活 (不可用)");
            ResizeSeg();

            SetOfflineStatus(
                           L"检测到外网授权服务器可达，请使用在线激活", kColorOrange);

            // 与 C# 一致：检测到可达时把界面切回在线激活页
            if (m_page == 1)
                SelectTabPage(0);
        }
        break;

    case TaskActivateOnline:
        if (value)
        {
            SetStatusLabel(IDC_LIC_ONLINE_STATUS,
                           (L"" + Utf8ToWide(message)).c_str(), kColorGreen);
            SetButtonText(IDC_LIC_BTN_ACTIVATE_ONLINE, L"✓ 激活成功");

            ::Sleep(800);
            EndDialog(IDOK);
        }
        else
        {
            SetStatusLabel(IDC_LIC_ONLINE_STATUS,
                           (L"" + Utf8ToWide(message)).c_str(), kColorRed);
            ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_LIC_BTN_ACTIVATE_ONLINE), TRUE);
            SetButtonText(IDC_LIC_BTN_ACTIVATE_ONLINE, L"激  活");
        }
        break;

    case TaskActivateOffline:
        if (value)
        {
            SetOfflineStatus(
                           (L"" + Utf8ToWide(message)).c_str(), kColorGreen);
            SetButtonText(IDC_LIC_BTN_ACTIVATE_OFFLINE, L"✓ 激活成功");

            ::Sleep(800);
            EndDialog(IDOK);
        }
        else
        {
            std::wstring text = Utf8ToWide(message);
            ::MessageBoxW(GetSafeHwnd(), text.c_str(), L"激活失败", MB_OK | MB_ICONERROR);

            SetOfflineStatus( (L"" + text).c_str(), kColorRed);
            ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_LIC_BTN_ACTIVATE_OFFLINE), TRUE);
            SetButtonText(IDC_LIC_BTN_ACTIVATE_OFFLINE, L"导入激活");
        }
        break;

    case TaskReportMachine:
        if (value)
        {
            SetStatusLabel(IDC_LIC_ONLINE_STATUS,
                           L"上报成功！管理员处理授权后，点击【检查授权】即可激活",
                           kColorGreen);
            SetButtonText(IDC_LIC_BTN_REPORT, L"✓ 已上报");
        }
        else
        {
            SetStatusLabel(IDC_LIC_ONLINE_STATUS,
                           L"上报失败，请检查网络连接后重试", kColorRed);
            ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_LIC_BTN_REPORT), TRUE);
            SetButtonText(IDC_LIC_BTN_REPORT, L"上报本机信息");
        }
        break;

    case TaskCheckLicense:
        if (value)
        {
            SetStatusLabel(IDC_LIC_ONLINE_STATUS, L"授权有效！即将进入程序...", kColorGreen);
            SetButtonText(IDC_LIC_BTN_CHECK, L"✓ 授权有效");

            ::Sleep(800);
            EndDialog(IDOK);
        }
        else
        {
            if (License::CurrentStatus() == License::Status::NetworkError)
            {
                SetStatusLabel(IDC_LIC_ONLINE_STATUS,
                               L"网络异常，请稍后重试或使用离线激活", kColorOrange);
            }
            else
            {
                SetStatusLabel(IDC_LIC_ONLINE_STATUS,
                               L"未检测到有效授权，请联系管理员处理", kColorRed);
            }

            ::EnableWindow(::GetDlgItem(GetSafeHwnd(), IDC_LIC_BTN_CHECK), TRUE);
            SetButtonText(IDC_LIC_BTN_CHECK, L"检查授权");
        }
        break;

    default:
        break;
    }

    return 0;
}

void CLicenseDlg::CheckAndClose()
{
    if (License::CurrentStatus() == License::Status::Licensed)
        EndDialog(IDOK);
}

} // namespace ps

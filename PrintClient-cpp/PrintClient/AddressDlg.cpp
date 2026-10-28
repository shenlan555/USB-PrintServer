#include "pch.h"
#include "AddressDlg.h"

#include "UiTheme.h"
#include "Wmi.h"

namespace ps
{

namespace
{

const int kFormWidth = 400;
const int kFormHeight = 165;

const COLORREF kColorWhite = RGB(255, 255, 255);
const COLORREF kBtnHostBack = RGB(40, 140, 200);
const COLORREF kBtnIpBack = RGB(100, 140, 200);

const wchar_t* kYahei = L"微软雅黑";

HWND CreateChild(HWND parent, const wchar_t* className, const wchar_t* text, DWORD style,
                 int x, int y, int w, int h, UINT id, HFONT font)
{
    HWND child = ::CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style,
                                   x, y, w, h, parent, (HMENU)(UINT_PTR)id,
                                   ::AfxGetInstanceHandle(), nullptr);
    if (child != nullptr && font != nullptr)
        ::SendMessageW(child, WM_SETFONT, (WPARAM)font, MAKELPARAM(TRUE, 0));

    return child;
}

} // namespace

IMPLEMENT_DYNAMIC(CAddressDlg, CDialogEx)

BEGIN_MESSAGE_MAP(CAddressDlg, CDialogEx)
    ON_WM_DRAWITEM()
    ON_BN_CLICKED(IDC_PC_ADDR_BTN_HOST, &CAddressDlg::OnOK)
    ON_BN_CLICKED(IDC_PC_ADDR_BTN_IP, &CAddressDlg::OnCancel)
END_MESSAGE_MAP()

CAddressDlg::CAddressDlg(const std::string& hostname, const std::string& ip, CWnd* pParent)
    : CDialogEx(IDD_ADDRESS_DIALOG, pParent)
    , m_hostname(hostname)
    , m_ip(ip)
{
}

BOOL CAddressDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    // 对应 Size = new Size(400, 165) + FixedDialog
    SetWindowPos(nullptr, 0, 0, kFormWidth, kFormHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);

    // 对应 StartPosition = FormStartPosition.CenterParent
    CenterWindow(GetParent());

    HWND self = GetSafeHwnd();

    HWND label = CreateChild(self, L"Static", L"请选择打印机连接地址：", SS_LEFT,
                             16, 16, 360, 18, IDC_PC_ADDR_HINT, Ui::GetFont(kYahei, 9, false));
    ::InvalidateRect(label, nullptr, TRUE);

    CreateChild(self, L"Button", (L"🖥️ 主机名: " + Utf8ToWide(m_hostname)).c_str(),
                BS_OWNERDRAW, 16, 46, 350, 32, IDC_PC_ADDR_BTN_HOST,
                Ui::GetFont(kYahei, 9, true));
    Ui::MakeFlatButton(this, IDC_PC_ADDR_BTN_HOST, kBtnHostBack, kColorWhite);

    CreateChild(self, L"Button", (L"🌐 IP地址: " + Utf8ToWide(m_ip)).c_str(),
                BS_OWNERDRAW, 16, 86, 350, 32, IDC_PC_ADDR_BTN_IP,
                Ui::GetFont(kYahei, 9, false));
    Ui::MakeFlatButton(this, IDC_PC_ADDR_BTN_IP, kBtnIpBack, kColorWhite);

    return TRUE;
}

void CAddressDlg::OnOK()
{
    m_choseHostname = true;
    EndDialog(IDYES);
}

void CAddressDlg::OnCancel()
{
    // 对应 CancelButton = btnIp → DialogResult.No
    m_choseHostname = false;
    EndDialog(IDNO);
}

void CAddressDlg::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct)
{
    if (!Ui::DrawFlatButton(lpDrawItemStruct))
        CDialogEx::OnDrawItem(nIDCtl, lpDrawItemStruct);
}

} // namespace ps

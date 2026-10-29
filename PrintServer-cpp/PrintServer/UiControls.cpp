#include "pch.h"
#include "UiControls.h"

#include <commctrl.h>

#include "UiTheme.h"

namespace ps
{
namespace Ui
{

IMPLEMENT_DYNAMIC(CColorTab, CWnd)

BEGIN_MESSAGE_MAP(CColorTab, CWnd)
    ON_WM_ERASEBKGND()
    ON_WM_PAINT()
END_MESSAGE_MAP()

CColorTab::CColorTab()
    : m_backColor(::GetSysColor(COLOR_BTNFACE))
    , m_superProc(nullptr)
{
}

BOOL CColorTab::OnEraseBkgnd(CDC* pDC)
{
    CRect rect;
    GetClientRect(&rect);
    pDC->FillSolidRect(&rect, m_backColor);
    return TRUE;
}

void CColorTab::OnPaint()
{
    // 先让控件自身（含系统主题）完成页签与页体绘制
    Default();

    CDC* dc = GetDC();
    if (dc == nullptr)
        return;

    // 系统主题把页体画成白色，与对话框的页面底色不一致；这里按页面底色重铺显示区，
    // 让页内的白色卡片/面板能与页面底色形成清晰的层次
    CRect display;
    GetClientRect(&display);
    ::SendMessageW(GetSafeHwnd(), TCM_ADJUSTRECT, FALSE, (LPARAM)&display);
    dc->FillSolidRect(display, m_backColor);

    // 卡片/面板的柔和阴影落在它们自己的矩形之外，会被本控件盖住，
    // 只能由本控件在页体上补画（只取本控件的兄弟部件）
    DrawCardShadows(dc->GetSafeHdc(), GetSafeHwnd());

    ReleaseDC(dc);
}

// ===========================================================================
// 左侧导航栏
// ===========================================================================

IMPLEMENT_DYNAMIC(CNavBar, CWnd)

BEGIN_MESSAGE_MAP(CNavBar, CWnd)
    ON_WM_ERASEBKGND()
    ON_WM_PAINT()
    ON_WM_MOUSEMOVE()
    ON_WM_MOUSELEAVE()
    ON_WM_LBUTTONDOWN()
    ON_WM_NCHITTEST()
END_MESSAGE_MAP()

CNavBar::CNavBar()
    : m_selection(0)
    , m_hover(-1)
    , m_tracking(false)
    , m_superProc(nullptr)
{
}

CRect CNavBar::GetItemRect(int index)
{
    const int top = kBrandHeight + 12 + index * (kItemHeight + kItemGap);
    return CRect(10, top, kWidth - 10, top + kItemHeight);
}

void CNavBar::SetBrand(const wchar_t* name, const wchar_t* version)
{
    m_brandName = (name != nullptr) ? name : L"";
    m_brandVersion = (version != nullptr) ? version : L"";
    InvalidateRect(nullptr, FALSE);
}

void CNavBar::SetSelection(int index)
{
    if (index == m_selection)
        return;

    m_selection = index;
    InvalidateRect(nullptr, FALSE);
}

BOOL CNavBar::OnEraseBkgnd(CDC* pDC)
{
    CRect rect;
    GetClientRect(&rect);
    pDC->FillSolidRect(&rect, Design::kNavBack);
    return TRUE;
}

void CNavBar::OnPaint()
{
    CPaintDC dc(this);
    HDC hdc = dc.GetSafeHdc();

    CRect client;
    GetClientRect(&client);

    ::FillRect(hdc, &client, GetSurfaceBrush(Design::kNavBack));

    // ---- 品牌区：全窗口唯一的重色块（下圆角，上两角填平） ----
    RECT brand = { 0, 0, client.right, kBrandHeight };
    Gfx::FillRoundRect(hdc, brand, 10, Design::kAccent);

    RECT brandTop = { 0, 0, client.right, 10 };
    ::FillRect(hdc, &brandTop, GetSurfaceBrush(Design::kAccent));

    ::SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ oldFont = ::SelectObject(hdc, ps::Ui::GetFont(L"微软雅黑", 11, true));
    ::SetTextColor(hdc, Design::kTextOnBrand);
    RECT nameRect = { 12, 14, client.right - 10, 36 };
    ::DrawTextW(hdc, m_brandName, -1, &nameRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    ::SelectObject(hdc, ps::Ui::GetFont(L"微软雅黑", 8, false));
    ::SetTextColor(hdc, Design::kTextOnBrandSub);
    RECT versionRect = { 13, 36, client.right - 10, 54 };
    ::DrawTextW(hdc, m_brandVersion, -1, &versionRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    // ---- 导航项（激活项 = 主色实底 + 白字） ----
    static const Gfx::Icon kIcons[kItemCount] = {
        Gfx::Icon::Signal, Gfx::Icon::Printer, Gfx::Icon::Document, Gfx::Icon::Shield
    };
    static const wchar_t* kLabels[kItemCount] = {
        L"服务器", L"设备列表", L"运行日志", L"软件信息"
    };

    for (int i = 0; i < kItemCount; ++i)
    {
        const CRect item = GetItemRect(i);
        const bool active = (i == m_selection);
        const bool hover = (!active && i == m_hover);

        const COLORREF ink = active ? Design::kTextOnBrand
                                    : (hover ? Design::kTextTitle : Design::kNavText);

        if (active)
        {
            Gfx::DrawSoftShadow(hdc, item, 8, 4, 1, 60, Design::kAccent);
            Gfx::FillRoundRect(hdc, item, 8, Design::kAccent);
        }
        else if (hover)
        {
            Gfx::FillRoundRect(hdc, item, 8, Design::kNavHover);
        }

        const int iconTop = item.top + ((kItemHeight - 16) / 2);
        RECT iconRect = { item.left + 12, iconTop, item.left + 28, iconTop + 16 };
        Gfx::DrawIcon(hdc, kIcons[i], iconRect, ink);

        ::SelectObject(hdc, ps::Ui::GetFont(L"微软雅黑", 9, active));
        ::SetTextColor(hdc, ink);
        RECT textRect = { item.left + 38, item.top, item.right - 8, item.bottom };
        ::DrawTextW(hdc, kLabels[i], -1, &textRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    // ---- 右侧 1px 分隔线 ----
    RECT edge = { client.right - 1, 0, client.right, client.bottom };
    ::FillRect(hdc, &edge, GetSurfaceBrush(Design::kNavBorder));

    ::SelectObject(hdc, oldFont);
}

int CNavBar::HitTest(CPoint point) const
{
    for (int i = 0; i < kItemCount; ++i)
    {
        if (GetItemRect(i).PtInRect(point))
            return i;
    }

    return -1;
}

void CNavBar::OnMouseMove(UINT /*nFlags*/, CPoint point)
{
    if (!m_tracking)
    {
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, GetSafeHwnd(), 0 };
        ::TrackMouseEvent(&track);
        m_tracking = true;
    }

    const int hover = HitTest(point);
    if (hover != m_hover)
    {
        m_hover = hover;
        InvalidateRect(nullptr, FALSE);
    }
}

void CNavBar::OnMouseLeave()
{
    m_tracking = false;

    if (m_hover != -1)
    {
        m_hover = -1;
        InvalidateRect(nullptr, FALSE);
    }
}

void CNavBar::OnLButtonDown(UINT /*nFlags*/, CPoint point)
{
    const int index = HitTest(point);
    if (index < 0)
        return;

    CWnd* parent = GetParent();
    if (parent != nullptr && parent->GetSafeHwnd() != nullptr)
        parent->SendMessageW(kNavSelectionChanged, (WPARAM)index, 0);
}

// 导航栏是 Static 窗口，默认命中测试返回 HTTRANSPARENT → 真实鼠标点击会被系统
// 转交给父窗口，导航项就永远点不动；这里显式返回 HTCLIENT 让点击落在导航栏上。
LRESULT CNavBar::OnNcHitTest(CPoint /*point*/)
{
    return HTCLIENT;
}

} // namespace Ui
} // namespace ps

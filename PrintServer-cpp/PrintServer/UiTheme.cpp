#include "pch.h"
#include "UiTheme.h"

#include <commctrl.h>
#include <string>
#include <map>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace ps
{
namespace Ui
{

namespace
{

struct FontKey
{
    std::wstring family;
    int points;
    bool bold;

    bool operator<(const FontKey& other) const
    {
        if (family != other.family)
            return family < other.family;
        if (points != other.points)
            return points < other.points;
        return bold < other.bold;
    }
};

// 控件外观
struct Part
{
    enum class Kind { Button, Card, Pill, Field, Panel, Badge, LogBox, Seg };

    Kind kind = Kind::Button;
    HWND hwnd = nullptr;
    COLORREF back = RGB(0, 0, 0);        // 按钮底色
    COLORREF fore = RGB(255, 255, 255);  // 按钮文字/图标色
    COLORREF surface = Design::kCardBack;
    COLORREF border = CLR_INVALID;       // 面板描边（CLR_INVALID = 不描边）
    // 圆角之外四角要铺的底色。控件落在白卡片上时可省（同 surface）；一旦控件落在
    // 页面底色上（如页面标题行的胶囊/图标按钮），必须显式指定，否则四角会露出白底
    COLORREF backdrop = CLR_INVALID;     // CLR_INVALID = 取 surface
    COLORREF accent = Design::kAccent;   // 徽标底色
    int radius = Design::kRadiusCard;
    Gfx::Icon icon = Gfx::Icon::None;
    bool hover = false;
    bool outline = false;                // 次级按钮：描边风格，不用实底
    bool on = false;                     // 分段控件条目：是否选中
};

std::map<FontKey, HFONT> g_fontCache;
std::map<HWND, Part> g_parts;

// 分段控件（原型 .seg）的条目文案与选中项：一个控件画整组，
// 点击命中在 PartSubclassProc 里完成
struct SegData
{
    std::vector<std::wstring> items;
    int selected = 0;
};

std::map<HWND, SegData> g_segs;
std::map<HWND, COLORREF> g_textColors;
std::map<HWND, CString> g_logNotes;      // 日志框顶部条右侧文案
// 卡片表面登记项：parent 为该登记所属的对话框（nullptr = 不限对话框）
struct SurfaceEntry
{
    HWND parent;
    RECT rect;
    COLORREF color;
};

std::vector<SurfaceEntry> g_surfaces;   // 卡片矩形（父窗口客户区坐标）→ 表面色
std::map<COLORREF, HBRUSH> g_surfaceBrushes;
int GetScreenDpiY()
{
    HDC screen = ::GetDC(nullptr);
    int dpi = ::GetDeviceCaps(screen, LOGPIXELSY);
    ::ReleaseDC(nullptr, screen);
    return (dpi > 0) ? dpi : 96;
}

BYTE Luminance(COLORREF color)
{
    return (BYTE)((GetRValue(color) * 77 + GetGValue(color) * 150 + GetBValue(color) * 29) / 256);
}

bool IsDark(COLORREF color)
{
    return Luminance(color) < 140;
}

// 线性混色：percent 为第二种颜色的比例（0..100）
COLORREF Mix(COLORREF from, COLORREF to, int percent)
{
    const int r = GetRValue(from) + (GetRValue(to) - GetRValue(from)) * percent / 100;
    const int g = GetGValue(from) + (GetGValue(to) - GetGValue(from)) * percent / 100;
    const int b = GetBValue(from) + (GetBValue(to) - GetBValue(from)) * percent / 100;
    return RGB(r, g, b);
}

COLORREF Lighten(COLORREF color, int percent) { return Mix(color, RGB(255, 255, 255), percent); }
COLORREF Darken(COLORREF color, int percent) { return Mix(color, RGB(0, 0, 0), percent); }

// 圆角矩形之外四角要铺的底色（不铺会残留旧像素或露出与周围不一致的白底）
COLORREF Backdrop(const Part& part)
{
    return (part.backdrop != CLR_INVALID) ? part.backdrop : part.surface;
}

// 跟踪鼠标离开以复位悬停态
LRESULT CALLBACK PartSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR idSubclass, DWORD_PTR refData)
{
    auto it = g_parts.find(hwnd);

    switch (message)
    {
    case WM_NCHITTEST:
        // Static 控件的默认命中测试返回 HTTRANSPARENT，真实鼠标点击会被系统转交到父窗口，
        // 于是「可点击的静态部件」（分段控件 seg）永远收不到点击 → 必须显式声明 HTCLIENT。
        if (it != g_parts.end() && it->second.kind == Part::Kind::Seg)
            return HTCLIENT;
        break;

    case WM_MOUSEMOVE:
        if (it != g_parts.end() && !it->second.hover)
        {
            it->second.hover = true;
            TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd, 0 };
            ::TrackMouseEvent(&track);
            ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;

    case WM_MOUSELEAVE:
        if (it != g_parts.end() && it->second.hover)
        {
            it->second.hover = false;
            ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;

    case WM_LBUTTONDOWN:
        // 分段控件：按点击位置命中条目并通知父窗口切换
        if (it != g_parts.end() && it->second.kind == Part::Kind::Seg)
        {
            auto segIt = g_segs.find(hwnd);
            if (segIt != g_segs.end())
            {
                HFONT segFont = (HFONT)::SendMessageW(hwnd, WM_GETFONT, 0, 0);
                const int x = GET_X_LPARAM(lParam);
                int left = Design::kSegPad;

                for (size_t i = 0; i < segIt->second.items.size(); ++i)
                {
                    const int width =
                        MeasureTextWidth(hwnd, segFont, segIt->second.items[i].c_str()) + 32;

                    if (x >= left && x < left + width)
                    {
                        HWND parent = ::GetParent(hwnd);
                        if (parent != nullptr)
                            ::SendMessageW(parent, kSegChanged, (WPARAM)i, 0);
                        break;
                    }

                    left += width + Design::kSegGap;
                }
            }
        }
        break;

    case WM_NCDESTROY:
        if (it != g_parts.end())
            g_parts.erase(it);
        g_segs.erase(hwnd);
        ::RemoveWindowSubclass(hwnd, PartSubclassProc, 1);
        break;

    case WM_ERASEBKGND:
        // 自绘部件总是完整覆盖自身矩形（按钮铺表面色、卡片先铺页面底色、胶囊与
        // 输入框底板铺表面色），抑制默认擦除可消除底色闪一下的闪烁
        return TRUE;

    default:
        break;
    }

    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

void RegisterPart(CWnd* parent, UINT id, const Part& part)
{
    if (parent == nullptr)
        return;

    CWnd* control = parent->GetDlgItem(id);
    if (control == nullptr || control->GetSafeHwnd() == nullptr)
        return;

    Gfx::Initialize();

    Part stored = part;
    stored.hwnd = control->GetSafeHwnd();
    g_parts[stored.hwnd] = stored;

    ::SetWindowSubclass(stored.hwnd, PartSubclassProc, 1, 0);
    ::InvalidateRect(stored.hwnd, nullptr, TRUE);
}

// ---- 按钮 ----

void DrawButton(HDC hdc, const RECT& rcItem, const Part& part, const wchar_t* text,
                HFONT font, bool disabled, bool pressed)
{
    // 圆角之外露出的四角用所在表面铺底（否则会残留旧像素）
    ::FillRect(hdc, &rcItem, GetSurfaceBrush(Backdrop(part)));

    RECT rc = rcItem;
    if (pressed)
        ::OffsetRect(&rc, 0, 1);

    COLORREF ink = part.fore;

    if (part.outline)
    {
        // 次级按钮：与卡片同底 + 1px 描边 + 文字色（对应原型 .btn-2 / .btn-danger）
        COLORREF back = part.surface;
        COLORREF edge = (part.border != CLR_INVALID) ? part.border : part.fore;

        if (disabled)
        {
            back = Mix(part.surface, part.fore, 8);
            edge = Mix(part.surface, part.fore, 45);
            ink = Mix(part.surface, part.fore, 45);
        }
        else if (pressed)
        {
            back = Mix(part.surface, ink, 16);
        }
        else if (part.hover)
        {
            back = Mix(part.surface, ink, 9);
        }

        Gfx::FillRoundRect(hdc, rc, Design::kRadiusButton, back);
        Gfx::DrawRoundRect(hdc, rc, Design::kRadiusButton, edge, 1.0f,
                           disabled ? (BYTE)150 : (BYTE)255);
    }
    else
    {
        COLORREF back = part.back;
        if (disabled)
            back = Mix(part.surface, part.back, 26);
        else if (pressed)
            back = Darken(part.back, 6);
        else if (part.hover)
            back = Darken(part.back, 8);

        // 实底主按钮为纯色平面（对应原型 .btn-1，无渐变无阴影）
        Gfx::FillRoundRect(hdc, rc, Design::kRadiusButton, back);

        if (disabled)
            ink = Mix(part.surface, part.fore, 55);
    }

    HGDIOBJ oldFont = nullptr;
    if (font != nullptr)
        oldFont = ::SelectObject(hdc, font);

    ::SetBkMode(hdc, TRANSPARENT);
    ::SetTextColor(hdc, ink);

    const int iconSize = (part.icon != Gfx::Icon::None) ? 15 : 0;
    const int iconGap = (iconSize > 0) ? 7 : 0;

    // 纯图标按钮（如页面标题行的刷新/设置）：图标居中，无文字
    if (text == nullptr || text[0] == 0)
    {
        if (iconSize > 0)
        {
            RECT iconRect = { rc.left + ((rc.right - rc.left) - iconSize) / 2,
                              rc.top + ((rc.bottom - rc.top) - iconSize) / 2,
                              rc.left + ((rc.right - rc.left) - iconSize) / 2 + iconSize,
                              rc.top + ((rc.bottom - rc.top) - iconSize) / 2 + iconSize };
            Gfx::DrawIcon(hdc, part.icon, iconRect, ink);
        }

        if (oldFont != nullptr)
            ::SelectObject(hdc, oldFont);
        return;
    }

    RECT calc = { 0, 0, 0, 0 };
    ::DrawTextW(hdc, text, -1, &calc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    const int textWidth = calc.right - calc.left;

    int startX = rc.left + ((rc.right - rc.left) - (textWidth + iconSize + iconGap)) / 2;
    if (startX < rc.left + 8)
        startX = rc.left + 8;

    if (iconSize > 0)
    {
        RECT iconRect = { startX, rc.top + ((rc.bottom - rc.top) - iconSize) / 2,
                          startX + iconSize, rc.top + ((rc.bottom - rc.top) - iconSize) / 2 + iconSize };
        Gfx::DrawIcon(hdc, part.icon, iconRect, ink);
    }

    RECT textRect = rc;
    textRect.left = startX + iconSize + iconGap;
    textRect.right -= 6;
    ::DrawTextW(hdc, text, -1, &textRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    if (oldFont != nullptr)
        ::SelectObject(hdc, oldFont);
}

// ---- 强调徽标（步骤数字或矢量图标） ----

void DrawBadge(HDC hdc, const RECT& rc, const Part& part, const wchar_t* text, HFONT font)
{
    // 徽标之外铺所在表面（面板底色）
    ::FillRect(hdc, &rc, GetSurfaceBrush(Backdrop(part)));

    const int radius = (part.radius >= 0) ? part.radius : 6;

    Gfx::FillRoundRect(hdc, rc, radius, part.accent);
    Gfx::DrawRoundRect(hdc, rc, radius, Darken(part.accent, 14));

    if (part.icon != Gfx::Icon::None)
    {
        RECT iconRect = rc;
        ::InflateRect(&iconRect, -5, -5);
        Gfx::DrawIcon(hdc, part.icon, iconRect, Design::kTextOnBrand);
        return;
    }

    if (text == nullptr || text[0] == 0)
        return;

    HGDIOBJ oldFont = nullptr;
    if (font != nullptr)
        oldFont = ::SelectObject(hdc, font);

    ::SetBkMode(hdc, TRANSPARENT);
    ::SetTextColor(hdc, Design::kTextOnBrand);

    RECT textRect = rc;
    ::DrawTextW(hdc, text, -1, &textRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (oldFont != nullptr)
        ::SelectObject(hdc, oldFont);
}

// ---- 卡片 ----

void DrawCard(HDC hdc, const RECT& rc, const Part& part, const wchar_t* title, HFONT font)
{
    const bool dark = IsDark(part.surface);

    // 圆角之外的四角露出所在页面底色（已抑制默认擦除，必须自己铺满整块矩形）
    ::FillRect(hdc, &rc, GetSurfaceBrush(Design::kPageBack));

    Gfx::FillRoundRect(hdc, rc, Design::kRadiusCard, part.surface);
    Gfx::DrawRoundRect(hdc, rc, Design::kRadiusCard,
                       dark ? Lighten(part.surface, 10) : Design::kCardBorder);

    if (title == nullptr || title[0] == 0)
        return;

    const int pad = Design::kCardPad;

    // 卡片标题行：左缘主色竖条（3x14，对应原型 .card-bar）
    const int headTop = rc.top + pad;
    RECT barRect = { rc.left + pad, headTop + (Design::kCardHeadHeight - 14) / 2,
                     rc.left + pad + 3, headTop + (Design::kCardHeadHeight - 14) / 2 + 14 };
    Gfx::FillRoundRect(hdc, barRect, 2, Design::kAccent);

    HGDIOBJ oldFont = nullptr;
    if (font != nullptr)
        oldFont = ::SelectObject(hdc, font);

    ::SetBkMode(hdc, TRANSPARENT);
    ::SetTextColor(hdc, dark ? Design::kTextOnBrand : Design::kTextTitle);

    RECT textRect = rc;
    textRect.left = rc.left + pad + 12;
    textRect.right = rc.right - pad;
    textRect.top = headTop;
    textRect.bottom = headTop + Design::kCardHeadHeight;
    ::DrawTextW(hdc, title, -1, &textRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    if (oldFont != nullptr)
        ::SelectObject(hdc, oldFont);
}

// ---- 状态胶囊 ----

void DrawPill(HDC hdc, const RECT& rc, const Part& part, const wchar_t* text, HFONT font)
{
    // 圆角之外的四角铺所在表面（不在白卡片上时必须显式指定 backdrop，否则会露白）
    ::FillRect(hdc, &rc, GetSurfaceBrush(Backdrop(part)));

    if (text == nullptr || text[0] == 0)
        return;

    COLORREF ink = Design::kTextBody;
    auto colorIt = g_textColors.find(part.hwnd);
    if (colorIt != g_textColors.end())
        ink = colorIt->second;

    // 底色 = 语义色向卡片表面靠拢，形成浅色胶囊（对应原型 .pill + ::before 圆点）
    const COLORREF pillBack = Mix(part.surface, ink, 15);

    Gfx::FillRoundRect(hdc, rc, (rc.bottom - rc.top) / 2, pillBack);

    // 左侧 6px 圆点（同文字色）
    const int dotSize = 6;
    RECT dot = { rc.left + 10, rc.top + ((rc.bottom - rc.top) - dotSize) / 2,
                 rc.left + 10 + dotSize, rc.top + ((rc.bottom - rc.top) - dotSize) / 2 + dotSize };
    Gfx::FillEllipse(hdc, dot, ink);

    HGDIOBJ oldFont = nullptr;
    if (font != nullptr)
        oldFont = ::SelectObject(hdc, font);

    ::SetBkMode(hdc, TRANSPARENT);
    ::SetTextColor(hdc, ink);

    RECT textRect = rc;
    textRect.left += 22;
    textRect.right -= 8;
    ::DrawTextW(hdc, text, -1, &textRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    if (oldFont != nullptr)
        ::SelectObject(hdc, oldFont);
}

// ---- 输入框底板（圆角描边 + 字段底色） ----

void DrawField(HDC hdc, const RECT& rc, const Part& part)
{
    // 圆角外四角铺白色卡片底（输入框底板都落在卡片内），框内用底板自己的表面色
    ::FillRect(hdc, &rc, GetSurfaceBrush(Design::kCardBack));

    Gfx::FillRoundRect(hdc, rc, Design::kRadiusButton, part.surface);
    Gfx::DrawRoundRect(hdc, rc, Design::kRadiusButton, Design::kFieldBorder);
}

// ---- 圆角面板（分区底色，无标题） ----

void DrawPanel(HDC hdc, const RECT& rc, const Part& part)
{
    // 圆角之外的四角铺页面底色（已抑制默认擦除，必须自己铺满整块矩形）
    ::FillRect(hdc, &rc, GetSurfaceBrush(Backdrop(part)));
    Gfx::FillRoundRect(hdc, rc, part.radius, part.surface);

    // 1px 清晰描边：只填色不描边时，白底面板与页面底色几乎连成一片，层级消失
    if (part.border != CLR_INVALID)
        Gfx::DrawRoundRect(hdc, rc, part.radius, part.border);
}

// ---- 日志框（深色圆角底 + 顶部实时条） ----

void DrawLogBox(HDC hdc, const RECT& rc, const Part& part)
{
    // 圆角之外铺卡片表面（卡片是白面）
    ::FillRect(hdc, &rc, GetSurfaceBrush(part.surface));

    Gfx::FillRoundRect(hdc, rc, Design::kRadiusButton, Design::kLogBack);

    // 实时条：上圆角、下平边（下沿与日志框底色相接）
    RECT strip = { rc.left, rc.top, rc.right, rc.top + Design::kLogStripHeight };
    Gfx::FillRoundRect(hdc, strip, Design::kRadiusButton, Design::kLogHeader);
    RECT stripLower = { rc.left, rc.top + 6, rc.right, rc.top + Design::kLogStripHeight };
    ::FillRect(hdc, &stripLower, GetSurfaceBrush(Design::kLogHeader));

    RECT dot = { rc.left + 10, rc.top + 9, rc.left + 16, rc.top + 15 };
    Gfx::FillEllipse(hdc, dot, Design::kLogDot);

    ::SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ oldFont = ::SelectObject(hdc, GetFont(L"微软雅黑", 8, false));

    ::SetTextColor(hdc, Design::kLogStripText);
    RECT titleRect = { rc.left + 24, rc.top, rc.left + 220, rc.top + Design::kLogStripHeight };
    ::DrawTextW(hdc, L"实时输出", -1, &titleRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    auto noteIt = g_logNotes.find(part.hwnd);
    const wchar_t* note = (noteIt != g_logNotes.end() && !noteIt->second.IsEmpty())
                              ? (const wchar_t*)noteIt->second
                              : L"自动刷新";

    ::SetTextColor(hdc, Design::kLogStripNote);
    RECT noteRect = { rc.right - 300, rc.top, rc.right - 10, rc.top + Design::kLogStripHeight };
    ::DrawTextW(hdc, note, -1, &noteRect,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    ::SelectObject(hdc, oldFont);
}

} // namespace

HFONT GetFont(const wchar_t* family, int pointSize, bool bold)
{
    FontKey key;
    key.family = family;
    key.points = pointSize;
    key.bold = bold;

    auto it = g_fontCache.find(key);
    if (it != g_fontCache.end())
        return it->second;

    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));

    // WinForms Font 使用点（point），按 GDI 的 -MulDiv(pt, dpi, 72) 换算逻辑高度
    lf.lfHeight = -::MulDiv(pointSize, GetScreenDpiY(), 72);
    lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;

    wcsncpy_s(lf.lfFaceName, family, _TRUNCATE);

    HFONT font = ::CreateFontIndirectW(&lf);
    g_fontCache[key] = font;
    return font;
}

HFONT GetDefaultFont()
{
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);

    // WinForms 的 SystemFonts.DefaultFont 即系统消息字体
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
    {
        FontKey key;
        key.family = ncm.lfMessageFont.lfFaceName;
        key.points = -1;    // 字号由系统决定，单独占一个缓存槽
        key.bold = false;

        auto it = g_fontCache.find(key);
        if (it != g_fontCache.end())
            return it->second;

        HFONT font = ::CreateFontIndirectW(&ncm.lfMessageFont);
        g_fontCache[key] = font;
        return font;
    }

    return GetFont(L"MS Shell Dlg", 9, false);
}

void ApplyFont(CWnd* wnd, HFONT font)
{
    if (wnd != nullptr && wnd->GetSafeHwnd() != nullptr && font != nullptr)
        wnd->SendMessage(WM_SETFONT, (WPARAM)font, MAKELPARAM(TRUE, 0));
}

void MakeFlatButton(CWnd* parent, UINT id, COLORREF back, COLORREF fore, Gfx::Icon icon,
                    COLORREF surface, COLORREF backdrop){
    Part part;
    part.kind = Part::Kind::Button;
    part.back = back;
    part.fore = fore;
    part.icon = icon;
    part.surface = surface;
    part.backdrop = backdrop;
    RegisterPart(parent, id, part);
}

void MakeOutlineButton(CWnd* parent, UINT id, COLORREF accent, Gfx::Icon icon,
                       COLORREF surface, COLORREF border, COLORREF backdrop)
{
    Part part;
    part.kind = Part::Kind::Button;
    part.back = surface;
    part.fore = accent;
    part.icon = icon;
    part.surface = surface;
    part.border = border;
    part.backdrop = backdrop;
    part.outline = true;
    RegisterPart(parent, id, part);
}

void MakeBadge(CWnd* parent, UINT id, Gfx::Icon icon, COLORREF surface, COLORREF accent,
               int radius)
{
    if (parent == nullptr)
        return;

    CWnd* badge = parent->GetDlgItem(id);
    if (badge == nullptr || badge->GetSafeHwnd() == nullptr)
        return;

    LONG style = ::GetWindowLongW(badge->GetSafeHwnd(), GWL_STYLE);
    style |= SS_OWNERDRAW;
    ::SetWindowLongW(badge->GetSafeHwnd(), GWL_STYLE, style);

    // 徽标是上层控件：不加 WS_CLIPSIBLINGS，否则更新区域会被裁掉画不出来
    Part part;
    part.kind = Part::Kind::Badge;
    part.icon = icon;
    part.surface = surface;
    part.accent = accent;
    part.radius = radius;
    RegisterPart(parent, id, part);
}

// ---- 分段控件（原型 .seg：浅蓝分组 + 选中项白底主色描边主色文字） ----

// 计算条目宽度（文字宽 + 左右各 16 内边距）与整组宽度（左右各 4 内边距 + 条目间距 4）
void MeasureSeg(HWND seg, HFONT font, const SegData& data, std::vector<int>& widths, int& total)
{
    widths.clear();
    total = Design::kSegPad * 2;

    for (size_t i = 0; i < data.items.size(); ++i)
    {
        const int width = MeasureTextWidth(seg, font, data.items[i].c_str()) + 32;
        widths.push_back(width);
        total += width;
    }

    if (!widths.empty())
        total += (int)(widths.size() - 1) * Design::kSegGap;
}

void DrawSeg(HDC hdc, const RECT& rc, const Part& part, HFONT font)
{
    auto dataIt = g_segs.find(part.hwnd);
    if (dataIt == g_segs.end())
        return;

    const SegData& data = dataIt->second;

    // 控件之外铺所在页面底色，再画分组底
    ::FillRect(hdc, &rc, GetSurfaceBrush(Design::kPageBack));
    Gfx::FillRoundRect(hdc, rc, Design::kRadiusButton, Design::kAccentSoft);

    std::vector<int> widths;
    int total = 0;
    MeasureSeg(part.hwnd, font, data, widths, total);

    HGDIOBJ oldFont = nullptr;
    if (font != nullptr)
        oldFont = ::SelectObject(hdc, font);

    ::SetBkMode(hdc, TRANSPARENT);

    int x = rc.left + Design::kSegPad;
    for (size_t i = 0; i < data.items.size(); ++i)
    {
        RECT item = { x, rc.top + Design::kSegPad,
                      x + widths[i], rc.top + Design::kSegPad + Design::kSegItemHeight };

        if ((int)i == data.selected)
        {
            Gfx::FillRoundRect(hdc, item, Design::kRadiusSegItem, Design::kCardBack);
            Gfx::DrawRoundRect(hdc, item, Design::kRadiusSegItem, Design::kAccent);
        }

        ::SetTextColor(hdc, ((int)i == data.selected) ? Design::kAccent : Design::kTextMuted);
        ::DrawTextW(hdc, data.items[i].c_str(), -1, &item,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

        x += widths[i] + Design::kSegGap;
    }

    if (oldFont != nullptr)
        ::SelectObject(hdc, oldFont);
}

void MakeSeg(CWnd* parent, UINT id, const std::vector<std::wstring>& items, int selected,
             COLORREF surface)
{
    if (parent == nullptr)
        return;

    CWnd* seg = parent->GetDlgItem(id);
    if (seg == nullptr || seg->GetSafeHwnd() == nullptr)
        return;

    LONG style = ::GetWindowLongW(seg->GetSafeHwnd(), GWL_STYLE);
    style |= SS_OWNERDRAW;
    ::SetWindowLongW(seg->GetSafeHwnd(), GWL_STYLE, style);

    SegData data;
    data.items = items;
    data.selected = selected;
    g_segs[seg->GetSafeHwnd()] = data;

    Part part;
    part.kind = Part::Kind::Seg;
    part.surface = surface;
    RegisterPart(parent, id, part);
}

void SetSegSelection(CWnd* parent, UINT id, int selected)
{
    if (parent == nullptr)
        return;

    CWnd* seg = parent->GetDlgItem(id);
    if (seg == nullptr || seg->GetSafeHwnd() == nullptr)
        return;

    auto it = g_segs.find(seg->GetSafeHwnd());
    if (it == g_segs.end() || it->second.selected == selected)
        return;

    it->second.selected = selected;
    ::InvalidateRect(seg->GetSafeHwnd(), nullptr, FALSE);
}

void SetSegItemText(CWnd* parent, UINT id, int index, const wchar_t* text)
{
    if (parent == nullptr)
        return;

    CWnd* seg = parent->GetDlgItem(id);
    if (seg == nullptr || seg->GetSafeHwnd() == nullptr)
        return;

    auto it = g_segs.find(seg->GetSafeHwnd());
    if (it == g_segs.end() || index < 0 || index >= (int)it->second.items.size())
        return;

    it->second.items[index] = text;
    ::InvalidateRect(seg->GetSafeHwnd(), nullptr, FALSE);
}

int MeasureSegWidth(CWnd* parent, UINT id)
{
    if (parent == nullptr)
        return 0;

    CWnd* seg = parent->GetDlgItem(id);
    if (seg == nullptr || seg->GetSafeHwnd() == nullptr)
        return 0;

    auto it = g_segs.find(seg->GetSafeHwnd());
    if (it == g_segs.end())
        return 0;

    std::vector<int> widths;
    int total = 0;
    MeasureSeg(seg->GetSafeHwnd(),
               (HFONT)::SendMessageW(seg->GetSafeHwnd(), WM_GETFONT, 0, 0),
               it->second, widths, total);
    return total;
}

// 背景层窗口（输入框底板 / 日志框底板）加 WS_CLIPSIBLINGS：底板位于 Edit、微调、日志编辑框之下，
// 加了这个样式后底板重绘时会把自己的更新区域裁掉被上层兄弟遮住的部分，
// 因此绝不会把上层控件的像素擦掉（否则输入框文字、微调按钮会「时有时无」甚至整块消失）。
// 注意：上层控件（按钮、标签、胶囊、Edit）**绝不能**加，否则它与背景层重叠时更新区域会被裁掉，
// 反而整个控件都画不出来。
void AddClipsiblings(HWND hwnd)
{
    if (hwnd == nullptr)
        return;

    LONG style = ::GetWindowLongW(hwnd, GWL_STYLE);
    if ((style & WS_CLIPSIBLINGS) == 0)
        ::SetWindowLongW(hwnd, GWL_STYLE, style | WS_CLIPSIBLINGS);
}

void MakeField(CWnd* parent, UINT id, COLORREF surface)
{
    if (parent == nullptr)
        return;

    CWnd* field = parent->GetDlgItem(id);
    if (field == nullptr || field->GetSafeHwnd() == nullptr)
        return;

    LONG style = ::GetWindowLongW(field->GetSafeHwnd(), GWL_STYLE);
    style |= SS_OWNERDRAW;
    ::SetWindowLongW(field->GetSafeHwnd(), GWL_STYLE, style);
    AddClipsiblings(field->GetSafeHwnd());

    Part part;
    part.kind = Part::Kind::Field;
    part.surface = surface;
    RegisterPart(parent, id, part);
}

// 卡片外观（阴影 + 圆角白底 + 描边 + 标题行）：卡片是纯装饰层，直接画在所在对话框的 DC 上。
// 教训：曾用 SS_OWNERDRAW 子窗口当卡片，再 SetWindowPos(HWND_BOTTOM) 把它下压 —— 结果卡片反而
// 排到了卡内控件之上，把卡内所有控件的点击全部吃掉（界面看着正常但点不动）。
// 结论：卡片 / 面板这类装饰层一律由父窗口 OnPaint 绘制，绝不建子窗口。
void DrawCardFace(HDC hdc, const RECT& rc, const wchar_t* title, HFONT font, COLORREF surface)
{
    const bool dark = IsDark(surface);

    Gfx::DrawSoftShadow(hdc, rc, Design::kRadiusCard, 6, 1, 20, RGB(15, 23, 42));
    Gfx::FillRoundRect(hdc, rc, Design::kRadiusCard, surface);
    Gfx::DrawRoundRect(hdc, rc, Design::kRadiusCard,
                       dark ? Lighten(surface, 10) : Design::kCardBorder);

    if (title == nullptr || title[0] == 0)
        return;

    const int pad = Design::kCardPad;
    const int headTop = rc.top + pad;

    RECT barRect = { rc.left + pad, headTop + (Design::kCardHeadHeight - 14) / 2,
                     rc.left + pad + 3, headTop + (Design::kCardHeadHeight - 14) / 2 + 14 };
    Gfx::FillRoundRect(hdc, barRect, 2, Design::kAccent);

    HGDIOBJ oldFont = nullptr;
    if (font != nullptr)
        oldFont = ::SelectObject(hdc, font);

    ::SetBkMode(hdc, TRANSPARENT);
    ::SetTextColor(hdc, dark ? Design::kTextOnBrand : Design::kTextTitle);

    RECT textRect = rc;
    textRect.left = rc.left + pad + 12;
    textRect.right = rc.right - pad;
    textRect.top = headTop;
    textRect.bottom = headTop + Design::kCardHeadHeight;
    ::DrawTextW(hdc, title, -1, &textRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    if (oldFont != nullptr)
        ::SelectObject(hdc, oldFont);
}

void MakePill(CWnd* parent, UINT id, COLORREF surface, COLORREF backdrop)
{
    if (parent == nullptr)
        return;

    CWnd* pill = parent->GetDlgItem(id);
    if (pill == nullptr || pill->GetSafeHwnd() == nullptr)
        return;

    LONG style = ::GetWindowLongW(pill->GetSafeHwnd(), GWL_STYLE);
    style |= SS_OWNERDRAW;
    ::SetWindowLongW(pill->GetSafeHwnd(), GWL_STYLE, style);

    Part part;
    part.kind = Part::Kind::Pill;
    part.surface = surface;
    part.backdrop = backdrop;
    RegisterPart(parent, id, part);
}

int MeasurePillWidth(HWND pill, HFONT font)
{
    // 与 DrawPill 一致：文字从左侧 22px 开始，右侧留 8px
    const int padding = 30;

    int textWidth = 0;
    if (pill != nullptr)
    {
        wchar_t text[128] = { 0 };
        ::GetWindowTextW(pill, text, _countof(text));
        textWidth = MeasureTextWidth(pill, font, text);
    }

    const int width = textWidth + padding;
    return width < Design::kPillMinWidth ? Design::kPillMinWidth : width;
}

int MeasureTextWidth(HWND wnd, HFONT font, const wchar_t* text)
{
    if (wnd == nullptr || text == nullptr || text[0] == 0)
        return 0;

    HDC hdc = ::GetDC(wnd);
    HGDIOBJ oldFont = nullptr;
    if (font != nullptr)
        oldFont = ::SelectObject(hdc, font);

    SIZE size = { 0, 0 };
    ::GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &size);

    if (oldFont != nullptr)
        ::SelectObject(hdc, oldFont);
    ::ReleaseDC(wnd, hdc);

    return size.cx;
}

void MakeLogBox(CWnd* parent, UINT id, COLORREF surface)
{    if (parent == nullptr)
        return;

    CWnd* box = parent->GetDlgItem(id);
    if (box == nullptr || box->GetSafeHwnd() == nullptr)
        return;

    LONG style = ::GetWindowLongW(box->GetSafeHwnd(), GWL_STYLE);
    style |= SS_OWNERDRAW;
    ::SetWindowLongW(box->GetSafeHwnd(), GWL_STYLE, style);
    AddClipsiblings(box->GetSafeHwnd());

    Part part;
    part.kind = Part::Kind::LogBox;
    part.surface = surface;
    RegisterPart(parent, id, part);
}

void SetLogBoxNote(HWND box, const wchar_t* note)
{
    if (box == nullptr)
        return;

    CString text = (note != nullptr) ? note : L"";
    auto it = g_logNotes.find(box);
    if (it != g_logNotes.end() && it->second == text)
        return;

    g_logNotes[box] = text;
    ::InvalidateRect(box, nullptr, FALSE);
}

bool DrawPart(const DRAWITEMSTRUCT* dis)
{
    if (dis == nullptr)
        return false;

    auto it = g_parts.find(dis->hwndItem);
    if (it == g_parts.end())
        return false;

    const Part& part = it->second;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    wchar_t text[256] = { 0 };
    ::GetWindowTextW(dis->hwndItem, text, _countof(text));

    HFONT font = (HFONT)::SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0);

    switch (part.kind)
    {
    case Part::Kind::Card:
        DrawCard(dis->hDC, dis->rcItem, part, text, font);
        break;
    case Part::Kind::Pill:
        DrawPill(dis->hDC, dis->rcItem, part, text, font);
        break;
    case Part::Kind::Field:
        DrawField(dis->hDC, dis->rcItem, part);
        break;
    case Part::Kind::Panel:
        DrawPanel(dis->hDC, dis->rcItem, part);
        break;
    case Part::Kind::LogBox:
        DrawLogBox(dis->hDC, dis->rcItem, part);
        break;
    case Part::Kind::Badge:
        DrawBadge(dis->hDC, dis->rcItem, part, text, font);
        break;
    case Part::Kind::Seg:
        DrawSeg(dis->hDC, dis->rcItem, part,
                (HFONT)::SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0));
        break;
    default:
        DrawButton(dis->hDC, dis->rcItem, part, text, font, disabled, pressed);
        break;
    }

    return true;
}

bool DrawFlatButton(const DRAWITEMSTRUCT* dis)
{
    return DrawPart(dis);
}

void SetFlatButtonColor(CWnd* parent, UINT id, COLORREF back, COLORREF fore)
{
    if (parent == nullptr)
        return;

    CWnd* button = parent->GetDlgItem(id);
    if (button == nullptr || button->GetSafeHwnd() == nullptr)
        return;

    auto it = g_parts.find(button->GetSafeHwnd());
    if (it == g_parts.end())
        return;

    // 配色没变就不重绘（状态轮询每几秒会调一次，重复重绘会造成可见闪烁）
    if (it->second.back == back && it->second.fore == fore)
        return;

    it->second.back = back;
    it->second.fore = fore;
    ::InvalidateRect(button->GetSafeHwnd(), nullptr, FALSE);
}

void DrawCardShadows(HDC hdc, HWND owner)
{
    HWND ownerParent = (owner != nullptr) ? ::GetParent(owner) : nullptr;

    for (const auto& entry : g_parts)
    {
        const Part& part = entry.second;
        const bool isCard = (part.kind == Part::Kind::Card);
        const bool isPanel = (part.kind == Part::Kind::Panel);
        if (!isCard && !isPanel)
            continue;

        // 隐藏的卡片/面板（例如未选中的页签页）不能画阴影，否则页面上会浮出一块影子
        if (!::IsWindowVisible(entry.first))
            continue;

        HWND parent = ::GetParent(entry.first);
        if (parent == nullptr)
            continue;

        // 指定 owner 时只画它的兄弟部件（页签页里的面板就是页签的兄弟窗口）
        if (ownerParent != nullptr && parent != ownerParent)
            continue;

        const HWND frame = (owner != nullptr) ? owner : parent;

        RECT rc;
        ::GetWindowRect(entry.first, &rc);
        POINT origin = { 0, 0 };
        ::ClientToScreen(frame, &origin);
        ::OffsetRect(&rc, -origin.x, -origin.y);

        // 阴影落在卡片/面板矩形之外，只能画在父窗口 DC 上（画在自己的 DC 里会被裁剪）
        if (isPanel)
            Gfx::DrawSoftShadow(hdc, rc, part.radius, 7, 1, 26, RGB(15, 23, 42));
        else
            Gfx::DrawSoftShadow(hdc, rc, Design::kRadiusCard, 6, 1, 20, RGB(15, 23, 42));
    }
}

void RegisterSurface(const RECT& cardRect, COLORREF surface, HWND parent)
{
    SurfaceEntry entry;
    entry.parent = parent;
    entry.rect = cardRect;
    entry.color = surface;
    g_surfaces.push_back(entry);
}

void ClearSurfaces(HWND parent)
{
    if (parent == nullptr)
    {
        g_surfaces.clear();
        return;
    }

    for (size_t i = g_surfaces.size(); i-- > 0; )
    {
        if (g_surfaces[i].parent == parent)
            g_surfaces.erase(g_surfaces.begin() + i);
    }
}

bool TryGetSurfaceColor(HWND control, COLORREF& surface)
{
    if (control == nullptr || g_surfaces.empty())
        return false;

    HWND parent = ::GetParent(control);
    if (parent == nullptr)
        return false;

    RECT rc;
    ::GetWindowRect(control, &rc);
    POINT origin = { 0, 0 };
    ::ClientToScreen(parent, &origin);
    ::OffsetRect(&rc, -origin.x, -origin.y);

    // 从后往前匹配（后注册的卡片优先）
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;

    for (size_t i = g_surfaces.size(); i-- > 0; )
    {
        // 只认本对话框的登记（未限定对话框的登记仍然通用）
        if (g_surfaces[i].parent != nullptr && g_surfaces[i].parent != parent)
            continue;

        const RECT& card = g_surfaces[i].rect;
        if (cx >= card.left && cx < card.right && cy >= card.top && cy < card.bottom)
        {
            surface = g_surfaces[i].color;
            return true;
        }
    }

    return false;
}

HBRUSH GetSurfaceBrush(COLORREF surface)
{
    auto it = g_surfaceBrushes.find(surface);
    if (it != g_surfaceBrushes.end())
        return it->second;

    HBRUSH brush = ::CreateSolidBrush(surface);
    g_surfaceBrushes[surface] = brush;
    return brush;
}

void SetOutlineButtonColor(CWnd* parent, UINT id, COLORREF ink, COLORREF border)
{
    if (parent == nullptr)
        return;

    CWnd* button = parent->GetDlgItem(id);
    if (button == nullptr || button->GetSafeHwnd() == nullptr)
        return;

    auto it = g_parts.find(button->GetSafeHwnd());
    if (it == g_parts.end() || !it->second.outline)
        return;

    // 配色没变就不重绘（状态轮询每几秒会调一次，重复重绘会造成可见闪烁）
    if (it->second.fore == ink && it->second.border == border)
        return;

    it->second.fore = ink;
    it->second.border = border;
    ::InvalidateRect(button->GetSafeHwnd(), nullptr, FALSE);
}

void SetButtonText(CWnd* parent, UINT id, const wchar_t* text)
{
    if (parent == nullptr)
        return;

    CWnd* button = parent->GetDlgItem(id);
    if (button == nullptr || button->GetSafeHwnd() == nullptr)
        return;

    // 文字没变就不设（状态轮询每几秒会调一次，重复 SetWindowText 会闪烁）
    CString current;
    button->GetWindowTextW(current);
    if (text != nullptr && current == text)
        return;

    button->SetWindowTextW(text);
}

void SetTextColor(HWND control, COLORREF color)
{
    if (control == nullptr)
        return;

    // 颜色没变就不重绘（状态轮询每几秒会调一次）
    auto it = g_textColors.find(control);
    if (it != g_textColors.end() && it->second == color)
        return;

    g_textColors[control] = color;
    ::InvalidateRect(control, nullptr, FALSE);
}

bool TryGetTextColor(HWND control, COLORREF& color)
{
    auto it = g_textColors.find(control);
    if (it == g_textColors.end())
        return false;

    color = it->second;
    return true;
}

void SetLabelText(CWnd* parent, UINT id, const wchar_t* text)
{
    if (parent == nullptr)
        return;

    CWnd* label = parent->GetDlgItem(id);
    if (label == nullptr || label->GetSafeHwnd() == nullptr)
        return;

    // 文字没变就不设：状态轮询每几秒会调一次，重复重绘会导致文字闪一下
    CString current;
    label->GetWindowTextW(current);
    if (text != nullptr && current == text)
        return;

    label->SetWindowTextW(text);
    label->Invalidate(FALSE);
}

} // namespace Ui
} // namespace ps

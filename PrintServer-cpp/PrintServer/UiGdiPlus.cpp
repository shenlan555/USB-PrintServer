#include "pch.h"
#include "UiGdiPlus.h"

#include <gdiplus.h>
#include <algorithm>

#pragma comment(lib, "gdiplus.lib")

namespace ps
{
namespace Ui
{
namespace Gfx
{

namespace
{

ULONG_PTR g_token = 0;
bool g_initialized = false;

using namespace Gdiplus;

// windows.h 定义了 min/max 宏，这里用自建模板避免 std::min/std::max 被宏破坏
template <typename T>
inline T MinVal(T a, T b) { return (a < b) ? a : b; }

template <typename T>
inline T MaxVal(T a, T b) { return (a > b) ? a : b; }

Color ToColor(COLORREF color, BYTE alpha)
{
    return Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

// 圆角矩形路径（GDI+ 没有内建 API，用四段圆弧拼接）
void BuildRoundRect(GraphicsPath& path, const RectF& rc, float radius)
{
    if (radius <= 0.5f)
    {
        path.AddRectangle(rc);
        return;
    }

    const float d = radius * 2.0f;
    if (d > rc.Width) radius = rc.Width / 2.0f;
    if (d > rc.Height) radius = rc.Height / 2.0f;
    const float dd = radius * 2.0f;

    path.AddArc(rc.X, rc.Y, dd, dd, 180.0f, 90.0f);
    path.AddArc(rc.GetRight() - dd, rc.Y, dd, dd, 270.0f, 90.0f);
    path.AddArc(rc.GetRight() - dd, rc.GetBottom() - dd, dd, dd, 0.0f, 90.0f);
    path.AddArc(rc.X, rc.GetBottom() - dd, dd, dd, 90.0f, 90.0f);
    path.CloseFigure();
}

RectF ToRectF(const RECT& rc)
{
    return RectF((REAL)rc.left, (REAL)rc.top,
                 (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top));
}

// 图标坐标系：16x16 归一化网格 → 目标矩形
inline REAL NX(const RectF& rc, REAL x) { return rc.X + x * rc.Width / 16.0f; }
inline REAL NY(const RectF& rc, REAL y) { return rc.Y + y * rc.Height / 16.0f; }
inline REAL NW(const RectF& rc, REAL w) { return w * MinVal(rc.Width, rc.Height) / 16.0f; }

PointF P(const RectF& rc, REAL x, REAL y) { return PointF(NX(rc, x), NY(rc, y)); }

void ConfigurePen(Pen& pen, const RectF& rc, REAL stroke)
{
    const REAL scale = MinVal(rc.Width, rc.Height) / 16.0f;
    pen.SetWidth(MaxVal(1.0f, stroke * scale));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
}

} // namespace

bool Initialize()
{
    if (g_initialized)
        return true;

    GdiplusStartupInput input;
    if (GdiplusStartup(&g_token, &input, nullptr) != Ok)
        return false;

    g_initialized = true;
    return true;
}

void FillRoundRect(HDC hdc, const RECT& rc, int radius, COLORREF color, BYTE alpha)
{
    if (!Initialize())
        return;

    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    GraphicsPath path;
    BuildRoundRect(path, ToRectF(rc), (REAL)radius);
    SolidBrush brush(ToColor(color, alpha));
    graphics.FillPath(&brush, &path);
}

void FillRoundRectGradient(HDC hdc, const RECT& rc, int radius,
                           COLORREF top, COLORREF bottom, BYTE alpha)
{
    if (!Initialize())
        return;

    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    GraphicsPath path;
    BuildRoundRect(path, ToRectF(rc), (REAL)radius);

    const RectF rf = ToRectF(rc);
    LinearGradientBrush brush(rf, ToColor(top, alpha), ToColor(bottom, alpha),
                              LinearGradientModeVertical);
    graphics.FillPath(&brush, &path);
}

void FillRoundRectGradientH(HDC hdc, const RECT& rc, int radius,
                            COLORREF left, COLORREF right, BYTE alpha)
{
    if (!Initialize())
        return;

    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    GraphicsPath path;
    BuildRoundRect(path, ToRectF(rc), (REAL)radius);

    const RectF rf = ToRectF(rc);
    LinearGradientBrush brush(rf, ToColor(left, alpha), ToColor(right, alpha),
                              LinearGradientModeHorizontal);
    graphics.FillPath(&brush, &path);
}

void DrawRoundRect(HDC hdc, const RECT& rc, int radius, COLORREF color,
                   float width, BYTE alpha)
{
    if (!Initialize())
        return;

    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    // 像素中心对齐到整数坐标：否则 1px 描边会以路径为中心各摊 0.5px 到相邻两像素上
    graphics.SetPixelOffsetMode(PixelOffsetModeHalf);

    RectF rf = ToRectF(rc);

    if (width <= 1.5f)
    {
        // 细描边：把路径整体内缩 0.5px，使整条线精确落在单个像素上，颜色不发虚
        rf.X += 0.5f;
        rf.Y += 0.5f;
        rf.Width -= 1.0f;
        rf.Height -= 1.0f;

        if (rf.Width < 1.0f)
            rf.Width = 1.0f;
        if (rf.Height < 1.0f)
            rf.Height = 1.0f;
    }

    GraphicsPath path;
    BuildRoundRect(path, rf, (REAL)radius);

    Pen pen(ToColor(color, alpha), MaxVal(1.0f, width));
    graphics.DrawPath(&pen, &path);
}

void FillEllipse(HDC hdc, const RECT& rc, COLORREF color, BYTE alpha)
{
    if (!Initialize())
        return;

    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    SolidBrush brush(ToColor(color, alpha));
    graphics.FillEllipse(&brush, ToRectF(rc));
}

void DrawSoftShadow(HDC hdc, const RECT& rc, int radius, int spread, int offsetY,
                    BYTE alpha, COLORREF color)
{
    if (!Initialize() || spread <= 0)
        return;

    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);

    const RECT base = { rc.left, rc.top + offsetY, rc.right, rc.bottom + offsetY };
    for (int i = spread; i >= 1; --i)
    {
        // 越靠外的层越淡（线性衰减），叠加后接近高斯阴影
        const BYTE layerAlpha = (BYTE)MaxVal(1, (int)alpha * (spread - i + 1) / (spread + 1));

        RECT layer = { base.left - i, base.top - i, base.right + i, base.bottom + i };
        GraphicsPath path;
        BuildRoundRect(path, ToRectF(layer), (REAL)(radius + i));
        SolidBrush brush(ToColor(color, layerAlpha));
        graphics.FillPath(&brush, &path);
    }
}

void DrawIcon(HDC hdc, Icon icon, const RECT& rc, COLORREF color, BYTE alpha)
{
    if (!Initialize())
        return;

    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHalf);

    const RectF box = ToRectF(rc);
    const Color ink = ToColor(color, alpha);
    Pen pen(ink);
    ConfigurePen(pen, box, 1.7f);
    SolidBrush brush(ink);

    GraphicsPath path;

    switch (icon)
    {
    case Icon::Printer:
        path.AddRectangle(RectF(NX(box, 4.0f), NY(box, 1.5f), NW(box, 8.0f), NW(box, 4.2f)));
        graphics.DrawPath(&pen, &path);
        path.Reset();
        BuildRoundRect(path, RectF(NX(box, 1.5f), NY(box, 5.6f), NW(box, 13.0f), NW(box, 6.6f)), 1.4f);
        graphics.DrawPath(&pen, &path);
        path.Reset();
        path.AddRectangle(RectF(NX(box, 4.0f), NY(box, 12.1f), NW(box, 8.0f), NW(box, 3.0f)));
        graphics.DrawPath(&pen, &path);
        graphics.FillEllipse(&brush, RectF(NX(box, 11.0f), NY(box, 8.0f), NW(box, 1.6f), NW(box, 1.6f)));
        break;

    case Icon::Gear:
        graphics.DrawEllipse(&pen, RectF(NX(box, 3.4f), NY(box, 3.4f), NW(box, 9.2f), NW(box, 9.2f)));
        graphics.DrawEllipse(&pen, RectF(NX(box, 6.2f), NY(box, 6.2f), NW(box, 3.6f), NW(box, 3.6f)));
        for (int i = 0; i < 8; ++i)
        {
            const REAL angle = (REAL)(i * 3.14159265358979 / 4.0);
            const REAL cx = 8.0f, cy = 8.0f;
            graphics.DrawLine(&pen, P(box, cx + 4.6f * cosf(angle), cy + 4.6f * sinf(angle)),
                                    P(box, cx + 6.4f * cosf(angle), cy + 6.4f * sinf(angle)));
        }
        break;

    case Icon::Signal:
        graphics.FillEllipse(&brush, RectF(NX(box, 6.8f), NY(box, 12.4f), NW(box, 2.4f), NW(box, 2.4f)));
        for (int i = 1; i <= 3; ++i)
        {
            const REAL r = 2.6f + i * 2.2f;
            GraphicsPath arc;
            arc.AddArc(NX(box, 8.0f) - NW(box, r), NY(box, 13.6f) - NW(box, r),
                       NW(box, r * 2.0f), NW(box, r * 2.0f), 210.0f, 120.0f);
            graphics.DrawPath(&pen, &arc);
        }
        break;

    case Icon::Wrench:
        graphics.DrawLine(&pen, P(box, 4.4f, 12.6f), P(box, 9.6f, 7.4f));
        {
            GraphicsPath head;
            head.AddArc(NX(box, 8.4f), NY(box, 1.4f), NW(box, 6.4f), NW(box, 6.4f), 40.0f, 280.0f);
            graphics.DrawPath(&pen, &head);
        }
        break;

    case Icon::Chart:
        graphics.DrawLine(&pen, P(box, 2.2f, 13.6f), P(box, 13.8f, 13.6f));
        graphics.DrawLine(&pen, P(box, 4.4f, 13.2f), P(box, 4.4f, 9.0f));
        graphics.DrawLine(&pen, P(box, 8.0f, 13.2f), P(box, 8.0f, 6.2f));
        graphics.DrawLine(&pen, P(box, 11.6f, 13.2f), P(box, 11.6f, 3.4f));
        break;

    case Icon::Document:
        BuildRoundRect(path, RectF(NX(box, 3.2f), NY(box, 1.8f), NW(box, 9.6f), NW(box, 12.4f)), 1.6f);
        graphics.DrawPath(&pen, &path);
        graphics.DrawLine(&pen, P(box, 5.6f, 6.0f), P(box, 10.4f, 6.0f));
        graphics.DrawLine(&pen, P(box, 5.6f, 8.6f), P(box, 10.4f, 8.6f));
        graphics.DrawLine(&pen, P(box, 5.6f, 11.2f), P(box, 8.8f, 11.2f));
        break;

    case Icon::Upload:
        graphics.DrawLine(&pen, P(box, 8.0f, 12.4f), P(box, 8.0f, 3.2f));
        graphics.DrawLine(&pen, P(box, 4.6f, 6.6f), P(box, 8.0f, 3.2f));
        graphics.DrawLine(&pen, P(box, 11.4f, 6.6f), P(box, 8.0f, 3.2f));
        graphics.DrawLine(&pen, P(box, 3.4f, 11.2f), P(box, 3.4f, 13.8f));
        graphics.DrawLine(&pen, P(box, 3.4f, 13.8f), P(box, 12.6f, 13.8f));
        graphics.DrawLine(&pen, P(box, 12.6f, 13.8f), P(box, 12.6f, 11.2f));
        break;

    case Icon::Download:
        graphics.DrawLine(&pen, P(box, 8.0f, 2.8f), P(box, 8.0f, 11.6f));
        graphics.DrawLine(&pen, P(box, 4.6f, 8.2f), P(box, 8.0f, 11.6f));
        graphics.DrawLine(&pen, P(box, 11.4f, 8.2f), P(box, 8.0f, 11.6f));
        graphics.DrawLine(&pen, P(box, 3.4f, 12.4f), P(box, 3.4f, 13.8f));
        graphics.DrawLine(&pen, P(box, 3.4f, 13.8f), P(box, 12.6f, 13.8f));
        graphics.DrawLine(&pen, P(box, 12.6f, 13.8f), P(box, 12.6f, 12.4f));
        break;

    case Icon::Search:
        graphics.DrawEllipse(&pen, RectF(NX(box, 2.6f), NY(box, 2.6f), NW(box, 8.6f), NW(box, 8.6f)));
        graphics.DrawLine(&pen, P(box, 10.2f, 10.2f), P(box, 13.6f, 13.6f));
        break;

    case Icon::Chat:
        BuildRoundRect(path, RectF(NX(box, 1.8f), NY(box, 2.6f), NW(box, 12.4f), NW(box, 8.8f)), 2.4f);
        graphics.DrawPath(&pen, &path);
        path.Reset();
        path.AddLine(P(box, 4.6f, 11.2f), P(box, 4.6f, 14.0f));
        path.AddLine(P(box, 4.6f, 14.0f), P(box, 8.2f, 11.4f));
        graphics.DrawPath(&pen, &path);
        for (int i = 0; i < 3; ++i)
            graphics.FillEllipse(&brush, RectF(NX(box, 5.2f + i * 2.6f), NY(box, 6.4f), NW(box, 1.5f), NW(box, 1.5f)));
        break;

    case Icon::Refresh:
        {
            GraphicsPath arc;
            arc.AddArc(NX(box, 2.6f), NY(box, 2.6f), NW(box, 10.8f), NW(box, 10.8f), 40.0f, 280.0f);
            graphics.DrawPath(&pen, &arc);
            path.AddLine(P(box, 10.6f, 1.6f), P(box, 12.9f, 5.6f));
            path.AddLine(P(box, 12.9f, 5.6f), P(box, 8.4f, 5.2f));
            graphics.DrawPath(&pen, &path);
        }
        break;

    case Icon::Trash:
        graphics.DrawLine(&pen, P(box, 2.6f, 4.4f), P(box, 13.4f, 4.4f));
        graphics.DrawLine(&pen, P(box, 6.4f, 2.2f), P(box, 9.6f, 2.2f));
        path.AddLine(P(box, 4.4f, 4.6f), P(box, 5.4f, 13.8f));
        path.AddLine(P(box, 5.4f, 13.8f), P(box, 10.6f, 13.8f));
        path.AddLine(P(box, 10.6f, 13.8f), P(box, 11.6f, 4.6f));
        graphics.DrawPath(&pen, &path);
        graphics.DrawLine(&pen, P(box, 6.8f, 6.8f), P(box, 7.1f, 11.6f));
        graphics.DrawLine(&pen, P(box, 9.2f, 6.8f), P(box, 8.9f, 11.6f));
        break;

    case Icon::Check:
        path.AddLine(P(box, 3.2f, 8.4f), P(box, 6.6f, 11.8f));
        path.AddLine(P(box, 6.6f, 11.8f), P(box, 12.8f, 4.4f));
        graphics.DrawPath(&pen, &path);
        break;

    case Icon::Copy:
        BuildRoundRect(path, RectF(NX(box, 2.4f), NY(box, 2.4f), NW(box, 7.6f), NW(box, 7.6f)), 1.4f);
        graphics.DrawPath(&pen, &path);
        path.Reset();
        BuildRoundRect(path, RectF(NX(box, 6.4f), NY(box, 6.4f), NW(box, 7.6f), NW(box, 7.6f)), 1.4f);
        graphics.DrawPath(&pen, &path);
        break;

    case Icon::Shield:
        path.AddLine(P(box, 8.0f, 1.8f), P(box, 13.4f, 3.8f));
        path.AddLine(P(box, 13.4f, 3.8f), P(box, 13.4f, 8.4f));
        path.AddBezier(P(box, 13.4f, 8.4f), P(box, 13.4f, 11.6f), P(box, 10.8f, 13.4f), P(box, 8.0f, 14.4f));
        path.AddBezier(P(box, 8.0f, 14.4f), P(box, 5.2f, 13.4f), P(box, 2.6f, 11.6f), P(box, 2.6f, 8.4f));
        path.AddLine(P(box, 2.6f, 8.4f), P(box, 2.6f, 3.8f));
        path.CloseFigure();
        graphics.DrawPath(&pen, &path);
        break;

    case Icon::Clock:
        graphics.DrawEllipse(&pen, RectF(NX(box, 2.4f), NY(box, 2.4f), NW(box, 11.2f), NW(box, 11.2f)));
        graphics.DrawLine(&pen, P(box, 8.0f, 8.0f), P(box, 8.0f, 4.6f));
        graphics.DrawLine(&pen, P(box, 8.0f, 8.0f), P(box, 10.8f, 9.4f));
        break;

    case Icon::Warning:
        path.AddLine(P(box, 8.0f, 2.2f), P(box, 14.6f, 13.6f));
        path.AddLine(P(box, 14.6f, 13.6f), P(box, 1.4f, 13.6f));
        path.CloseFigure();
        graphics.DrawPath(&pen, &path);
        graphics.DrawLine(&pen, P(box, 8.0f, 6.2f), P(box, 8.0f, 9.6f));
        graphics.FillEllipse(&brush, RectF(NX(box, 7.3f), NY(box, 11.0f), NW(box, 1.5f), NW(box, 1.5f)));
        break;

    case Icon::Info:
        graphics.DrawEllipse(&pen, RectF(NX(box, 2.4f), NY(box, 2.4f), NW(box, 11.2f), NW(box, 11.2f)));
        graphics.FillEllipse(&brush, RectF(NX(box, 7.4f), NY(box, 4.6f), NW(box, 1.4f), NW(box, 1.4f)));
        graphics.DrawLine(&pen, P(box, 8.0f, 7.6f), P(box, 8.0f, 11.4f));
        break;

    case Icon::Play:
        path.AddLine(P(box, 4.6f, 3.0f), P(box, 12.6f, 8.0f));
        path.AddLine(P(box, 12.6f, 8.0f), P(box, 4.6f, 13.0f));
        path.CloseFigure();
        graphics.FillPath(&brush, &path);
        break;

    case Icon::Stop:
        BuildRoundRect(path, RectF(NX(box, 4.0f), NY(box, 4.0f), NW(box, 8.0f), NW(box, 8.0f)), 1.6f);
        graphics.FillPath(&brush, &path);
        break;

    case Icon::Key:
        graphics.DrawEllipse(&pen, RectF(NX(box, 1.8f), NY(box, 8.2f), NW(box, 6.4f), NW(box, 6.4f)));
        graphics.DrawLine(&pen, P(box, 6.8f, 9.2f), P(box, 13.6f, 2.8f));
        graphics.DrawLine(&pen, P(box, 11.0f, 5.2f), P(box, 12.8f, 6.8f));
        graphics.DrawLine(&pen, P(box, 9.2f, 6.8f), P(box, 11.0f, 8.4f));
        break;
    }
}

void DrawIconCentered(HDC hdc, Icon icon, POINT center, int size, COLORREF color, BYTE alpha)
{
    const int half = size / 2;
    RECT rc = { center.x - half, center.y - half, center.x - half + size, center.y - half + size };
    DrawIcon(hdc, icon, rc, color, alpha);
}

} // namespace Gfx
} // namespace Ui
} // namespace ps

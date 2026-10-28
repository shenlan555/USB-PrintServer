#pragma once

// 复用的自绘控件
//
// SysTabControl32 不会自行填充页面区域背景，WinForms 的 TabPage 背景是
// SystemColors.Control，因此这里用同色重绘保持观感一致。
// 系统主题会把页体画成白色，故 WM_PAINT 里在控件自绘之后再按设定底色重铺显示区，
// 并补画页内卡片/面板的柔和阴影（阴影落在卡片之外，会被本控件盖住）。
//
// CNavBar 为界面自有设计元素：左侧浅蓝白导航栏（顶部实底品牌区 + 导航项 + 本机 IP
// 信息块），全部矢量自绘，不使用系统按钮，也不引入任何第三方资源。

#include <afxwin.h>

namespace ps
{
namespace Ui
{

// 导航项选中通知：wParam = 项序号（0 服务器 / 1 设备列表 / 2 运行日志 / 3 授权信息）
const UINT kNavSelectionChanged = WM_APP + 60;

class CColorTab : public CWnd
{
    DECLARE_DYNAMIC(CColorTab)

public:
    CColorTab();

    void SetBackColor(COLORREF color) { m_backColor = color; }

protected:
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnPaint();
    DECLARE_MESSAGE_MAP()

    // MFC 默认实现把"被替换掉的原始 WNDPROC"存在一个共享静态变量里：
    // 同一进程内若有多个 CWnd 派生对象 SubclassWindow 不同窗口类，会相互冲掉。
    // 按 MFC 的要求为派生类提供独立存储，避免该隐患。
    virtual WNDPROC* GetSuperWndProcAddr() { return &m_superProc; }

private:
    COLORREF m_backColor;
    WNDPROC m_superProc;
};

// ===== 左侧导航栏（主界面自有设计元素）=====
class CNavBar : public CWnd
{
    DECLARE_DYNAMIC(CNavBar)

public:
    enum
    {
        kWidth = 148,       // 导航栏宽度（客户区左侧整条）
        kBrandHeight = 64,  // 顶部品牌区高度
        kItemHeight = 36,
        kItemGap = 4,
        kItemCount = 4
    };

    CNavBar();

    void SetBrand(const wchar_t* name, const wchar_t* version);

    void SetSelection(int index);

    int GetSelection() const { return m_selection; }

    // 第 index 项在控件客户区中的矩形（尺寸固定，与父窗口大小无关）
    static CRect GetItemRect(int index);

protected:
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnPaint();
    afx_msg void OnMouseMove(UINT nFlags, CPoint point);
    afx_msg void OnMouseLeave();
    afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
    // 注意：MFC 的 ON_WM_NCHITTEST() 绑定的函数名是 OnNcHitTest（小写 c），
    // 名字写错会静默落到 CWnd::OnNcHitTest 默认实现（Static 返回 HTTRANSPARENT，
    // 真实鼠标点击会被转交父窗口，导航项就点不动）
    afx_msg LRESULT OnNcHitTest(CPoint point);
    DECLARE_MESSAGE_MAP()

    // 同 CColorTab：派生类需要独立的 super proc 存储，避免多个子类对象相互冲掉
    virtual WNDPROC* GetSuperWndProcAddr() { return &m_superProc; }

private:
    int HitTest(CPoint point) const;

    int m_selection;
    int m_hover;
    bool m_tracking;
    CString m_brandName;
    CString m_brandVersion;
    WNDPROC m_superProc;
};

} // namespace Ui
} // namespace ps

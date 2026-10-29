#pragma once

// GDI+ 绘制层（现代扁平 + 卡片化视觉的基础设施）
//
// 只依赖系统自带的 gdiplus.dll（Win7 SP1 原生可用），不引入任何第三方库。
// 提供圆角矩形、渐变、柔和阴影、胶囊标签与矢量图标，供 UiTheme / 各对话框绘制使用。

#include <afxwin.h>

namespace ps
{
namespace Ui
{
namespace Gfx
{

// 初始化 GDI+（幂等；进程内只需一次，进程退出由系统回收）
bool Initialize();

// ---- 基础图元（颜色均为 COLORREF，alpha 0..255）----

void FillRoundRect(HDC hdc, const RECT& rc, int radius, COLORREF color, BYTE alpha = 255);

// 竖直渐变：top 在上、bottom 在下
void FillRoundRectGradient(HDC hdc, const RECT& rc, int radius,
                           COLORREF top, COLORREF bottom, BYTE alpha = 255);

// 水平渐变（左→右）
void FillRoundRectGradientH(HDC hdc, const RECT& rc, int radius,
                            COLORREF left, COLORREF right, BYTE alpha = 255);

void DrawRoundRect(HDC hdc, const RECT& rc, int radius, COLORREF color,
                   float width = 1.0f, BYTE alpha = 255);

void FillEllipse(HDC hdc, const RECT& rc, COLORREF color, BYTE alpha = 255);

// 柔和阴影：在 rc 之外按 spread 像素逐层衰减（模拟高斯阴影）
void DrawSoftShadow(HDC hdc, const RECT& rc, int radius, int spread, int offsetY,
                    BYTE alpha, COLORREF color);

// ---- 矢量图标（统一 1.7px 描边、圆头，按 rect 自动缩放）----

enum class Icon
{
    None,           // 无图标
    Printer,        // 打印机
    Gear,           // 齿轮
    Signal,         // 网络信号
    Wrench,         // 工具
    Chart,          // 统计
    Document,       // 文档 / 日志
    Upload,         // 上报
    Download,       // 下载
    Search,         // 检查
    Chat,           // 交流群
    Refresh,        // 刷新
    Trash,          // 清空
    Check,          // 勾
    Copy,           // 复制
    Shield,         // 安全 / 防护
    Clock,          // 等待
    Warning,        // 警告
    Info,           // 提示
    Play,           // 启动
    Stop,           // 停止
    Key,            // 密钥
};

void DrawIcon(HDC hdc, Icon icon, const RECT& rc, COLORREF color, BYTE alpha = 255);

// 把图标画在中心为 center、边长 size 的正方形里
void DrawIconCentered(HDC hdc, Icon icon, POINT center, int size, COLORREF color,
                      BYTE alpha = 255);

} // namespace Gfx
} // namespace Ui
} // namespace ps

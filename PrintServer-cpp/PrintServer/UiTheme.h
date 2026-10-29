#pragma once

// WinForms 视觉元素的 MFC 等价实现 + 现代扁平化设计系统
//
// 设计系统约定（96 DPI 逻辑坐标；窗口 DPI 非感知，由系统整体缩放）：
//   - 页面底色 / 卡片表面 / 卡片描边 三级层次
//   - 品牌渐变头部（深蓝 → 亮蓝）
//   - 卡片圆角 10、按钮圆角 8、胶囊标签全圆角
//   - 语义色（成功 / 危险 / 警告 / 信息）用于状态胶囊
//   - 图标一律矢量绘制：不用 emoji（Win7 上 emoji 会显示成方框）

#include <afxwin.h>
#include <string>
#include <vector>
#include "UiGdiPlus.h"

namespace ps
{
namespace Ui
{

// ===== 设计令牌 =====
namespace Design
{
// 表面层次（页面底 → 卡片/面板白 → 字段底，逐级拉开以免"发灰看不出层级"）
const COLORREF kPageBack = RGB(241, 245, 251);    // #F1F5FB
const COLORREF kCardBack = RGB(255, 255, 255);    // #FFFFFF
const COLORREF kCardBorder = RGB(220, 229, 241);  // #DCE5F1
const COLORREF kDivider = RGB(237, 242, 249);     // #EDF2F9
const COLORREF kFieldBack = RGB(250, 252, 255);   // #FAFCFF
const COLORREF kFieldBorder = RGB(203, 213, 225); // #CBD5E1
const COLORREF kReadonlyBack = RGB(241, 245, 249); // #F1F5F9（只读输入框底）

// 品牌色（蓝白搭配：白为主，蓝色只用于品牌区/主按钮/激活导航项/徽标）
const COLORREF kAccent = RGB(29, 78, 216);        // #1D4ED8
const COLORREF kAccentHover = RGB(27, 69, 196);   // #1B45C4
const COLORREF kAccentSoft = RGB(232, 239, 253);  // #E8EFFD
const COLORREF kBrandTop = RGB(29, 78, 216);      // 兼容旧的渐变组件
const COLORREF kBrandBottom = RGB(37, 99, 235);

// 左侧导航栏（浅蓝白，唯一重色块是顶部品牌区）
const COLORREF kNavBack = RGB(247, 250, 255);     // #F7FAFF
const COLORREF kNavBorder = RGB(226, 234, 246);   // #E2EAF6
const COLORREF kNavText = RGB(74, 92, 120);       // #4A5C78
const COLORREF kNavHover = RGB(234, 241, 255);    // #EAF1FF
const COLORREF kNavFootBack = RGB(239, 245, 255); // #EFF5FF
const COLORREF kNavFootText = RGB(91, 112, 147);  // #5B7093

// 对话框头部（浅蓝白 + 顶部主色横条，不出现深色大块）
const COLORREF kHeadBack = RGB(245, 249, 255);    // #F5F9FF
const COLORREF kHeadBorder = RGB(226, 234, 246);  // #E2EAF6

// 文本
const COLORREF kTextTitle = RGB(11, 31, 58);      // #0B1F3A
const COLORREF kTextBody = RGB(51, 65, 85);       // #334155
const COLORREF kTextMuted = RGB(100, 116, 139);   // #64748B
const COLORREF kTextDisabled = RGB(148, 163, 184); // #94A3B8
const COLORREF kTextOnBrand = RGB(255, 255, 255);
const COLORREF kTextOnBrandSub = RGB(199, 219, 255); // #C7DBFF

// 语义色（各配浅底，用于状态胶囊/步骤徽标/提示行）
const COLORREF kSuccess = RGB(22, 163, 74);       // #16A34A
const COLORREF kSuccessSoft = RGB(232, 247, 238); // #E8F7EE
const COLORREF kDanger = RGB(220, 38, 38);        // #DC2626
const COLORREF kDangerSoft = RGB(253, 236, 236);  // #FDECEC
const COLORREF kWarning = RGB(217, 119, 6);       // #D97706
const COLORREF kWarningSoft = RGB(253, 243, 227); // #FDF3E3
const COLORREF kInfo = RGB(14, 165, 233);         // #0EA5E9
const COLORREF kInfoSoft = RGB(231, 246, 254);    // #E7F6FE
const COLORREF kNeutralSoft = RGB(238, 243, 250); // #EEF3FA

// 日志面板（界面内唯一保留深色的控件）
const COLORREF kLogBack = RGB(15, 23, 41);        // #0F1729
const COLORREF kLogText = RGB(167, 243, 208);     // #A7F3D0
const COLORREF kLogHeader = RGB(24, 34, 54);      // #182236
const COLORREF kLogDot = RGB(52, 211, 153);
const COLORREF kLogStripText = RGB(138, 160, 184);
const COLORREF kLogStripNote = RGB(91, 107, 128);
const int kLogStripHeight = 24;                   // 日志框顶部「实时日志」条
const int kLogBoxInset = 16;                      // 日志框距卡片边缘

// 圆角与间距
const int kRadiusCard = 12;
const int kRadiusButton = 8;
const int kCardPad = 16;          // 卡片内边距（原型 .card padding:16px）
const int kCardHeadHeight = 22;   // 卡片标题行高（原型 .card-head height:22px）
const int kCardBodyGap = 10;      // 标题行与内容间距（原型 .card-body margin-top:10px）
const int kCardContentTop = kCardPad + kCardHeadHeight + kCardBodyGap;   // = 48
const int kBtnHeight = 36;        // 按钮高（原型 .btn height:36px）
const int kBtnHeightSmall = 28;   // 小按钮高（原型 .btn-sm height:28px）
const int kInputHeight = 34;      // 输入框高（原型 .input height:34px）
const int kPillHeight = 22;       // 胶囊高（原型 .pill height:22px）
const int kIconButtonSize = 28;   // 图标按钮尺寸（原型 .icon-btn 28x28）
const int kPillMinWidth = 60;     // 胶囊最小宽度（文字自适应时的下限）
const int kRadiusSegItem = 6;     // 分段控件条目圆角（原型 .seg .item border-radius:6px）
const int kSegHeight = 32;        // 分段控件高（原型 .seg height:32px）
const int kSegPad = 4;            // 分组内边距（原型 .seg padding:4px）
const int kSegGap = 4;            // 条目间距（原型 .seg gap:4px）
const int kSegItemHeight = 24;    // 条目高（原型 .seg .item height:24px）
} // namespace Design

// 字体（按 族名+字号+粗体 缓存，进程生命周期内复用）
HFONT GetFont(const wchar_t* family, int pointSize, bool bold);

// WinForms 的 SystemFonts.DefaultFont（对应非客户端消息字体）
HFONT GetDefaultFont();

// 设置控件字体
void ApplyFont(CWnd* wnd, HFONT font);

// ---- 现代按钮 / 卡片 / 状态胶囊 ----

// 将按钮设为自绘并注册外观（圆角 + 渐变 + 悬停/按下/禁用 + 可选左侧矢量图标）
// parent 为按钮的直接父窗口（通常是对话框）；surface 为按钮所在的表面色
// （用于绘制圆角外四角与禁用态混色）
// backdrop 为圆角之外四角实际要铺的底色（CLR_INVALID = 同 surface）。按钮不在白卡片
// 上时（如页面标题行）必须显式传入所在底色，否则四角会露出白底
void MakeFlatButton(CWnd* parent, UINT id, COLORREF back, COLORREF fore,
                    Gfx::Icon icon = Gfx::Icon::None,
                    COLORREF surface = Design::kCardBack,
                    COLORREF backdrop = CLR_INVALID);

// 将按钮设为次级（描边）风格：表面底色 + 1px 描边 + 文字色。
// border 为描边色；不传（CLR_INVALID）时描边取 accent 同色（语义色按钮用）
void MakeOutlineButton(CWnd* parent, UINT id, COLORREF accent = Design::kAccent,
                       Gfx::Icon icon = Gfx::Icon::None,
                       COLORREF surface = Design::kCardBack,
                       COLORREF border = CLR_INVALID,
                       COLORREF backdrop = CLR_INVALID);

// 注册为强调徽标（步骤数字或矢量图标）：强调色实底圆角小块 + 白色文字/图标
// radius 传半高即为圆形徽标（原型 .step-no 24px 圆形）
void MakeBadge(CWnd* parent, UINT id, Gfx::Icon icon = Gfx::Icon::None,
               COLORREF surface = Design::kCardBack,
               COLORREF accent = Design::kAccent,
               int radius = 6);

// 注册为输入框底板：圆角 + 字段底色/描边（须置于 Edit 之下，见 z 序）
void MakeField(CWnd* parent, UINT id, COLORREF surface = Design::kCardBack);

// 注册为状态胶囊：底色由文字色派生浅色调，文字色沿用 SetTextColor
void MakePill(CWnd* parent, UINT id, COLORREF surface = Design::kCardBack,
              COLORREF backdrop = CLR_INVALID);

// 状态胶囊按当前文字自适应宽度（左内边距 22 含圆点 + 右内边距 8，圆角取半高）
int MeasurePillWidth(HWND pill, HFONT font);

// 文本像素宽度（供按内容排布的控件使用）
int MeasureTextWidth(HWND wnd, HFONT font, const wchar_t* text);

// 注册为分段控件（原型 .seg）：一个控件画整组浅蓝底 + 选中项白底主色描边，
// 点击条目后向父窗口发送 kSegChanged（wParam = 选中序号）
void MakeSeg(CWnd* parent, UINT id, const std::vector<std::wstring>& items, int selected,
             COLORREF surface = Design::kPageBack);
void SetSegSelection(CWnd* parent, UINT id, int selected);
void SetSegItemText(CWnd* parent, UINT id, int index, const wchar_t* text);

// 分段控件按当前文案应有的宽度（供创建与文案变化后重排使用）
int MeasureSegWidth(CWnd* parent, UINT id);

// 分段控件选中项变化通知（wParam = 新选中项序号）
const UINT kSegChanged = WM_APP + 70;

// 注册为日志框：深色圆角底 + 顶部「实时输出」条（自有识别元素，叠在卡片之上、
// 日志编辑框之下）
void MakeLogBox(CWnd* parent, UINT id, COLORREF surface = Design::kCardBack);

// 日志框顶部条右侧文案（默认「自动刷新」，传入日志文件名即显示文件名）
void SetLogBoxNote(HWND box, const wchar_t* note);

// 在对话框的 WM_DRAWITEM 中调用；返回 true 表示已处理该控件
bool DrawPart(const DRAWITEMSTRUCT* dis);

// 兼容旧调用（等价于 DrawPart）
bool DrawFlatButton(const DRAWITEMSTRUCT* dis);

// 卡片与分区面板阴影：由对话框 OnPaint 在子控件绘制之前调用
// （阴影落在卡片/面板矩形之外，必须画在父窗口 DC 上，且隐藏的部件会跳过）。
// owner 为空：画在部件自己的父窗口坐标里；owner 非空：只画 owner 的兄弟部件，
// 并换算到 owner 的客户区坐标（用于页签控件这类盖住卡片区域的窗口）
void DrawCardShadows(HDC hdc, HWND owner = nullptr);

// 卡片表面登记：落在 cardRect 内的控件其背景刷使用 surface（供 OnCtlColor 使用）。
// parent 用于把登记限定在某个对话框内：同一进程可同时存在多个对话框（如主窗口
// 模态打开子对话框），不限定时坐标相近的另一对话框会冲掉登记或被误匹配。
void RegisterSurface(const RECT& cardRect, COLORREF surface, HWND parent = nullptr);
void ClearSurfaces(HWND parent = nullptr);
bool TryGetSurfaceColor(HWND control, COLORREF& surface);
HBRUSH GetSurfaceBrush(COLORREF surface);

// 运行期修改按钮配色与文字（对应 C# 直接改 BackColor / Text）
void SetFlatButtonColor(CWnd* parent, UINT id, COLORREF back, COLORREF fore);
void SetButtonText(CWnd* parent, UINT id, const wchar_t* text);

// 运行期修改次级（描边）按钮的描边色与文字色（对应原型 .btn-2 / .btn-danger 切换）
void SetOutlineButtonColor(CWnd* parent, UINT id, COLORREF ink, COLORREF border);

// 卡片外观（阴影 + 圆角白底 + 描边 + 标题行 + 左侧 3px 主色竖条）。
// 卡片改用对话框自己的 DC 绘制（不再用子窗口），避免卡片遮挡卡内控件
void DrawCardFace(HDC hdc, const RECT& rc, const wchar_t* title, HFONT font,
                  COLORREF surface = Design::kCardBack);

// ---- 控件文字颜色（对应 WinForms 的 ForeColor）----

void SetTextColor(HWND control, COLORREF color);
bool TryGetTextColor(HWND control, COLORREF& color);

// 标签文字
void SetLabelText(CWnd* parent, UINT id, const wchar_t* text);

} // namespace Ui
} // namespace ps

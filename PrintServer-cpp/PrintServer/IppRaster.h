#pragma once

// IPP 免驱打印的渲染侧（Phase B）
//
// 目标：把客户端发来的栅格/文档格式，变成「本地打印机驱动能消费的东西」。
//
// 落地路线（零第三方依赖、零许可风险）：
//   页位图  →  自组装 OpenXPS 包（用内置 miniz 打 OPC zip，PNG 用 miniz 的 zlib）
//           →  StartXpsPrintJob 交给 Windows spooler
//           →  服务端上已装的厂商驱动渲染成设备原生语言  →  USB 打印机
//
// 这样做的关键好处：**不需要为每种打印机写栅格编码器**，渲染交给已有驱动。
// 也刻意避开了 Ghostscript/MuPDF(Poppler) 的 AGPL/GPL 许可问题。

#include <functional>
#include <string>
#include <vector>

namespace ps
{
namespace IppRaster
{

// 32bpp BGRA 位图（行序自上而下，每行 width*4 字节）
struct RasterPage
{
    int width = 0;
    int height = 0;
    int dpiX = 300;
    int dpiY = 300;
    std::vector<unsigned char> bgra;

    bool IsValid() const
    {
        return width > 0 && height > 0 &&
               bgra.size() >= (size_t)width * (size_t)height * 4;
    }
};

// 生成一张自检页：白底 + 黑边框 + 对角线 + 四角方块 + 顶部标尺线。
// 用于验证「位图 → XPS → 驱动 → 纸面」的比例、方向、边距是否正确。
RasterPage MakeTestPage(int width, int height, int dpi);

// PNG(真彩 RGB) 编码（zlib 由 miniz 提供）
bool EncodePngRgb(const RasterPage& page, std::vector<unsigned char>& png, std::string& error);

// 组装 OpenXPS/OPC 包（多页）
bool BuildXpsPackage(const std::vector<RasterPage>& pages, std::vector<unsigned char>& package,
                     std::string& error);

// 按需取页的组装方式：包内只保留 PNG（每页约百 KB），
// 因此不会因为文档页数多而把几十上百页的原始位图全驻留内存。
// 回调返回 nullptr 表示失败（error 给出原因），组装随之中止。
// 返回的指针只需保证在"下一次回调之前"有效。
using PageProvider = std::function<const RasterPage*(size_t pageIndex, std::string& error)>;

bool BuildXpsPackageFromProvider(size_t pageCount, const PageProvider& provider,
                                 std::vector<unsigned char>& package, std::string& error);

// 把 XPS 包交给本地打印机驱动渲染打印
bool SubmitXpsToPrinter(const std::string& printerNameUtf8, const std::string& documentName,
                        const std::vector<unsigned char>& package, std::string& error);

// 便捷入口：组装 + 打印
bool PrintRasterPages(const std::string& printerNameUtf8, const std::vector<RasterPage>& pages,
                      const std::string& documentName, std::string& error);

// ===========================================================================
// GDI 位图打印（默认路径，优先使用）
// ===========================================================================
// 直接用打印机 DC 画位图（StretchDIBits），不经过 XPS 部件/资源 URI 解析：
//   - 对 v3/v4 驱动、各类厂商驱动都有效，兼容性比 XpsPrint 管线宽得多
//   - 不会因为 XPS 资源引用方式不同而输出空白页
// 页面按"保持纵横比、整页居中"映射到打印机物理页面。
// paperWidthMm / paperHeightMm > 0 时按该尺寸设置驱动纸张（跟随 PDF 页面），
// 宽 > 高自动切换横向；驱动不接受自定义纸张时静默回退到驱动默认纸张。
bool PrintPagesViaGdi(const std::string& printerNameUtf8, size_t pageCount,
                      const PageProvider& provider, const std::string& documentName,
                      std::string& error,
                      double paperWidthMm = 0.0, double paperHeightMm = 0.0);

// 便捷入口：把内存中的若干页直接走 GDI 打印
bool PrintRasterPagesViaGdi(const std::string& printerNameUtf8,
                            const std::vector<RasterPage>& pages,
                            const std::string& documentName, std::string& error);

// 读取打印机 DC 的 LOGPIXELSX/Y（打印机分辨率）。
// 用于按打印机原生分辨率渲染，避免低分辨率位图被放大后发虚。
bool GetPrinterResolution(const std::string& printerNameUtf8, int& dpiX, int& dpiY);

// ===========================================================================
// 打印机能力（直接问驱动，而不是靠配置猜）
// ===========================================================================
// IPP 的 color-supported / sides-supported / media-supported / printer-resolution
// 必须反映服务端这台打印机（及其驱动）的真实能力：
//   - 彩色机 / 黑白机      → DeviceCapabilities(DC_COLORDEVICE)
//   - 双面                 → DeviceCapabilities(DC_DUPLEX)
//   - 支持的纸张           → DeviceCapabilities(DC_PAPERS + DC_PAPERSIZE)
//   - 分辨率               → GetDeviceCaps(LOGPIXELSX/Y)
// 针式打印机（连续纸）会枚举出 241×140mm 这类非常规尺寸，硬编码 A4 会直接错位。
struct PrinterCapabilities
{
    bool valid = false;
    bool color = false;
    bool duplex = false;
    int dpiX = 600;
    int dpiY = 600;
    int paperWidthMm = 210;      // 驱动当前默认纸张
    int paperHeightMm = 297;
    std::vector<std::string> mediaKeywords;   // IPP media 关键字（去重，最多 12 个）
};

bool QueryPrinterCapabilities(const std::string& printerNameUtf8, PrinterCapabilities& caps,
                              std::string& error);

// 打印前位图增强（就地修改）：
//   中性灰（文字/线条）→ 按亮度二值化，避免驱动网点抖动导致发虚；
//   近白 → 纯白；深彩色 → 纯黑。
// monochromeOutput = true 时（目标打印机为黑白机），彩色元素也按亮度二值化：
//   否则彩色交给驱动转灰，各驱动算法不同、普遍偏淡（蓝色标题/图标会变浅）。
void EnhanceForPrint(RasterPage& page, bool monochromeOutput = false);

} // namespace IppRaster
} // namespace ps

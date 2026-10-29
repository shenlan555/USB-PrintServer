#include "pch.h"
#include "IppRaster.h"

#include "LogHelper.h"
#include "Wmi.h"
#include "third_party/miniz/miniz.h"

#include <xpsprint.h>
#include <winspool.h>   // DocumentPropertiesW（设置打印质量）
#include <string.h>
#include <math.h>
#include <algorithm>
#include <cstdlib>

#pragma comment(lib, "XpsPrint.lib")

// StartXpsPrintJob / XPS_JOB_STATUS 在新版 SDK 里被标记为 deprecated
// （官方建议改用 Print Document Package API）。但那套 API 需要 Windows 8+，
// 而本产品要兼容 Win7/2008，因此继续使用这套 Vista/Win7 即可用的接口，
// 并在此关闭该弃用警告。
#pragma warning(disable: 4995)

namespace ps
{
namespace IppRaster
{

namespace
{

// ===========================================================================
// PNG 编码（真彩 RGB，zlib 由 miniz 提供）
// ===========================================================================

void AppendBigEndian32(std::vector<unsigned char>& out, unsigned value)
{
    out.push_back((unsigned char)((value >> 24) & 0xFF));
    out.push_back((unsigned char)((value >> 16) & 0xFF));
    out.push_back((unsigned char)((value >> 8) & 0xFF));
    out.push_back((unsigned char)(value & 0xFF));
}

// 写入一个 PNG 块：长度 + 类型 + 数据 + CRC32(类型+数据)
void AppendPngChunk(std::vector<unsigned char>& out, const char type[4],
                    const unsigned char* data, size_t length)
{
    AppendBigEndian32(out, (unsigned)length);

    size_t crcStart = out.size();
    out.insert(out.end(), type, type + 4);
    if (length > 0 && data != nullptr)
        out.insert(out.end(), data, data + length);

    mz_ulong crc = mz_crc32(MZ_CRC32_INIT, out.data() + crcStart, out.size() - crcStart);
    AppendBigEndian32(out, (unsigned)crc);
}

std::string FormatNumber(double value)
{
    char buffer[48] = { 0 };
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%.2f", value);
    return buffer;
}

} // namespace

bool EncodePngRgb(const RasterPage& page, std::vector<unsigned char>& png, std::string& error)
{
    error.clear();
    png.clear();

    if (!page.IsValid())
    {
        error = "位图无效";
        return false;
    }

    // 原始扫描线：每行 1 字节滤波类型(0) + RGB
    std::vector<unsigned char> raw;
    raw.reserve((size_t)page.height * (1 + (size_t)page.width * 3));

    for (int y = 0; y < page.height; ++y)
    {
        raw.push_back(0);
        const unsigned char* src = page.bgra.data() + (size_t)y * page.width * 4;
        for (int x = 0; x < page.width; ++x)
        {
            raw.push_back(src[x * 4 + 2]);   // R
            raw.push_back(src[x * 4 + 1]);   // G
            raw.push_back(src[x * 4 + 0]);   // B
        }
    }

    mz_ulong bound = mz_compressBound((mz_ulong)raw.size());
    std::vector<unsigned char> compressed((size_t)bound);
    mz_ulong compressedLength = bound;

    if (mz_compress(compressed.data(), &compressedLength, raw.data(), (mz_ulong)raw.size()) != MZ_OK)
    {
        error = "PNG 压缩失败";
        return false;
    }
    compressed.resize((size_t)compressedLength);

    static const unsigned char kSignature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    png.insert(png.end(), kSignature, kSignature + 8);

    unsigned char ihdr[13];
    ihdr[0] = (unsigned char)((page.width >> 24) & 0xFF);
    ihdr[1] = (unsigned char)((page.width >> 16) & 0xFF);
    ihdr[2] = (unsigned char)((page.width >> 8) & 0xFF);
    ihdr[3] = (unsigned char)(page.width & 0xFF);
    ihdr[4] = (unsigned char)((page.height >> 24) & 0xFF);
    ihdr[5] = (unsigned char)((page.height >> 16) & 0xFF);
    ihdr[6] = (unsigned char)((page.height >> 8) & 0xFF);
    ihdr[7] = (unsigned char)(page.height & 0xFF);
    ihdr[8] = 8;    // bit depth
    ihdr[9] = 2;    // color type: truecolor RGB
    ihdr[10] = 0;   // compression
    ihdr[11] = 0;   // filter
    ihdr[12] = 0;   // interlace
    AppendPngChunk(png, "IHDR", ihdr, sizeof(ihdr));
    AppendPngChunk(png, "IDAT", compressed.data(), compressed.size());
    AppendPngChunk(png, "IEND", nullptr, 0);

    return true;
}

// ===========================================================================
// OpenXPS / OPC 包组装
// ===========================================================================

namespace
{

const char* const kContentTypes =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
    "<Default Extension=\"fdseq\" ContentType=\"application/vnd.ms-package.xps-fixeddocumentsequence+xml\"/>"
    "<Default Extension=\"fdoc\" ContentType=\"application/vnd.ms-package.xps-fixeddocument+xml\"/>"
    "<Default Extension=\"fpage\" ContentType=\"application/vnd.ms-package.xps-fixedpage+xml\"/>"
    "<Default Extension=\"png\" ContentType=\"image/png\"/>"
    "</Types>";

const char* const kPackageRels =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"R1\" Type=\"http://schemas.microsoft.com/xps/2005/06/fixedrepresentation\""
    " Target=\"/FixedDocumentSequence.fdseq\"/>"
    "</Relationships>";

const char* const kDocumentName = "/Documents/1/FixedDocument.fdoc";

bool ZipAddText(mz_zip_archive& zip, const char* name, const std::string& text)
{
    const void* data = text.empty() ? (const void*)"" : (const void*)text.data();
    return mz_zip_writer_add_mem(&zip, name, data, text.size(), MZ_DEFAULT_COMPRESSION) != 0;
}

// 页面尺寸换算：位图像素 → XPS 的 1/96 英寸单位
double PageWidth96(const RasterPage& page)
{
    return (double)page.width * 96.0 / (double)(page.dpiX > 0 ? page.dpiX : 300);
}

double PageHeight96(const RasterPage& page)
{
    return (double)page.height * 96.0 / (double)(page.dpiY > 0 ? page.dpiY : 300);
}

// 组装单个页面部件
std::string BuildPageMarkup(double pageWidth96, double pageHeight96, const std::string& imageSource)
{
    std::string width = FormatNumber(pageWidth96);
    std::string height = FormatNumber(pageHeight96);

    std::string xml;
    xml += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n";
    xml += "<FixedPage xmlns=\"http://schemas.microsoft.com/xps/2005/06\" Width=\"";
    xml += width;
    xml += "\" Height=\"";
    xml += height;
    xml += "\" xml:lang=\"en-US\">";

    // 用 Path + ImageBrush 铺满整页。Viewbox 省略（默认整幅图），
    // Viewport 用绝对页坐标，TileMode=None。
    xml += "<Path Data=\"M0,0 L";
    xml += width;
    xml += ",0 ";
    xml += width;
    xml += ",";
    xml += height;
    xml += " 0,";
    xml += height;
    xml += " z\"><Path.Fill><ImageBrush ImageSource=\"";
    xml += imageSource;
    xml += "\" Viewport=\"0,0,";
    xml += width;
    xml += ",";
    xml += height;
    xml += "\" ViewportUnits=\"Absolute\" TileMode=\"None\"/></Path.Fill></Path>";

    xml += "</FixedPage>";
    return xml;
}

} // namespace

bool BuildXpsPackageFromProvider(size_t pageCount, const PageProvider& provider,
                                 std::vector<unsigned char>& package, std::string& error)
{
    error.clear();
    package.clear();

    if (pageCount == 0 || !provider)
    {
        error = "没有页面";
        return false;
    }

    // 先把每页编码成 PNG（顺便校验位图），并记下页面尺寸。
    // 之后只保留 PNG（每页约百 KB），不再持有原始位图，避免大文档爆内存。
    std::vector<std::vector<unsigned char>> pngs(pageCount);
    std::vector<double> pageWidths(pageCount, 0.0);
    std::vector<double> pageHeights(pageCount, 0.0);

    for (size_t i = 0; i < pageCount; ++i)
    {
        std::string pageError;
        const RasterPage* page = provider(i, pageError);
        if (page == nullptr)
        {
            error = "第 " + std::to_string(i + 1) + " 页失败: " + pageError;
            return false;
        }

        std::string pngError;
        if (!EncodePngRgb(*page, pngs[i], pngError))
        {
            error = "第 " + std::to_string(i + 1) + " 页编码失败: " + pngError;
            return false;
        }

        pageWidths[i] = PageWidth96(*page);
        pageHeights[i] = PageHeight96(*page);
    }

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_heap(&zip, 0, 0))
    {
        error = "无法创建 XPS 包";
        return false;
    }

    bool ok = true;

    // [Content_Types].xml 与包级关系
    ok = ok && ZipAddText(zip, "[Content_Types].xml", kContentTypes);
    ok = ok && ZipAddText(zip, "_rels/.rels", kPackageRels);

    // FixedDocumentSequence + 其关系
    // 注意：Source 用「相对路径」，且 fdseq/fdoc 不再需要各自的 .rels
    // （这是对照微软 XPS 组件实际输出得到的规范结构）
    std::string sequence = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                           "<FixedDocumentSequence xmlns=\"http://schemas.microsoft.com/xps/2005/06\">"
                           "<DocumentReference Source=\"Documents/1/FixedDocument.fdoc\"/>"
                           "</FixedDocumentSequence>";
    ok = ok && ZipAddText(zip, "FixedDocumentSequence.fdseq", sequence);

    // FixedDocument：列出全部页（Source 相对本部件所在目录）
    std::string document = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                           "<FixedDocument xmlns=\"http://schemas.microsoft.com/xps/2005/06\">";

    for (size_t i = 0; i < pageCount; ++i)
        document += "<PageContent Source=\"Pages/" + std::to_string(i + 1) + ".fpage\"/>";

    document += "</FixedDocument>";

    ok = ok && ZipAddText(zip, "Documents/1/FixedDocument.fdoc", document);

    // 每页：页面标记 + 页面关系(required-resource) + 图片资源
    for (size_t i = 0; i < pageCount; ++i)
    {
        std::string index = std::to_string(i + 1);

        std::string pagePart = "Documents/1/Pages/" + index + ".fpage";
        std::string pageRelsPart = "Documents/1/Pages/_rels/" + index + ".fpage.rels";
        std::string imageName = "p" + index + ".png";
        std::string imagePart = "Resources/" + imageName;

        // 页面部件位于 Documents/1/Pages/，图片位于包根 Resources/。
        // 这里必须用"相对本部件"的 URI（../../../Resources/...）：
        // 写成包根绝对路径 "/Resources/..." 时，XpsPrint 的 XAML 解析器
        // 找不到资源，页面会被渲染成纯白（已修复）。
        const std::string imageSource = "../../../" + imagePart;

        ok = ok && ZipAddText(zip, pagePart.c_str(),
                              BuildPageMarkup(pageWidths[i], pageHeights[i], imageSource));

        std::string pageRels =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"Img" + index +
            "\" Type=\"http://schemas.microsoft.com/xps/2005/06/required-resource\""
            " Target=\"../../../Resources/" + imageName + "\"/></Relationships>";
        ok = ok && ZipAddText(zip, pageRelsPart.c_str(), pageRels);

        if (!mz_zip_writer_add_mem(&zip, imagePart.c_str(),
                                   pngs[i].empty() ? (const void*)"" : (const void*)pngs[i].data(),
                                   pngs[i].size(), MZ_DEFAULT_COMPRESSION))
        {
            ok = false;
        }
    }

    if (ok)
    {
        void* buffer = nullptr;
        size_t size = 0;
        if (mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size) && buffer != nullptr && size > 0)
        {
            package.assign((unsigned char*)buffer, (unsigned char*)buffer + size);
        }
        else
        {
            ok = false;
            error = "XPS 包写入失败";
        }
        if (buffer != nullptr)
            mz_free(buffer);
    }

    mz_zip_writer_end(&zip);

    if (!ok && error.empty())
        error = "XPS 包组装失败";

    return ok;
}

bool BuildXpsPackage(const std::vector<RasterPage>& pages, std::vector<unsigned char>& package,
                     std::string& error)
{
    if (pages.empty())
    {
        error = "没有页面";
        package.clear();
        return false;
    }

    // 零拷贝：直接引用调用方持有的位图
    const std::vector<RasterPage>* source = &pages;
    return BuildXpsPackageFromProvider(
        pages.size(),
        [source](size_t index, std::string&) -> const RasterPage* {
            if (index >= source->size())
                return nullptr;
            return &(*source)[index];
        },
        package, error);
}

// ===========================================================================
// 交作业：XPS 包 → Windows 打印管线
// ===========================================================================

bool SubmitXpsToPrinter(const std::string& printerNameUtf8, const std::string& documentName,
                        const std::vector<unsigned char>& package, std::string& error)
{
    error.clear();

    if (package.empty())
    {
        error = "XPS 包为空";
        return false;
    }

    std::wstring printerName = Utf8ToWide(printerNameUtf8);
    std::wstring jobName = Utf8ToWide(documentName.empty() ? std::string("IPP Document") : documentName);

    // StartXpsPrintJob 需要 COM；工作线程可能是首次使用。
    // 注意：只有 S_OK 才表示"本次由我们初始化"，S_FALSE 表示已被初始化过，
    // 此时绝不能 CoUninitialize，否则会提前把别人的引用计数减掉。
    HRESULT hrInit = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool needUninitialize = (hrInit == S_OK);

    HANDLE completion = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (completion == nullptr)
    {
        if (needUninitialize)
            ::CoUninitialize();
        error = "创建完成事件失败";
        return false;
    }

    IXpsPrintJob* job = nullptr;
    IXpsPrintJobStream* stream = nullptr;

    // xpsprint.h 的真实签名（10 个参数，注意没有 documentName）：
    //   printerName, jobName, outputFileName, progressEvent, completionEvent,
    //   printablePagesOn, printablePagesOnCount, xpsPrintJob, documentStream, printTicketStream
    //
    // 第 3 个参数是"输出文件名"：只要非 NULL，作业就会被写进这个文件，
    // **不会送到打印机**（xpsprint 的"打印到文件"模式）。
    // 1.4.1 一直把 jobName 传进这个位置，所以 IPP 作业从未真正到达打印机：
    // spooler 报完成、队列里没有作业、打印机不动，全部由此而来。
    HRESULT hr = ::StartXpsPrintJob(printerName.c_str(), jobName.c_str(), nullptr,
                                    nullptr, completion, nullptr, 0,
                                    &job, &stream, nullptr);
    if (FAILED(hr) || job == nullptr || stream == nullptr)
    {
        if (stream != nullptr)
            stream->Release();
        if (job != nullptr)
            job->Release();
        ::CloseHandle(completion);
        if (needUninitialize)
            ::CoUninitialize();

        char text[64] = { 0 };
        _snprintf_s(text, sizeof(text), _TRUNCATE, "0x%08X", (unsigned)hr);
        error = "StartXpsPrintJob 失败 (" + std::string(text) + ")";
        return false;
    }

    bool writeOk = true;

    size_t offset = 0;
    while (offset < package.size())
    {
        ULONG chunk = (ULONG)((package.size() - offset > (1u << 20)) ? (1u << 20)
                                                                     : (package.size() - offset));
        ULONG written = 0;
        if (FAILED(stream->Write(package.data() + offset, chunk, &written)) || written == 0)
        {
            writeOk = false;
            break;
        }
        offset += written;
    }

    if (writeOk)
        writeOk = SUCCEEDED(stream->Close());

    if (!writeOk)
    {
        error = "写入 XPS 数据失败";
        stream->Release();
        job->Release();
        ::CloseHandle(completion);
        if (needUninitialize)
            ::CoUninitialize();
        return false;
    }

    // 等待后台完成（大页面 + 慢驱动可能较久）
    DWORD wait = ::WaitForSingleObject(completion, 5 * 60 * 1000);

    XPS_JOB_STATUS status;
    memset(&status, 0, sizeof(status));
    HRESULT statusResult = job->GetJobStatus(&status);

    // jobStatus 是打印管线回报的 HRESULT：驱动/端口出错时 completion 仍可能是
    // COMPLETED，但 jobStatus 会带错误码，必须一并判断，否则会"假成功"。
    bool completed = (wait == WAIT_OBJECT_0) && SUCCEEDED(statusResult) &&
                     status.completion == XPS_JOB_COMPLETED &&
                     SUCCEEDED(status.jobStatus);

    if (!completed)
    {
        if (wait == WAIT_TIMEOUT)
        {
            error = "打印超时（驱动未在 5 分钟内完成）";
        }
        else
        {
            char text[96] = { 0 };
            _snprintf_s(text, sizeof(text), _TRUNCATE, "completion=%d, hr=0x%08X",
                        (int)status.completion, (unsigned)status.jobStatus);
            error = "打印未完成（" + std::string(text) + "）";
        }
    }

    stream->Release();
    job->Release();
    ::CloseHandle(completion);

    if (needUninitialize)
        ::CoUninitialize();

    return completed;
}

bool PrintRasterPages(const std::string& printerNameUtf8, const std::vector<RasterPage>& pages,
                      const std::string& documentName, std::string& error)
{
    std::vector<unsigned char> package;
    if (!BuildXpsPackage(pages, package, error))
        return false;

    Log::WriteLog("[IppRaster] XPS 包已组装: " + std::to_string(pages.size()) + " 页, " +
                  std::to_string(package.size() / 1024) + " KB");

    return SubmitXpsToPrinter(printerNameUtf8, documentName, package, error);
}

// ===========================================================================
// GDI 位图打印（默认路径）
// ===========================================================================

namespace
{

// 一次性设置驱动的打印质量、纸张与方向（同一个 DEVMODE，只 ResetDC 一次）。
//   dpi：目标打印质量。很多驱动默认 300 dpi 的"标准/省墨"模式，
//        即使渲染了 600 dpi 位图也会被降采样，小字必然发虚。
//   paperWidthMm / paperHeightMm：跟随 PDF 页面的纸张尺寸；<=0 表示不改。
//        宽 > 高时按横向处理（交换宽高 + DMORIENT_LANDSCAPE）。
// 驱动不接受自定义纸张时回退为"只设质量"，不影响打印。
bool ApplyDeviceSetup(HDC& dc, const std::wstring& printerName, int dpi,
                      double paperWidthMm, double paperHeightMm)
{
    if (dc == nullptr || printerName.empty())
        return false;

    // DocumentProperties 的第二个参数是打印机句柄，设备名是第三个参数
    std::wstring name = printerName;
    HANDLE printer = nullptr;
    if (!::OpenPrinterW(&name[0], &printer, nullptr))
        return false;

    LONG size = ::DocumentPropertiesW(nullptr, printer, &name[0], nullptr, nullptr, 0);
    if (size <= 0)
    {
        ::ClosePrinter(printer);
        return false;
    }

    std::vector<unsigned char> buffer((size_t)size);
    DEVMODEW* devMode = reinterpret_cast<DEVMODEW*>(buffer.data());
    devMode->dmSize = sizeof(DEVMODEW);

    if (::DocumentPropertiesW(nullptr, printer, &name[0], devMode, nullptr, DM_OUT_BUFFER) != IDOK)
    {
        ::ClosePrinter(printer);
        return false;
    }

    if (dpi > 0)
    {
        devMode->dmFields |= DM_PRINTQUALITY;
        devMode->dmPrintQuality = dpi;
    }

    const bool hasPaper = (paperWidthMm > 1.0 && paperHeightMm > 1.0);

    // 自定义纸张：DEVMODE 的字段是 dmPaperWidth（宽）+ dmPaperLength（高），
    // 单位 1/10 毫米。横向页面直接给"宽 > 高"的物理尺寸，不改 dmOrientation
    // （位图本身已经是横向的，再旋转会变成双倍旋转）。
    if (hasPaper)
    {
        devMode->dmFields |= DM_PAPERWIDTH | DM_PAPERLENGTH;
        devMode->dmPaperWidth = (short)(paperWidthMm * 10.0 + 0.5);
        devMode->dmPaperLength = (short)(paperHeightMm * 10.0 + 0.5);
    }

    if (::DocumentPropertiesW(nullptr, printer, &name[0], devMode, devMode,
                              DM_IN_BUFFER | DM_OUT_BUFFER) != IDOK)
    {
        if (!hasPaper)
        {
            ::ClosePrinter(printer);
            return false;
        }

        // 该驱动不支持自定义纸张：只保留打印质量再试一次
        devMode->dmFields &= ~(DM_PAPERWIDTH | DM_PAPERLENGTH);

        if (::DocumentPropertiesW(nullptr, printer, &name[0], devMode, devMode,
                                  DM_IN_BUFFER | DM_OUT_BUFFER) != IDOK)
        {
            ::ClosePrinter(printer);
            return false;
        }
    }

    HDC updated = ::ResetDCW(dc, devMode);
    ::ClosePrinter(printer);

    if (updated == nullptr)
        return false;

    dc = updated;
    return true;
}

} // namespace

bool PrintPagesViaGdi(const std::string& printerNameUtf8, size_t pageCount,
                      const PageProvider& provider, const std::string& documentName,
                      std::string& error, double paperWidthMm, double paperHeightMm)
{
    error.clear();

    if (pageCount == 0 || !provider)
    {
        error = "没有页面";
        return false;
    }

    const std::wstring printerName = Utf8ToWide(printerNameUtf8);
    if (printerName.empty())
    {
        error = "打印机名为空";
        return false;
    }

    HDC dc = ::CreateDCW(L"WINSPOOL", printerName.c_str(), nullptr, nullptr);
    if (dc == nullptr)
    {
        error = "无法创建打印机 DC（打印机名或驱动不可用，错误 " +
                std::to_string(::GetLastError()) + "）";
        return false;
    }

    // 打印质量拉到设备原生分辨率，并按 PDF 页面尺寸设置纸张与方向。
    // 驱动若默认 300 dpi，600 dpi 位图会被降采样回去；纸张不设则跟随驱动默认
    // （PDF 是 Letter/A5/横向时会被整体缩放甚至方向错误）。
    const int requestedDpi = ::GetDeviceCaps(dc, LOGPIXELSX);
    const bool deviceSetupOk =
        ApplyDeviceSetup(dc, printerName, requestedDpi, paperWidthMm, paperHeightMm);

    if (!deviceSetupOk)
        Log::WriteLog("[IppRaster] 设备设置未生效，沿用驱动默认纸张/质量");

    const int deviceDpiX = ::GetDeviceCaps(dc, LOGPIXELSX);
    const int deviceDpiY = ::GetDeviceCaps(dc, LOGPIXELSY);

    int physicalWidth = ::GetDeviceCaps(dc, PHYSICALWIDTH);
    int physicalHeight = ::GetDeviceCaps(dc, PHYSICALHEIGHT);
    int offsetX = ::GetDeviceCaps(dc, PHYSICALOFFSETX);
    int offsetY = ::GetDeviceCaps(dc, PHYSICALOFFSETY);

    // 个别驱动不实现 PHYSICALxxx，退回可打印区域
    if (physicalWidth <= 0 || physicalHeight <= 0)
    {
        physicalWidth = ::GetDeviceCaps(dc, HORZRES);
        physicalHeight = ::GetDeviceCaps(dc, VERTRES);
        offsetX = 0;
        offsetY = 0;
    }

    if (physicalWidth <= 0 || physicalHeight <= 0)
    {
        error = "无法获取打印机页面尺寸";
        ::DeleteDC(dc);
        return false;
    }

    Log::WriteLog("[IppRaster] GDI 打印: 设备 " + std::to_string(deviceDpiX) + "x" +
                  std::to_string(deviceDpiY) + " dpi, 物理页面 " +
                  std::to_string(physicalWidth) + "x" + std::to_string(physicalHeight) +
                  ", 偏移 " + std::to_string(offsetX) + "," + std::to_string(offsetY));

    DOCINFOW docInfo;
    memset(&docInfo, 0, sizeof(docInfo));
    docInfo.cbSize = sizeof(docInfo);
    const std::wstring wideName =
        Utf8ToWide(documentName.empty() ? std::string("IPP Document") : documentName);
    docInfo.lpszDocName = wideName.c_str();

    if (::StartDocW(dc, &docInfo) <= 0)
    {
        error = "StartDoc 失败（错误 " + std::to_string(::GetLastError()) + "）";
        ::DeleteDC(dc);
        return false;
    }

    bool ok = true;

    for (size_t i = 0; i < pageCount && ok; ++i)
    {
        std::string pageError;
        const RasterPage* page = provider(i, pageError);
        if (page == nullptr || !page->IsValid())
        {
            error = pageError.empty() ? ("第 " + std::to_string(i + 1) + " 页无效") : pageError;
            ok = false;
            break;
        }

        if (::StartPage(dc) <= 0)
        {
            error = "StartPage 失败";
            ok = false;
            break;
        }

        // 保持纵横比、整页居中
        const double pageRatio = (double)page->width / (double)page->height;
        const double paperRatio = (double)physicalWidth / (double)physicalHeight;

        int targetWidth = physicalWidth;
        int targetHeight = physicalHeight;

        if (pageRatio > paperRatio)
            targetHeight = (int)((double)physicalWidth / pageRatio + 0.5);
        else
            targetWidth = (int)((double)physicalHeight * pageRatio + 0.5);

        if (targetWidth <= 0)
            targetWidth = physicalWidth;
        if (targetHeight <= 0)
            targetHeight = physicalHeight;

        const int targetX = -offsetX + (physicalWidth - targetWidth) / 2;
        const int targetY = -offsetY + (physicalHeight - targetHeight) / 2;

        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = page->width;
        bmi.bmiHeader.biHeight = -page->height;   // 负数：自上而下
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        // PDF 页面按 600 dpi 渲染出来往往是 4961x7016，而打印机物理页面是
        // 4960x7016：相差 1 像素也会走 StretchDIBits 的 halftone 重采样，
        // 整页文字边缘被网点化 —— 这是"看起来发虚"的直接原因。
        // 因此差值在几个像素内时，直接按位图原尺寸 1:1 拷贝（最多裁掉
        // 几个像素，远小于文档页边距）。
        const int widthDiff = (targetWidth > page->width) ? (targetWidth - page->width)
                                                          : (page->width - targetWidth);
        const int heightDiff = (targetHeight > page->height) ? (targetHeight - page->height)
                                                              : (page->height - targetHeight);

        const bool oneToOne = (widthDiff <= 8 && heightDiff <= 8);

        Log::WriteLog("[IppRaster] 页面绘制: 源 " + std::to_string(page->width) + "x" +
                      std::to_string(page->height) + ", 目标 " + std::to_string(targetWidth) +
                      "x" + std::to_string(targetHeight) + ", 1:1=" +
                      (oneToOne ? "是" : "否(缩放)"));

        if (oneToOne)
        {
            // 以位图原尺寸绘制，绝不重采样（文字最锐利）
            const int drawX = -offsetX + (physicalWidth - page->width) / 2;
            const int drawY = -offsetY + (physicalHeight - page->height) / 2;

            const int lines = ::SetDIBitsToDevice(dc, drawX, drawY,
                                                  (DWORD)page->width, (DWORD)page->height,
                                                  0, 0, 0, (UINT)page->height,
                                                  page->bgra.data(), &bmi, DIB_RGB_COLORS);
            if (lines <= 0)
            {
                error = "SetDIBitsToDevice 失败（错误 " + std::to_string(::GetLastError()) + "）";
                ok = false;
            }
        }
        else
        {
            // 需要缩放时用 HALFTONE（对灰阶/照片更平滑）
            ::SetStretchBltMode(dc, HALFTONE);
            ::SetBrushOrgEx(dc, 0, 0, nullptr);

            const int copied = ::StretchDIBits(dc, targetX, targetY, targetWidth, targetHeight,
                                               0, 0, page->width, page->height,
                                               page->bgra.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
            if (copied == GDI_ERROR)
            {
                error = "StretchDIBits 失败（错误 " + std::to_string(::GetLastError()) + "）";
                ok = false;
            }
        }

        if (::EndPage(dc) <= 0)
        {
            if (ok)
                error = "EndPage 失败";
            ok = false;
        }
    }

    if (ok)
    {
        if (::EndDoc(dc) <= 0)
        {
            error = "EndDoc 失败（错误 " + std::to_string(::GetLastError()) + "）";
            ok = false;
        }
    }
    else
    {
        ::AbortDoc(dc);
    }

    ::DeleteDC(dc);
    return ok;
}

namespace
{

// DMPAPER_* → IPP media 关键字；识别不了时按尺寸兜底（容差 4mm）
std::string MapPaperToIppMedia(int paperId, int widthTenthMm, int heightTenthMm)
{
    switch (paperId)
    {
    case DMPAPER_A3:        return "iso_a3_297x420mm";
    case DMPAPER_A4:        return "iso_a4_210x297mm";
    case DMPAPER_A5:        return "iso_a5_148x210mm";
    case DMPAPER_A6:        return "iso_a6_105x148mm";
    case DMPAPER_B4:        return "jis_b4_257x364mm";
    case DMPAPER_B5:        return "jis_b5_182x257mm";
    case DMPAPER_LETTER:    return "na_letter_8.5x11in";
    case DMPAPER_LEGAL:     return "na_legal_8.5x14in";
    case DMPAPER_EXECUTIVE: return "na_executive_7.25x10.5in";
    case DMPAPER_TABLOID:   return "na_ledger_11x17in";
    default:                break;
    }

    if (widthTenthMm <= 0 || heightTenthMm <= 0)
        return std::string();

    struct KnownSize
    {
        int width;
        int height;
        const char* keyword;
    };

    static const KnownSize kKnown[] = {
        { 2970, 4200, "iso_a3_297x420mm" },
        { 2100, 2970, "iso_a4_210x297mm" },
        { 1480, 2100, "iso_a5_148x210mm" },
        { 1050, 1480, "iso_a6_105x148mm" },
        { 2570, 3640, "jis_b4_257x364mm" },
        { 1820, 2570, "jis_b5_182x257mm" },
        { 2159, 2794, "na_letter_8.5x11in" },
        { 2159, 3556, "na_legal_8.5x14in" },
        { 1842, 2667, "na_executive_7.25x10.5in" },
        { 2794, 4318, "na_ledger_11x17in" },
    };

    for (size_t i = 0; i < _countof(kKnown); ++i)
    {
        if (abs(widthTenthMm - kKnown[i].width) <= 40 &&
            abs(heightTenthMm - kKnown[i].height) <= 40)
        {
            return kKnown[i].keyword;
        }
    }

    return std::string();
}

} // namespace

bool QueryPrinterCapabilities(const std::string& printerNameUtf8, PrinterCapabilities& caps,
                              std::string& error)
{
    error.clear();
    caps = PrinterCapabilities();

    const std::wstring printerName = Utf8ToWide(printerNameUtf8);
    if (printerName.empty())
    {
        error = "打印机名为空";
        return false;
    }

    HDC dc = ::CreateDCW(L"WINSPOOL", printerName.c_str(), nullptr, nullptr);
    if (dc == nullptr)
    {
        error = "无法创建打印机 DC（打印机名/驱动不可用）";
        return false;
    }

    caps.dpiX = ::GetDeviceCaps(dc, LOGPIXELSX);
    caps.dpiY = ::GetDeviceCaps(dc, LOGPIXELSY);
    if (caps.dpiX <= 0)
        caps.dpiX = 600;
    if (caps.dpiY <= 0)
        caps.dpiY = 600;

    const int physicalWidth = ::GetDeviceCaps(dc, PHYSICALWIDTH);
    const int physicalHeight = ::GetDeviceCaps(dc, PHYSICALHEIGHT);

    if (physicalWidth > 0 && physicalHeight > 0)
    {
        caps.paperWidthMm = (physicalWidth * 254 + caps.dpiX * 5) / (caps.dpiX * 10);
        caps.paperHeightMm = (physicalHeight * 254 + caps.dpiY * 5) / (caps.dpiY * 10);
    }

    const std::wstring& name = printerName;

    // 彩色能力：DC_COLORDEVICE 返回 1（彩色）/ 0（单色）
    caps.color =
        (::DeviceCapabilitiesW(name.c_str(), nullptr, DC_COLORDEVICE, nullptr, nullptr) > 0);

    // 双面：1 表示支持
    caps.duplex =
        (::DeviceCapabilitiesW(name.c_str(), nullptr, DC_DUPLEX, nullptr, nullptr) > 0);

    // 纸张列表：DC_PAPERS 给 DMPAPER_* 数组，DC_PAPERSIZE 给 1/10 mm 尺寸
    const int paperCount =
        ::DeviceCapabilitiesW(name.c_str(), nullptr, DC_PAPERS, nullptr, nullptr);

    if (paperCount > 0)
    {
        std::vector<WORD> ids((size_t)paperCount);
        std::vector<POINT> sizes((size_t)paperCount);

        const int gotIds = ::DeviceCapabilitiesW(name.c_str(), nullptr, DC_PAPERS,
                                                 reinterpret_cast<LPWSTR>(ids.data()), nullptr);
        const int gotSizes =
            ::DeviceCapabilitiesW(name.c_str(), nullptr, DC_PAPERSIZE,
                                  reinterpret_cast<LPWSTR>(sizes.data()), nullptr);

        if (gotIds > 0 && gotSizes > 0)
        {
            const int count = (gotIds < gotSizes) ? gotIds : gotSizes;

            for (int i = 0; i < count && caps.mediaKeywords.size() < 12; ++i)
            {
                const std::string keyword = MapPaperToIppMedia((int)ids[(size_t)i],
                                                               sizes[(size_t)i].x,
                                                               sizes[(size_t)i].y);
                if (keyword.empty())
                    continue;

                if (std::find(caps.mediaKeywords.begin(), caps.mediaKeywords.end(), keyword) ==
                    caps.mediaKeywords.end())
                {
                    caps.mediaKeywords.push_back(keyword);
                }
            }
        }
    }

    ::DeleteDC(dc);

    caps.valid = true;
    return true;
}

void EnhanceForPrint(RasterPage& page, bool monochromeOutput)
{
    if (!page.IsValid())
        return;

    // 彩色输出不做黑白化：PDFium 的原始渲染已足够，
    // 二值化会把彩色图与灰阶渐变压成硬边黑白，在彩色机上属于倒退。
    if (!monochromeOutput)
        return;

    const int width = page.width;
    const int height = page.height;

    // 第一步：3x3 非锐化掩模。
    // 激光打印时笔画偏轻、边缘灰阶宽会显得"发虚"，先提高边缘对比度，
    // 再交给下面的黑白场压缩，轮廓会明显更实。
    //
    // 内存：只保留相邻三行的原始像素（3 × width × 4 字节），
    // 不再整页拷贝——A4@600 的整页副本是 139 MB，会让单页峰值从 139 MB 涨到 278 MB。
    if (width > 2 && height > 2)
    {
        const size_t rowBytes = (size_t)width * 4;
        unsigned char* pixels = page.bgra.data();

        std::vector<unsigned char> prevRow(rowBytes);
        std::vector<unsigned char> curRow(rowBytes);
        std::vector<unsigned char> nextRow(rowBytes);

        memcpy(prevRow.data(), pixels, rowBytes);
        memcpy(curRow.data(), pixels + rowBytes, rowBytes);
        memcpy(nextRow.data(), pixels + rowBytes * 2, rowBytes);

        for (int y = 1; y < height - 1; ++y)
        {
            unsigned char* dstRow = pixels + (size_t)y * rowBytes;

            for (int x = 1; x < width - 1; ++x)
            {
                const size_t offset = (size_t)x * 4;
                unsigned char* dst = dstRow + offset;

                for (int c = 0; c < 3; ++c)
                {
                    const int center = curRow[offset + c];
                    const int up = prevRow[offset + c];
                    const int down = nextRow[offset + c];
                    const int left = curRow[offset - 4 + c];
                    const int right = curRow[offset + 4 + c];

                    const int blur = (up + down + left + right + 2) / 4;
                    int value = center + (center - blur);   // 强度 1.0

                    if (value < 0)
                        value = 0;
                    else if (value > 255)
                        value = 255;

                    dst[c] = (unsigned char)value;
                }
            }

            // 三行缓冲整体下移，再把下一行原始数据读进来
            if (y + 2 < height)
            {
                std::swap(prevRow, curRow);
                std::swap(curRow, nextRow);
                memcpy(nextRow.data(), pixels + (size_t)(y + 2) * rowBytes, rowBytes);
            }
        }
    }

    unsigned char* pixels = page.bgra.data();
    const size_t count = (size_t)page.width * (size_t)page.height;

    for (size_t i = 0; i < count; ++i)
    {
        unsigned char* p = pixels + i * 4;   // B, G, R, A

        const int b = p[0];
        const int g = p[1];
        const int r = p[2];

        const int maxC = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
        const int minC = (r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b);

        if (monochromeOutput)
        {
            const int luma = (r * 299 + g * 587 + b * 114) / 1000;

            if (maxC - minC <= 24)
            {
                // 中性灰（正文/线条）：二值化，保持笔画锐利
                const unsigned char value = (luma >= 150) ? 255 : 0;
                p[0] = value;
                p[1] = value;
                p[2] = value;
            }
            else
            {
                // 彩色（标题/图标）：自己转灰度交给驱动网点。
                // 不做二值化 —— 纯黑会比 USB 直打重得多；
                // 也不原样留给驱动 —— 驱动会走色彩管理，蓝色转出来偏浅。
                // 再乘 0.85 略微加深：纯亮度的中间调落在网点上偏淡一档。
                int tone = (int)(luma * 0.85 + 0.5);
                if (tone < 0)
                    tone = 0;
                if (tone > 255)
                    tone = 255;

                p[0] = (unsigned char)tone;
                p[1] = (unsigned char)tone;
                p[2] = (unsigned char)tone;
            }

            continue;
        }

        if (maxC - minC <= 24)
        {
            // 中性灰（文字、线条、黑框）：直接二值化。
            // 黑白激光打印机没有灰度，最终都靠网点抖动模拟灰度；抗锯齿产生的
            // 灰阶边缘被网点化后，视觉上就是"笔画发虚、轮廓不清"。
            // 这里直接给纯黑/纯白，驱动按 100% / 0% 覆盖率输出，不再抖动。
            const int luma = (r * 299 + g * 587 + b * 114) / 1000;
            const unsigned char value = (luma >= 150) ? 255 : 0;
            p[0] = value;
            p[1] = value;
            p[2] = value;
        }
        else if (minC >= 235)
        {
            // 近白彩色 → 纯白：去掉浅色底纹/水印带来的灰底
            p[0] = 255;
            p[1] = 255;
            p[2] = 255;
        }
        else if (maxC <= 90)
        {
            // 深彩色（如深蓝图标）→ 纯黑，避免转灰后偏淡
            p[0] = 0;
            p[1] = 0;
            p[2] = 0;
        }
        // 其余彩色保持原样，由驱动转灰度
    }
}

bool GetPrinterResolution(const std::string& printerNameUtf8, int& dpiX, int& dpiY)
{
    dpiX = 0;
    dpiY = 0;

    const std::wstring printerName = Utf8ToWide(printerNameUtf8);
    if (printerName.empty())
        return false;

    HDC dc = ::CreateDCW(L"WINSPOOL", printerName.c_str(), nullptr, nullptr);
    if (dc == nullptr)
        return false;

    dpiX = ::GetDeviceCaps(dc, LOGPIXELSX);
    dpiY = ::GetDeviceCaps(dc, LOGPIXELSY);

    ::DeleteDC(dc);

    return dpiX > 0 && dpiY > 0;
}

bool PrintRasterPagesViaGdi(const std::string& printerNameUtf8,
                            const std::vector<RasterPage>& pages,
                            const std::string& documentName, std::string& error)
{
    if (pages.empty())
    {
        error = "没有页面";
        return false;
    }

    const std::vector<RasterPage>* source = &pages;
    return PrintPagesViaGdi(
        printerNameUtf8, pages.size(),
        [source](size_t index, std::string&) -> const RasterPage* {
            if (index >= source->size())
                return nullptr;
            return &(*source)[index];
        },
        documentName, error);
}

// ===========================================================================
// 自检页
// ===========================================================================

namespace
{

void SetPixel(RasterPage& page, int x, int y, unsigned char r, unsigned char g, unsigned char b)
{
    if (x < 0 || y < 0 || x >= page.width || y >= page.height)
        return;

    unsigned char* pixel = page.bgra.data() + ((size_t)y * page.width + x) * 4;
    pixel[0] = b;
    pixel[1] = g;
    pixel[2] = r;
    pixel[3] = 255;
}

void FillRect(RasterPage& page, int x, int y, int w, int h,
              unsigned char r, unsigned char g, unsigned char b)
{
    for (int dy = 0; dy < h; ++dy)
    {
        for (int dx = 0; dx < w; ++dx)
            SetPixel(page, x + dx, y + dy, r, g, b);
    }
}

// Bresenham 直线
void DrawLine(RasterPage& page, int x0, int y0, int x1, int y1,
              unsigned char r, unsigned char g, unsigned char b, int thickness)
{
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    for (;;)
    {
        FillRect(page, x0 - thickness / 2, y0 - thickness / 2, thickness, thickness, r, g, b);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

} // namespace

RasterPage MakeTestPage(int width, int height, int dpi)
{
    RasterPage page;
    page.width = (width > 0) ? width : 2480;
    page.height = (height > 0) ? height : 3508;
    page.dpiX = (dpi > 0) ? dpi : 300;
    page.dpiY = page.dpiX;
    page.bgra.assign((size_t)page.width * page.height * 4, 0xFF);   // 白底

    int margin = page.width / 40;
    int border = 6;

    // 外边框（判断四边是否有裁切、边距是否对称）
    FillRect(page, margin, margin, page.width - margin * 2, border, 0, 0, 0);
    FillRect(page, margin, page.height - margin - border, page.width - margin * 2, border, 0, 0, 0);
    FillRect(page, margin, margin, border, page.height - margin * 2, 0, 0, 0);
    FillRect(page, page.width - margin - border, margin, border, page.height - margin * 2, 0, 0, 0);

    // 对角线（判断缩放/方向是否正确）
    DrawLine(page, margin, margin, page.width - margin, page.height - margin, 0x99, 0x99, 0x99, 3);
    DrawLine(page, page.width - margin, margin, margin, page.height - margin, 0x99, 0x99, 0x99, 3);

    // 四角实心方块（判断方向：左上=红，右上=绿，左下=蓝，右下=黑）
    int box = page.width / 24;
    FillRect(page, margin + 40, margin + 40, box, box, 0xE0, 0x20, 0x20);
    FillRect(page, page.width - margin - 40 - box, margin + 40, box, box, 0x20, 0xC0, 0x20);
    FillRect(page, margin + 40, page.height - margin - 40 - box, box, box, 0x20, 0x40, 0xE0);
    FillRect(page, page.width - margin - 40 - box, page.height - margin - 40 - box, box, box, 0, 0, 0);

    // 顶部刻度线：每 100 像素一根，每 500 像素加粗（可核对水平比例）
    int rulerY = margin + 120;
    for (int x = margin; x < page.width - margin; x += 100)
    {
        bool major = ((x - margin) % 500 == 0);
        FillRect(page, x, rulerY, 3, major ? 60 : 28, 0, 0, 0);
    }
    FillRect(page, margin, rulerY + 70, page.width - margin * 2, 3, 0, 0, 0);

    return page;
}

} // namespace IppRaster
} // namespace ps

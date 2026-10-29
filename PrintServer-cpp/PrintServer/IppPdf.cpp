// PDF 栅格化（Phase B：IPP 免驱打印）
//
// 改用 PDFium（Chrome/Edge 同款 PDF 引擎，BSD-3-Clause）渲染：
//   原来用 Windows.Data.Pdf，小字与细线偏软，打印后轮廓发虚；
//   PDFium 的字形渲染与 Adobe Reader 接近，600 dpi 输出基本等同 USB 直打。
//
// 动态加载：只在运行时 LoadLibrary("pdfium.dll")（与 exe 同目录）。
//   - 找不到 dll 时 IsAvailable() 返回 false，IPP 服务自动降级为 PDL 直通，
//     不会出现"漏拷 dll 导致整个服务起不来"的情况。
//   - 不引入导入库，因此 vcxproj 只多了把 dll 复制到输出目录的步骤。
//
// 许可：PDFium 为 BSD-3-Clause，可随包分发，需保留其版权声明
//       （third_party/pdfium/LICENSE 与 licenses/ 目录随包保留）。
//
// 注意：从 IppPdf.cpp 原 Windows.Data.Pdf 实现切换而来，接口未变；
//       旧实现见源码归档 v1.4.1。

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "IppPdf.h"
#include "IppRaster.h"

namespace ps
{
namespace IppPdf
{

namespace
{

// ===========================================================================
// PDFium 动态绑定（只用 C 接口，头文件不参与编译）
// ===========================================================================

struct FpdfLibraryConfig
{
    int version;
    const char** userFontPaths;
    void* isolate;
    unsigned int v8EmbedderSlot;
    void* platform;
};

struct PdfiumApi
{
    HMODULE module = nullptr;
    bool ready = false;

    void (*initLibrary)(const FpdfLibraryConfig*) = nullptr;
    void* (*loadDocument)(const char* path, const char* password) = nullptr;
    void (*closeDocument)(void* document) = nullptr;
    int (*getPageCount)(void* document) = nullptr;
    void* (*loadPage)(void* document, int index) = nullptr;
    void (*closePage)(void* page) = nullptr;
    float (*getPageWidth)(void* page) = nullptr;
    float (*getPageHeight)(void* page) = nullptr;
    void* (*bitmapCreate)(int width, int height, int format, void* buffer, int stride) = nullptr;
    void (*bitmapDestroy)(void* bitmap) = nullptr;
    void (*renderPageBitmap)(void* bitmap, void* page, int startX, int startY,
                             int sizeX, int sizeY, int rotate, int flags) = nullptr;
};

const int kBitmapBgra = 4;   // FPDFBitmap_BGRA
const int kPageMargin = 0;

PdfiumApi& Api()
{
    static PdfiumApi api;
    static std::once_flag once;

    std::call_once(once, []() {
        // dll 必须与可执行文件同目录
        wchar_t path[MAX_PATH * 2] = { 0 };
        DWORD length = ::GetModuleFileNameW(nullptr, path, _countof(path));

        std::wstring directory(path, length);
        const size_t slash = directory.find_last_of(L'\\');
        if (slash == std::wstring::npos)
            return;

        directory.resize(slash + 1);

        const std::wstring dllPath = directory + L"pdfium.dll";
        api.module = ::LoadLibraryW(dllPath.c_str());
        if (api.module == nullptr)
            return;

        api.initLibrary = reinterpret_cast<decltype(api.initLibrary)>(
            ::GetProcAddress(api.module, "FPDF_InitLibraryWithConfig"));
        api.loadDocument = reinterpret_cast<decltype(api.loadDocument)>(
            ::GetProcAddress(api.module, "FPDF_LoadDocument"));
        api.closeDocument = reinterpret_cast<decltype(api.closeDocument)>(
            ::GetProcAddress(api.module, "FPDF_CloseDocument"));
        api.getPageCount = reinterpret_cast<decltype(api.getPageCount)>(
            ::GetProcAddress(api.module, "FPDF_GetPageCount"));
        api.loadPage = reinterpret_cast<decltype(api.loadPage)>(
            ::GetProcAddress(api.module, "FPDF_LoadPage"));
        api.closePage = reinterpret_cast<decltype(api.closePage)>(
            ::GetProcAddress(api.module, "FPDF_ClosePage"));
        api.getPageWidth = reinterpret_cast<decltype(api.getPageWidth)>(
            ::GetProcAddress(api.module, "FPDF_GetPageWidthF"));
        api.getPageHeight = reinterpret_cast<decltype(api.getPageHeight)>(
            ::GetProcAddress(api.module, "FPDF_GetPageHeightF"));
        api.bitmapCreate = reinterpret_cast<decltype(api.bitmapCreate)>(
            ::GetProcAddress(api.module, "FPDFBitmap_CreateEx"));
        api.bitmapDestroy = reinterpret_cast<decltype(api.bitmapDestroy)>(
            ::GetProcAddress(api.module, "FPDFBitmap_Destroy"));
        api.renderPageBitmap = reinterpret_cast<decltype(api.renderPageBitmap)>(
            ::GetProcAddress(api.module, "FPDF_RenderPageBitmap"));

        const bool complete = api.initLibrary != nullptr && api.loadDocument != nullptr &&
                              api.closeDocument != nullptr && api.getPageCount != nullptr &&
                              api.loadPage != nullptr && api.closePage != nullptr &&
                              api.getPageWidth != nullptr && api.getPageHeight != nullptr &&
                              api.bitmapCreate != nullptr && api.bitmapDestroy != nullptr &&
                              api.renderPageBitmap != nullptr;

        if (!complete)
        {
            ::FreeLibrary(api.module);
            api.module = nullptr;
            return;
        }

        FpdfLibraryConfig config;
        memset(&config, 0, sizeof(config));
        config.version = 2;

        api.initLibrary(&config);
        api.ready = true;
    });

    return api;
}

// PDFium 不是线程安全的：多台打印机并行渲染时必须串行
std::mutex& RenderMutex()
{
    static std::mutex mutex;
    return mutex;
}

} // namespace

bool IsAvailable()
{
    static std::atomic<int> cached(-1);

    int value = cached.load();
    if (value >= 0)
        return value != 0;

    const bool available = Api().ready;
    cached.store(available ? 1 : 0);
    return available;
}

// ===========================================================================
// Document
// ===========================================================================

struct Document::Impl
{
    void* document = nullptr;
    int pageCount = 0;
    bool opened = false;
};

Document::Document() : m_impl(new Impl())
{
}

Document::~Document()
{
    Close();
}

void Document::Close()
{
    if (m_impl == nullptr)
        return;

    if (m_impl->document != nullptr)
    {
        std::lock_guard<std::mutex> guard(RenderMutex());
        Api().closeDocument(m_impl->document);
        m_impl->document = nullptr;
    }

    m_impl->pageCount = 0;
    m_impl->opened = false;
}

bool Document::Open(const std::string& pdfPathUtf8, std::string& error)
{
    error.clear();
    Close();

    if (!IsAvailable())
    {
        error = "未找到 pdfium.dll（需与 PrintServer.exe 同目录），PDF 免驱渲染不可用";
        return false;
    }

    if (pdfPathUtf8.empty())
    {
        error = "PDF 路径为空";
        return false;
    }

    PdfiumApi& api = Api();

    std::lock_guard<std::mutex> guard(RenderMutex());

    void* document = api.loadDocument(pdfPathUtf8.c_str(), nullptr);
    if (document == nullptr)
    {
        error = "PDF 解析失败（文件损坏或加密）";
        return false;
    }

    const int pages = api.getPageCount(document);
    if (pages <= 0)
    {
        api.closeDocument(document);
        error = "PDF 中没有页面";
        return false;
    }

    m_impl->document = document;
    m_impl->pageCount = pages;
    m_impl->opened = true;
    return true;
}

bool Document::IsOpen() const
{
    return m_impl != nullptr && m_impl->opened && m_impl->document != nullptr;
}

int Document::PageCount() const
{
    if (!IsOpen())
        return 0;
    return m_impl->pageCount;
}

bool Document::GetPageSize(int index, double& widthPt, double& heightPt) const
{
    widthPt = 0.0;
    heightPt = 0.0;

    if (!IsOpen() || index < 0 || index >= m_impl->pageCount)
        return false;

    PdfiumApi& api = Api();

    std::lock_guard<std::mutex> guard(RenderMutex());

    void* pdfPage = api.loadPage(m_impl->document, index);
    if (pdfPage == nullptr)
        return false;

    widthPt = (double)api.getPageWidth(pdfPage);
    heightPt = (double)api.getPageHeight(pdfPage);

    api.closePage(pdfPage);

    return widthPt > 0.0 && heightPt > 0.0;
}

bool Document::RenderPage(int index, int targetDpi, IppRaster::RasterPage& page,
                          std::string& error)
{
    error.clear();
    page = IppRaster::RasterPage();

    if (!IsOpen())
    {
        error = "PDF 未打开";
        return false;
    }

    if (index < 0 || index >= m_impl->pageCount)
    {
        error = "页码越界";
        return false;
    }

    if (targetDpi < 72)
        targetDpi = 72;

    PdfiumApi& api = Api();

    std::lock_guard<std::mutex> guard(RenderMutex());

    void* pdfPage = api.loadPage(m_impl->document, index);
    if (pdfPage == nullptr)
    {
        error = "无法加载 PDF 页面";
        return false;
    }

    // PDF 页面尺寸单位是 1/72 英寸
    const double widthPt = (double)api.getPageWidth(pdfPage);
    const double heightPt = (double)api.getPageHeight(pdfPage);

    int width = (int)(widthPt / 72.0 * (double)targetDpi + 0.5);
    int height = (int)(heightPt / 72.0 * (double)targetDpi + 0.5);

    if (width <= 0)
        width = 1;
    if (height <= 0)
        height = 1;

    page.width = width;
    page.height = height;
    page.dpiX = targetDpi;
    page.dpiY = targetDpi;
    page.bgra.assign((size_t)width * (size_t)height * 4, 255);   // 白底

    void* bitmap = api.bitmapCreate(width, height, kBitmapBgra, page.bgra.data(), width * 4);
    if (bitmap == nullptr)
    {
        api.closePage(pdfPage);
        error = "创建 PDFium 位图失败（内存不足）";
        page = IppRaster::RasterPage();
        return false;
    }

    // flags = 0：不做 LCD 子像素渲染（那是给屏幕看的，打印会让彩色边缘变脏）
    api.renderPageBitmap(bitmap, pdfPage, kPageMargin, kPageMargin, width, height, 0, 0);

    api.bitmapDestroy(bitmap);
    api.closePage(pdfPage);

    return true;
}

} // namespace IppPdf
} // namespace ps

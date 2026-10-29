#pragma once

// PDF 栅格化（Phase B：免驱打印）
//
// 1.4.3 用 PDFium（BSD-3-Clause，动态加载 pdfium.dll）把 PDF 渲染成位图，
// 再交给 IppRaster 走 GDI 输出到本地驱动 → 打印机。
// （早期版本用 Windows.Data.Pdf，小字偏软，已替换。）
//
// 意义：Windows 10/11 的 "Microsoft IPP Class Driver"、macOS/iOS 的 AirPrint、
//       安卓的 Mopria，发来的首选格式就是 PDF。支持 PDF 后客户端**无需安装
//       厂商驱动**，现有"抓驱动包 + 数字签名 + 提权安装"那条链路退化为兜底。
//
// 依赖取舍：只用系统自带能力 —— 不引入 Ghostscript / MuPDF / Poppler
//           （它们的许可证为 AGPL / AGPL / GPL，与本项目的 MIT 许可不兼容）。
//
// 限制：Win7/2008 系统不带 PDF 栅格化器，此时只能宣告 PDL 直通格式。

#include <memory>
#include <string>

#include "IppRaster.h"

namespace ps
{
namespace IppPdf
{

// 当前系统是否具备内置 PDF 栅格化能力（结果会缓存）
bool IsAvailable();

// 一份已打开的 PDF。按页渲染，避免整份文档的位图同时驻留内存。
class Document
{
public:
    Document();
    ~Document();

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    bool Open(const std::string& pdfPathUtf8, std::string& error);
    void Close();

    bool IsOpen() const;
    int PageCount() const;

    // 读取页面尺寸（单位：点，1/72 英寸）。
    // 用于决定输出纸张与方向：纸张跟随 PDF 页面，而不是服务端驱动的默认纸张。
    bool GetPageSize(int index, double& widthPt, double& heightPt) const;

    // 渲染第 index 页（0 基）到 page；targetDpi 同时写入 page.dpiX/dpiY，
    // 使 XPS 页面尺寸与原 PDF 页面一致。
    bool RenderPage(int index, int targetDpi, IppRaster::RasterPage& page, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace IppPdf
} // namespace ps

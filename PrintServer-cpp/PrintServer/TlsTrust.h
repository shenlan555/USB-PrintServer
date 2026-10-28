#pragma once

// 授权服务器 HTTPS 证书链自校验（Windows 7 兼容）
//
// 背景：授权服务器 https://ps.itlog.vip 使用 Let's Encrypt 证书，证书链为
//         叶证书 -> YR1 -> Root YR -> ISRG Root X1（自签根）
//       Windows 7 SP1 若长期未联网更新根证书库，系统 Root 存储中没有 ISRG Root X1，
//       系统链校验会以"未知颁发机构"失败，WinHTTP 返回 12175
//       (ERROR_WINHTTP_SECURE_FAILURE)，在线激活/校验全部不可用。
//
// 解决方式（不修改系统信任库、不安装任何证书、不依赖 Win7 补丁）：
//   1. 请求失败且错误码为 ERROR_WINHTTP_SECURE_FAILURE 时，用一次"仅取证书"的探测连接
//      （探测连接允许忽略证书错误，因为证书随后由我们自己判定）取得服务器证书；
//   2. 以 hExclusiveRoot = 内嵌根证书存储 建立专用链引擎，独立完成一次完整校验：
//      链信任（锚点必须是内嵌根证书）、主机名、有效期、服务器用途；
//      注意此时系统的 Root 存储是被忽略的，也就是说"通过"等价于
//      "该证书确实由内嵌根证书签发且其它各项均合法"；
//   3. 只有第 2 步通过时，才判定需要放宽，由调用方对本次请求设置
//      SECURITY_FLAG_IGNORE_UNKNOWN_CA 后重试一次。任何其它错误
//      （主机名不符、证书过期、用途不符、由其它根签发）都不会放宽。
//
// 结果（需要放宽）按主机名缓存，同一进程内每个主机最多探测一次。

#include <string>
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>

namespace ps
{
namespace TlsTrust
{
    // 该主机是否"只缺根证书"：是则返回 true，调用方可放宽 unknown-CA 后重试一次。
    // 需要网络访问；探测失败（网络不通等）返回 false。
    bool HostNeedsEmbeddedRootRelax(const std::wstring& host, INTERNET_PORT port);

    // ---------------- 供自检使用 ----------------

    // 内嵌根证书数量
    size_t EmbeddedRootCount();

    // 内嵌根证书 DER 的 SHA-256（小写十六进制），用于自检时核对内容未被改动
    std::string EmbeddedRootSha256(size_t index);

    // 仅以"内嵌根证书"为信任锚校验（系统 Root 存储不参与）
    bool VerifyWithEmbeddedRootOnly(const CERT_CONTEXT* cert, const std::wstring& host);

    // 仅以"系统 Root 存储"校验（等价于系统默认行为）
    bool VerifyWithSystemStoreOnly(const CERT_CONTEXT* cert, const std::wstring& host);

    // 综合判定：0 = 系统存储即可信任；1 = 仅内嵌根证书可信任；-1 = 不通过
    int EvaluateCertificate(const CERT_CONTEXT* cert, const std::wstring& host);

    // 探测服务器证书（允许忽略证书错误，仅供随后自校验使用）；失败返回 nullptr，
    // 成功后由调用方用 CertFreeCertificateContext 释放
    PCCERT_CONTEXT ProbeServerCertificate(const std::wstring& host, INTERNET_PORT port);
}
}

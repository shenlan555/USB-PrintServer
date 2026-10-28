#pragma once

// 驱动包清单（manifest）的数字签名。
//
// 目的：打印包走的是明文 HTTP，仅靠清单内的 SHA256 无法防住中间人
//       （改包的人同样能改清单）。这里用非对称签名把清单钉死：
//         - 服务端持有私钥（内置），对清单载荷签名；
//         - 客户端只内置公钥，验签通过才允许安装驱动。
//
// 算法：RSA-2048 + SHA-256 + PKCS#1 v1.5
// 载荷格式由 BuildPayload 定义，服务端与客户端必须完全一致。

#include <string>
#include <vector>

namespace ps
{
namespace DriverSigning
{

// 待签名载荷（两侧构造逻辑必须逐字节一致）：
//   "PSD1\n" + driverName + "\n" + inf + "\n" + architecture + "\n" +
//   catalogFile + "\n" + 文件数 + "\n" +
//   每个文件: 相对路径 + "\n" + sha256(小写十六进制) + "\n"
std::string BuildPayload(const std::string& driverName,
                         const std::string& inf,
                         const std::string& architecture,
                         const std::string& catalogFile,
                         const std::vector<std::string>& filePaths,
                         const std::vector<std::string>& fileHashes);

// 服务端：用内置私钥签名，返回 Base64；失败返回空串。
// 客户端不参与编译该函数（客户端只验签，不持有私钥）。
std::string Sign(const std::string& payload);

// 用内置公钥验签（服务端与客户端都可用）
bool Verify(const std::string& payload, const std::string& signatureBase64);

} // namespace DriverSigning
} // namespace ps

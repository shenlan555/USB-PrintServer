#pragma once

// 加解密辅助
//
// 只保留产品实际使用的原语：
//   - Base64 编解码
//   - MD5 / SHA-256 摘要
//   - HMAC-SHA256（/api/driver 鉴权）
//   - RSA 签名验证（SHA256 + PKCS#1 v1.5，公钥为 MS XML 格式，用于驱动包清单）
//
// 失败时返回空串 / false，而不是抛异常。

#include <string>
#include <vector>

namespace ps
{
namespace Crypto
{
    // ---------------- Base64 ----------------
    std::string Base64Encode(const std::vector<unsigned char>& data);
    bool Base64Decode(const std::string& text, std::vector<unsigned char>& out);

    // ---------------- 摘要 ----------------
    std::vector<unsigned char> Sha256(const std::vector<unsigned char>& data);
    std::vector<unsigned char> Sha256(const std::string& utf8);
    std::vector<unsigned char> Md5(const std::string& utf8);

    // ---------------- RSA 签名验证 ----------------
    // publicKey 为 MS XML 格式（<Modulus>/<Exponent>）
    bool VerifySignature(const std::string& data, const std::string& signature,
                         const std::string& publicKeyXml);

    // ---------------- HMAC-SHA256 ----------------
    // 返回 Base64；失败返回空串
    std::string GenerateHmacSignature(const std::string& data, const std::string& secret);
    // OrdinalIgnoreCase 比较
    bool VerifyHmacSignature(const std::string& data, const std::string& signature,
                             const std::string& secret);
}
}

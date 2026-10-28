#pragma once

// 加解密辅助类（对应 C# CryptoHelper.cs）
//
// 实现要点与 .NET 版严格对齐：
//   - HMAC-SHA256 → Base64
//   - AES-256-CBC + PKCS#7 填充
//   - DPAPI 用于本地授权缓存 license.dat（本机作用域写入，用户/本机作用域均可读取）
//   - RSA 签名验证（SHA256 + PKCS#1 v1.5，公钥为 MS XML 格式）
//
// 失败时与 C# 一致地返回空串 / false，而不是抛异常。

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

    // ---------------- 授权服务器地址 ----------------
    // 对应 C# CryptoHelper.DecodeLicenseUrl()
    std::string DecodeLicenseUrl();

    // ---------------- RSA 签名验证 ----------------
    // 对应 C# CryptoHelper.VerifySignature()，publicKey 为 MS XML 格式
    bool VerifySignature(const std::string& data, const std::string& signature,
                         const std::string& publicKeyXml);

    // ---------------- HMAC-SHA256 ----------------
    // 对应 C# CryptoHelper.GenerateHmacSignature()：返回 Base64；失败返回空串
    std::string GenerateHmacSignature(const std::string& data, const std::string& secret);
    // 对应 C# CryptoHelper.VerifyHmacSignature()：OrdinalIgnoreCase 比较
    bool VerifyHmacSignature(const std::string& data, const std::string& signature,
                             const std::string& secret);

    // -------- AES-256-CBC（本地固定密钥，仅用于本地存储）--------
    // 对应 C# AesEncryptToBase64 / AesDecryptFromBase64
    std::string AesEncryptToBase64(const std::string& plainText);
    std::string AesDecryptFromBase64(const std::string& encryptedBase64);

    // -------- AES-256-CBC（与服务端 PHP 一致，用于 .lic 文件）--------
    // Key = SHA256(secret)，IV = SHA256(secret + "iv_salt") 的前 16 字节
    std::string AesEncryptWithSecret(const std::string& plainText, const std::string& secret);
    std::string AesDecryptWithSecret(const std::string& encryptedBase64, const std::string& secret);

    // ---------------- DPAPI（本地授权缓存）----------------
    // 写入使用本机作用域（界面与服务账户不同，必须都能解密）；
    // 读取先尝试用户作用域（兼容 .NET 版缓存）再尝试本机作用域。
    std::string EncryptToBase64(const std::string& plainText);
    std::string DecryptFromBase64(const std::string& encryptedBase64);

    // ---------------- 离线 .lic 文件解密 ----------------
    // 先 HMAC 验签，再 AES 解密；成功返回解密后的 JSON，失败返回空串
    std::string DecryptLicenseFile(const std::string& encryptedData, const std::string& signature,
                                   const std::string& secret);
}
}

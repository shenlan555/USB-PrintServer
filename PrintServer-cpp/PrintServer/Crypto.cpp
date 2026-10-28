#include "pch.h"
#include "Crypto.h"
#include "BuildConfig.h"

#include <bcrypt.h>
#include <wincrypt.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif

namespace ps
{
namespace Crypto
{

// ===========================================================================
// Base64
// ===========================================================================

namespace
{
const char kB64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int B64Value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
}

std::string Base64Encode(const std::vector<unsigned char>& data)
{
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 2 < data.size())
    {
        unsigned int n = ((unsigned int)data[i] << 16) | ((unsigned int)data[i + 1] << 8) | data[i + 2];
        out.push_back(kB64Chars[(n >> 18) & 0x3F]);
        out.push_back(kB64Chars[(n >> 12) & 0x3F]);
        out.push_back(kB64Chars[(n >> 6) & 0x3F]);
        out.push_back(kB64Chars[n & 0x3F]);
        i += 3;
    }

    size_t remain = data.size() - i;
    if (remain == 1)
    {
        unsigned int n = (unsigned int)data[i] << 16;
        out.push_back(kB64Chars[(n >> 18) & 0x3F]);
        out.push_back(kB64Chars[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    }
    else if (remain == 2)
    {
        unsigned int n = ((unsigned int)data[i] << 16) | ((unsigned int)data[i + 1] << 8);
        out.push_back(kB64Chars[(n >> 18) & 0x3F]);
        out.push_back(kB64Chars[(n >> 12) & 0x3F]);
        out.push_back(kB64Chars[(n >> 6) & 0x3F]);
        out.push_back('=');
    }

    return out;
}

bool Base64Decode(const std::string& text, std::vector<unsigned char>& out)
{
    out.clear();
    out.reserve((text.size() / 4) * 3);

    int acc = 0;
    int accBits = 0;

    for (char c : text)
    {
        if (c == '=' )
            break;
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
            continue; // XML 里的 base64 可能折行

        int v = B64Value(c);
        if (v < 0)
            return false;

        acc = (acc << 6) | v;
        accBits += 6;
        if (accBits >= 8)
        {
            accBits -= 8;
            out.push_back((unsigned char)((acc >> accBits) & 0xFF));
        }
    }

    return true;
}

// ===========================================================================
// 摘要算法（BCrypt）
// ===========================================================================

namespace
{
// hmacKey 为空表示普通摘要；非空表示 HMAC
bool HashData(LPCWSTR algId, const unsigned char* data, size_t len,
              const unsigned char* hmacKey, size_t hmacKeyLen,
              std::vector<unsigned char>& out)
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::vector<unsigned char> hashObj;
    ULONG objLen = 0, hashLen = 0, cb = 0;
    bool ok = false;

    do
    {
        ULONG flags = (hmacKey != nullptr) ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0;
        if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, algId, nullptr, flags)))
            break;

        if (!NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
            (PUCHAR)&objLen, sizeof(objLen), &cb, 0)))
            break;

        if (!NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
            (PUCHAR)&hashLen, sizeof(hashLen), &cb, 0)))
            break;

        hashObj.resize(objLen);

        if (!NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, hashObj.data(), (ULONG)hashObj.size(),
            (PUCHAR)hmacKey, (ULONG)hmacKeyLen, 0)))
            break;

        if (len > 0 && !NT_SUCCESS(BCryptHashData(hHash, (PUCHAR)data, (ULONG)len, 0)))
            break;

        out.resize(hashLen);
        if (!NT_SUCCESS(BCryptFinishHash(hHash, out.data(), hashLen, 0)))
            break;

        ok = true;
    } while (false);

    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}
}

std::vector<unsigned char> Sha256(const std::vector<unsigned char>& data)
{
    std::vector<unsigned char> out;
    HashData(BCRYPT_SHA256_ALGORITHM, data.empty() ? nullptr : data.data(), data.size(),
             nullptr, 0, out);
    return out;
}

std::vector<unsigned char> Sha256(const std::string& utf8)
{
    return Sha256(std::vector<unsigned char>(utf8.begin(), utf8.end()));
}

std::vector<unsigned char> Md5(const std::string& utf8)
{
    std::vector<unsigned char> out;
    HashData(BCRYPT_MD5_ALGORITHM,
             utf8.empty() ? nullptr : (const unsigned char*)utf8.data(), utf8.size(),
             nullptr, 0, out);
    return out;
}

// ===========================================================================
// 授权服务器地址
// ===========================================================================

std::string DecodeLicenseUrl()
{
    // 对应 C# 从程序集特性读取；这里读取编译期常量
    if (BuildConfig::LicenseUrl != nullptr && BuildConfig::LicenseUrl[0] != '\0')
        return BuildConfig::LicenseUrl;

    return BuildConfig::LicenseUrlFallback;
}

// ===========================================================================
// HMAC-SHA256
// ===========================================================================

std::string GenerateHmacSignature(const std::string& data, const std::string& secret)
{
    std::vector<unsigned char> mac;
    if (!HashData(BCRYPT_SHA256_ALGORITHM,
                  (const unsigned char*)data.data(), data.size(),
                  (const unsigned char*)secret.data(), secret.size(),
                  mac))
    {
        return std::string();
    }

    return Base64Encode(mac);
}

bool VerifyHmacSignature(const std::string& data, const std::string& signature,
                         const std::string& secret)
{
    std::string expected = GenerateHmacSignature(data, secret);
    if (expected.empty() || signature.empty())
        return false;

    // 对应 C#: string.Equals(expected, signature, StringComparison.OrdinalIgnoreCase)
    return _stricmp(expected.c_str(), signature.c_str()) == 0;
}

// ===========================================================================
// AES-256-CBC
// ===========================================================================

namespace
{
bool AesCbc(bool encrypt, const std::vector<unsigned char>& key,
            const std::vector<unsigned char>& iv,
            const std::vector<unsigned char>& input,
            std::vector<unsigned char>& output)
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    std::vector<unsigned char> keyObj;
    std::vector<unsigned char> ivCopy;
    ULONG objLen = 0, cb = 0, outLen = 0, needLen = 0;
    bool ok = false;

    do
    {
        if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0)))
            break;

        // CBC 模式
        if (!NT_SUCCESS(BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
            (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0)))
            break;

        if (!NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
            (PUCHAR)&objLen, sizeof(objLen), &cb, 0)))
            break;

        keyObj.resize(objLen);

        if (!NT_SUCCESS(BCryptGenerateSymmetricKey(hAlg, &hKey, keyObj.data(), (ULONG)keyObj.size(),
            (PUCHAR)key.data(), (ULONG)key.size(), 0)))
            break;

        ivCopy = iv;
        if (!NT_SUCCESS(BCryptSetProperty(hKey, BCRYPT_INITIALIZATION_VECTOR,
            ivCopy.data(), (ULONG)ivCopy.size(), 0)))
            break;

        ULONG inLen = (ULONG)input.size();
        const PUCHAR inBuf = input.empty() ? nullptr : (PUCHAR)input.data();

        // BCRYPT_BLOCK_PADDING → PKCS#7，与 .NET PaddingMode.PKCS7 / PHP openssl 默认一致
        NTSTATUS st;
        if (encrypt)
        {
            st = BCryptEncrypt(hKey, inBuf, inLen, nullptr, nullptr, 0,
                               nullptr, 0, &needLen, BCRYPT_BLOCK_PADDING);
            if (!NT_SUCCESS(st)) break;

            output.resize(needLen);
            st = BCryptEncrypt(hKey, inBuf, inLen, nullptr, nullptr, 0,
                               output.data(), (ULONG)output.size(), &outLen, BCRYPT_BLOCK_PADDING);
            if (!NT_SUCCESS(st)) break;
        }
        else
        {
            st = BCryptDecrypt(hKey, inBuf, inLen, nullptr, nullptr, 0,
                               nullptr, 0, &needLen, BCRYPT_BLOCK_PADDING);
            if (!NT_SUCCESS(st)) break;

            output.resize(needLen);
            st = BCryptDecrypt(hKey, inBuf, inLen, nullptr, nullptr, 0,
                               output.data(), (ULONG)output.size(), &outLen, BCRYPT_BLOCK_PADDING);
            if (!NT_SUCCESS(st)) break;
        }

        output.resize(outLen);
        ok = true;
    } while (false);

    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

// 对应 C# GetAes256Key()：SHA256(AesKey)
std::vector<unsigned char> GetAes256Key()
{
    static const char kLocalAesKey[] = "P5@ntS3rv3rK3y!9";
    return Sha256(std::string(kLocalAesKey));
}

// 对应 C# 的 AesIv：16 字节固定 IV
std::vector<unsigned char> GetLocalAesIv()
{
    static const char kLocalAesIv[] = "P5@ntS3rv3r1v@9!";
    return std::vector<unsigned char>(kLocalAesIv, kLocalAesIv + 16);
}
}

std::string AesEncryptToBase64(const std::string& plainText)
{
    std::vector<unsigned char> input(plainText.begin(), plainText.end());
    std::vector<unsigned char> output;

    if (!AesCbc(true, GetAes256Key(), GetLocalAesIv(), input, output))
        return std::string();

    return Base64Encode(output);
}

std::string AesDecryptFromBase64(const std::string& encryptedBase64)
{
    std::vector<unsigned char> input;
    if (!Base64Decode(encryptedBase64, input) || input.empty())
        return std::string();

    std::vector<unsigned char> output;
    if (!AesCbc(false, GetAes256Key(), GetLocalAesIv(), input, output))
        return std::string();

    return std::string(output.begin(), output.end());
}

std::string AesEncryptWithSecret(const std::string& plainText, const std::string& secret)
{
    // Key = SHA256(secret)；IV = SHA256(secret + "iv_salt") 的前 16 字节
    std::vector<unsigned char> key = Sha256(secret);

    std::vector<unsigned char> ivFull = Sha256(secret + "iv_salt");
    std::vector<unsigned char> iv(ivFull.begin(), ivFull.begin() + 16);

    std::vector<unsigned char> input(plainText.begin(), plainText.end());
    std::vector<unsigned char> output;

    if (!AesCbc(true, key, iv, input, output))
        return std::string();

    return Base64Encode(output);
}

std::string AesDecryptWithSecret(const std::string& encryptedBase64, const std::string& secret)
{
    std::vector<unsigned char> key = Sha256(secret);

    std::vector<unsigned char> ivFull = Sha256(secret + "iv_salt");
    std::vector<unsigned char> iv(ivFull.begin(), ivFull.begin() + 16);

    std::vector<unsigned char> input;
    if (!Base64Decode(encryptedBase64, input) || input.empty())
        return std::string();

    std::vector<unsigned char> output;
    if (!AesCbc(false, key, iv, input, output))
        return std::string();

    return std::string(output.begin(), output.end());
}

// ===========================================================================
// DPAPI（本地授权缓存 license.dat）
//
// 写入时使用"本机"作用域：界面程序以登录用户身份运行，服务以 LocalSystem 身份运行，
// 二者共用同一份 license.dat；用户作用域的密文在另一账户下无法解密，
// 会导致离线校验失败（授权服务器不可达时服务会直接启动失败）。
// 读取时先按用户作用域（兼容 .NET 版/旧版写下的缓存），再按本机作用域。
// ===========================================================================

namespace
{

bool TryUnprotect(const std::vector<unsigned char>& encrypted, DWORD extraFlags, std::string& result)
{
    DATA_BLOB in = { 0 };
    in.pbData = const_cast<BYTE*>(encrypted.data());
    in.cbData = (DWORD)encrypted.size();

    DATA_BLOB outBlob = { 0 };
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN | extraFlags, &outBlob))
        return false;

    result.assign((char*)outBlob.pbData, outBlob.cbData);
    LocalFree(outBlob.pbData);
    return true;
}

}

std::string EncryptToBase64(const std::string& plainText)
{
    DATA_BLOB in = { 0 };
    in.pbData = (BYTE*)plainText.data();
    in.cbData = (DWORD)plainText.size();

    DATA_BLOB outBlob = { 0 };
    // 与 .NET 一致：无描述、无额外熵；作用域为本机（同机所有账户均可解密）
    if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN | CRYPTPROTECT_LOCAL_MACHINE, &outBlob))
    {
        // 本机作用域失败时退回用户作用域
        if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &outBlob))
            return std::string();
    }

    std::vector<unsigned char> buf(outBlob.pbData, outBlob.pbData + outBlob.cbData);
    LocalFree(outBlob.pbData);

    return Base64Encode(buf);
}

std::string DecryptFromBase64(const std::string& encryptedBase64)
{
    std::vector<unsigned char> encrypted;
    if (!Base64Decode(encryptedBase64, encrypted) || encrypted.empty())
        return std::string();

    std::string result;
    if (TryUnprotect(encrypted, 0, result))
        return result;
    if (TryUnprotect(encrypted, CRYPTPROTECT_LOCAL_MACHINE, result))
        return result;

    return std::string();
}

// ===========================================================================
// RSA 签名验证（SHA256 + PKCS#1 v1.5，公钥为 MS XML 格式）
// ===========================================================================

namespace
{
// 从 XML 中提取 <Tag>...</Tag> 的内容
bool ExtractXmlTag(const std::string& xml, const char* tag, std::string& value)
{
    std::string open = std::string("<") + tag + ">";
    std::string close = std::string("</") + tag + ">";

    size_t start = xml.find(open);
    if (start == std::string::npos)
        return false;
    start += open.size();

    size_t end = xml.find(close, start);
    if (end == std::string::npos)
        return false;

    value = xml.substr(start, end - start);
    return true;
}
}

bool VerifySignature(const std::string& data, const std::string& signature,
                     const std::string& publicKeyXml)
{
    std::string modulusB64, exponentB64;
    if (!ExtractXmlTag(publicKeyXml, "Modulus", modulusB64) ||
        !ExtractXmlTag(publicKeyXml, "Exponent", exponentB64))
    {
        return false;
    }

    std::vector<unsigned char> modulus, exponent, sig;
    if (!Base64Decode(modulusB64, modulus) || modulus.empty())
        return false;
    if (!Base64Decode(exponentB64, exponent) || exponent.empty())
        return false;
    if (!Base64Decode(signature, sig) || sig.empty())
        return false;

    // 去掉大端序前导零
    size_t modOff = 0;
    while (modOff + 1 < modulus.size() && modulus[modOff] == 0)
        ++modOff;

    std::vector<unsigned char> mod(modulus.begin() + modOff, modulus.end());

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    bool verified = false;

    do
    {
        if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_RSA_ALGORITHM, nullptr, 0)))
            break;

        // 组装 BCRYPT_RSAKEY_BLOB（公钥）
        BCRYPT_RSAKEY_BLOB hdr = { 0 };
        hdr.Magic = BCRYPT_RSAPUBLIC_MAGIC;
        hdr.BitLength = (ULONG)(mod.size() * 8);
        hdr.cbPublicExp = (ULONG)exponent.size();
        hdr.cbModulus = (ULONG)mod.size();
        hdr.cbPrime1 = 0;
        hdr.cbPrime2 = 0;

        std::vector<unsigned char> blob;
        blob.resize(sizeof(hdr) + exponent.size() + mod.size());
        memcpy(blob.data(), &hdr, sizeof(hdr));
        memcpy(blob.data() + sizeof(hdr), exponent.data(), exponent.size());
        memcpy(blob.data() + sizeof(hdr) + exponent.size(), mod.data(), mod.size());

        if (!NT_SUCCESS(BCryptImportKeyPair(hAlg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &hKey,
            blob.data(), (ULONG)blob.size(), 0)))
            break;

        std::vector<unsigned char> hash = Sha256(data);
        if (hash.empty())
            break;

        NTSTATUS st = BCryptVerifySignature(hKey, nullptr, hash.data(), (ULONG)hash.size(),
                                           sig.data(), (ULONG)sig.size(), BCRYPT_PAD_PKCS1);
        verified = NT_SUCCESS(st);
    } while (false);

    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return verified;
}

// ===========================================================================
// 离线 .lic 文件解密
// ===========================================================================

std::string DecryptLicenseFile(const std::string& encryptedData, const std::string& signature,
                               const std::string& secret)
{
    // 1. 验证 HMAC 签名（对加密后的 data 验签）
    if (!VerifyHmacSignature(encryptedData, signature, secret))
        return std::string();

    // 2. 与服务端 PHP 一致的 AES-256-CBC 解密
    return AesDecryptWithSecret(encryptedData, secret);
}

}
}

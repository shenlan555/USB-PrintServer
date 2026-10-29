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

}
}

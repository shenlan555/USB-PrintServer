#include "pch.h"
#include "DriverSigning.h"

#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif

// 客户端只做验签：内置公钥，不持有私钥（Sign 不在本工程编译）。
// Base64 与 SHA-256 自带实现，避免把服务端的授权相关代码拉进客户端。

namespace ps
{
namespace DriverSigning
{

namespace
{

const char* const kPublicKeyXml =
    "<RSAKeyValue><Modulus>wtod0fOGpPHKsMj9Mn/XjAF+dPDTaZAaPzXwlGQxBnQOoKiCgN0JSW4D13y4f9GTDtv5eFlVD5CVeOk9jxW0h12ZEMXd6vJ4RNRefhIJSgwawk29mwvyL7HpQmMdBIy26JTcIq7V9OCOrOOscxS/nGZXq4AlpzvMJ5carUBpVlBs4qQB3IbaD6KSMeJ1Ndo8Ht4qCYWUxod7+EmJQyB2l2sfEDXbv69j+UXwYEV/xWVUtxjqrKKb6pxE5Iwj4MLDaretXjyyPtfwR0C3bqGXoq84ltz7CdeU52Yo7AJN1lhfPJ9xbZyn3fUXH42OZUIXcJXVqNxGiWFp3SK9lb3n4Q==</Modulus><Exponent>AQAB</Exponent></RSAKeyValue>";

// 从 XML 中提取 <Tag>...</Tag>
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

// ---- Base64 解码 ----
int Base64Value(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

bool Base64Decode(const std::string& text, std::vector<unsigned char>& out)
{
    out.clear();

    int buffer = 0;
    int bits = 0;

    for (char ch : text)
    {
        if (ch == '=' || ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t')
            continue;

        int value = Base64Value(ch);
        if (value < 0)
            return false;

        buffer = (buffer << 6) | value;
        bits += 6;

        if (bits >= 8)
        {
            bits -= 8;
            out.push_back((unsigned char)((buffer >> bits) & 0xFF));
        }
    }

    return true;
}

// ---- SHA-256（BCrypt；Windows Vista 起可用，Win7/2008 同样支持）----
std::vector<unsigned char> Sha256(const std::string& data)
{
    std::vector<unsigned char> digest;

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;

    do
    {
        if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                                   nullptr, 0)))
        {
            break;
        }

        DWORD objectLength = 0;
        DWORD hashLength = 0;
        ULONG written = 0;

        if (!NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                                         (PUCHAR)&objectLength, sizeof(objectLength),
                                         &written, 0)))
        {
            break;
        }
        if (!NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                                         (PUCHAR)&hashLength, sizeof(hashLength),
                                         &written, 0)))
        {
            break;
        }

        std::vector<unsigned char> hashObject(objectLength);

        if (!NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, hashObject.data(),
                                        (ULONG)hashObject.size(), nullptr, 0, 0)))
        {
            break;
        }

        if (!data.empty() &&
            !NT_SUCCESS(BCryptHashData(hHash, (PUCHAR)data.data(), (ULONG)data.size(), 0)))
        {
            break;
        }

        digest.resize(hashLength);
        if (!NT_SUCCESS(BCryptFinishHash(hHash, digest.data(), (ULONG)digest.size(), 0)))
        {
            digest.clear();
            break;
        }
    } while (false);

    if (hHash != nullptr)
        BCryptDestroyHash(hHash);
    if (hAlg != nullptr)
        BCryptCloseAlgorithmProvider(hAlg, 0);

    return digest;
}

} // namespace

std::string BuildPayload(const std::string& driverName,
                         const std::string& inf,
                         const std::string& architecture,
                         const std::string& catalogFile,
                         const std::vector<std::string>& filePaths,
                         const std::vector<std::string>& fileHashes)
{
    std::string payload;
    payload.reserve(256 + filePaths.size() * 96);

    payload += "PSD1\n";
    payload += driverName;
    payload += "\n";
    payload += inf;
    payload += "\n";
    payload += architecture;
    payload += "\n";
    payload += catalogFile;
    payload += "\n";
    payload += std::to_string(filePaths.size());
    payload += "\n";

    for (size_t i = 0; i < filePaths.size(); ++i)
    {
        payload += filePaths[i];
        payload += "\n";
        payload += (i < fileHashes.size()) ? fileHashes[i] : std::string();
        payload += "\n";
    }

    return payload;
}

bool Verify(const std::string& payload, const std::string& signatureBase64)
{
    std::string modulusB64, exponentB64;
    if (!ExtractXmlTag(kPublicKeyXml, "Modulus", modulusB64) ||
        !ExtractXmlTag(kPublicKeyXml, "Exponent", exponentB64))
    {
        return false;
    }

    std::vector<unsigned char> modulus, exponent, signature;
    if (!Base64Decode(modulusB64, modulus) || modulus.empty())
        return false;
    if (!Base64Decode(exponentB64, exponent) || exponent.empty())
        return false;
    if (!Base64Decode(signatureBase64, signature) || signature.empty())
        return false;

    // 去掉大端序前导零
    size_t offset = 0;
    while (offset + 1 < modulus.size() && modulus[offset] == 0)
        ++offset;
    std::vector<unsigned char> mod(modulus.begin() + offset, modulus.end());

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    bool verified = false;

    do
    {
        if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_RSA_ALGORITHM, nullptr, 0)))
            break;

        BCRYPT_RSAKEY_BLOB header = { 0 };
        header.Magic = BCRYPT_RSAPUBLIC_MAGIC;
        header.BitLength = (ULONG)(mod.size() * 8);
        header.cbPublicExp = (ULONG)exponent.size();
        header.cbModulus = (ULONG)mod.size();

        std::vector<unsigned char> blob(sizeof(header) + exponent.size() + mod.size());
        memcpy(blob.data(), &header, sizeof(header));
        memcpy(blob.data() + sizeof(header), exponent.data(), exponent.size());
        memcpy(blob.data() + sizeof(header) + exponent.size(), mod.data(), mod.size());

        if (!NT_SUCCESS(BCryptImportKeyPair(hAlg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &hKey,
                                           blob.data(), (ULONG)blob.size(), 0)))
            break;

        std::vector<unsigned char> hash = Sha256(payload);
        if (hash.empty())
            break;

        // 必须与签名侧对称：签名用 BCRYPT_PKCS1_PADDING_INFO(SHA256)，
        // 验签也必须带上同一 OID；传 nullptr 会按裸哈希处理而验签失败。
        BCRYPT_PKCS1_PADDING_INFO padding;
        padding.pszAlgId = BCRYPT_SHA256_ALGORITHM;

        verified = NT_SUCCESS(BCryptVerifySignature(hKey, &padding, hash.data(),
                                                   (ULONG)hash.size(), signature.data(),
                                                   (ULONG)signature.size(), BCRYPT_PAD_PKCS1));
    } while (false);

    if (hKey != nullptr)
        BCryptDestroyKey(hKey);
    if (hAlg != nullptr)
        BCryptCloseAlgorithmProvider(hAlg, 0);

    return verified;
}

} // namespace DriverSigning
} // namespace ps

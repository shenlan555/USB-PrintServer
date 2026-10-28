#include "pch.h"
#include "DriverSigning.h"
#include "Crypto.h"

#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif

namespace ps
{
namespace DriverSigning
{

namespace
{

// ===========================================================================
// 密钥材料
//
// 私钥：BCRYPT_RSAPRIVATE_BLOB（magic "RSA2"）的 Base64，仅服务端持有。
// 公钥：MS XML 格式（<RSAKeyValue><Modulus>..</Modulus><Exponent>..</Exponent>），
//       与服务端 Crypto::VerifySignature 的格式一致，客户端内置同一份。
//
// 换密钥：重新生成一对，私钥换到下面，公钥同步到
//         PrintClient-cpp/PrintClient/DriverSigning.cpp。
// ===========================================================================

const char* const kPrivateKeyBlobBase64 =
    "UlNBMgAIAAADAAAAAAEAAIAAAACAAAAAAQABwtod0fOGpPHKsMj9Mn/XjAF+dPDTaZAaPzXwlGQxBnQOoKiCgN0JSW4D13y4f9GTDtv5eFlVD5CVeOk9jxW0h12ZEMXd6vJ4RNRefhIJSgwawk29mwvyL7HpQmMdBIy26JTcIq7V9OCOrOOscxS/nGZXq4AlpzvMJ5carUBpVlBs4qQB3IbaD6KSMeJ1Ndo8Ht4qCYWUxod7+EmJQyB2l2sfEDXbv69j+UXwYEV/xWVUtxjqrKKb6pxE5Iwj4MLDaretXjyyPtfwR0C3bqGXoq84ltz7CdeU52Yo7AJN1lhfPJ9xbZyn3fUXH42OZUIXcJXVqNxGiWFp3SK9lb3n4ehjBjFtic019yCHpZIRYVW1Hx3MXS2hPA0zvkf7Kb7GhMv/zp1K7PNeYZFoQDInzNDg/87E4kIA9xkJDchqDP8T4dMCITUtJufHkECsN99mXfLw/q0s+Lzy9sN1pGRWLLHfMPClcmAnS4c+XG3kKsS7kfe1mQXT4yMhqqZw1kXn1qa3V/EQQx3LSzwOnyVbg3Q/MnZE3Pi1Rri21iFVIQqTwXoqpINQf/a78riw+t0zCD4A83sk+9+sYBwgszrH+RoUgvrTmfcn2/5seIGuc/gNBFCRGO4JollgnOW4ttF3Iuay1u6q7ctmTDca9swTFAPaIswxELO0KxvmniIBGvdEcG1xfyYXVq5aWTis5Qt6d2Ybm4VRm+uYo8WFY3Xu5n5TqhnKfzd7kS9A6KgHXWtXlPwhhCByeeBL8jup63m6YbufLkBjbdHIbyCPu2iVFYvTsZ//loAG/bMo62un1S1CkU3TgZZGtSvsP7Ru9MSXvfoGLuhLLBT30sF0VUT1v6mMKknmecfp4shcedgvM+fTjI+ZZ697LATeplzhG1Lr/eXXT5N+vbk19SM1y2V8D3E1OJ8Z6A6jkI6h6RppFcenWAhVg4qb9PoKLzfIhznG9tEvqkycyTNZkbycn5iYP5hrzVuETbNi9DzDUiMa8v7eRBE0ZJUivO9ILJM/nMvlRd+wmKIkaq/fbM7uj1V1xhTTCGhkvUEMdhAlTc1JPtuJlX1zW1tMCyLONZ/WPzDIyIpQGd1BtPsw+f8hYnf69Cmh6NeVZs+/UBS8g5v4qN6Sf0Cq+az7A4eJ50+7KMFAb8Z4uU2Su2cu4v+TBZZgDQy+SnlrI/Cl8lwqJFo9LR17fkHoDrAHWjdcVAYlO2EhGtFKzFcxUGNSLJ2NbvKjZGrg5+vOQEjigRduO12zpN/PSxuRawy+EKSrPSgGx3rjUC7ysnUzsvkGppI81LGOfJ15CExjN+8Gy3tK2AZMG+LRBYd/c8+mo6vJZ4KkbUIDndlymIVEuGFrthjyNSCN0s/b6DX1mV1B9Y5lxmzSA430ilM7+npYR9KZrMc8dK1ahHpA/aeTnuMlJ3lzqb3IsojinG0CaHXxW/3GC1XUpaFpoCABR85nwwix5KX/4LAjlB7UMVqbBAY1f+kdw5WbpFEdb1L2xf/3W2pHIkTMtSQGpKw41w4WFEHtoAVSEUkB";

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

std::string Sign(const std::string& payload)
{
    std::vector<unsigned char> blob;
    if (!Crypto::Base64Decode(kPrivateKeyBlobBase64, blob) || blob.empty())
        return std::string();

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    std::string signature;

    do
    {
        if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_RSA_ALGORITHM, nullptr, 0)))
            break;

        if (!NT_SUCCESS(BCryptImportKeyPair(hAlg, nullptr, BCRYPT_RSAPRIVATE_BLOB, &hKey,
                                           blob.data(), (ULONG)blob.size(), 0)))
            break;

        DWORD signatureLength = 0;
        ULONG written = 0;
        if (!NT_SUCCESS(BCryptGetProperty(hKey, BCRYPT_SIGNATURE_LENGTH,
                                         (PUCHAR)&signatureLength, sizeof(signatureLength),
                                         &written, 0)))
            break;

        std::vector<unsigned char> hash = Crypto::Sha256(payload);
        if (hash.empty())
            break;

        std::vector<unsigned char> raw(signatureLength);

        BCRYPT_PKCS1_PADDING_INFO padding;
        padding.pszAlgId = BCRYPT_SHA256_ALGORITHM;

        ULONG signedLength = 0;
        if (!NT_SUCCESS(BCryptSignHash(hKey, &padding, hash.data(), (ULONG)hash.size(),
                                       raw.data(), (ULONG)raw.size(), &signedLength,
                                       BCRYPT_PAD_PKCS1)))
        {
            break;
        }

        raw.resize(signedLength);
        signature = Crypto::Base64Encode(raw);
    } while (false);

    if (hKey != nullptr)
        BCryptDestroyKey(hKey);
    if (hAlg != nullptr)
        BCryptCloseAlgorithmProvider(hAlg, 0);

    return signature;
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
    if (!Crypto::Base64Decode(modulusB64, modulus) || modulus.empty())
        return false;
    if (!Crypto::Base64Decode(exponentB64, exponent) || exponent.empty())
        return false;
    if (!Crypto::Base64Decode(signatureBase64, signature) || signature.empty())
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

        std::vector<unsigned char> hash = Crypto::Sha256(payload);
        if (hash.empty())
            break;

        // 与签名侧对称：签名用 BCRYPT_PKCS1_PADDING_INFO(SHA256)，验签也必须带同一 OID
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

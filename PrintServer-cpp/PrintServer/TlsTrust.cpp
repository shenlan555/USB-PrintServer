#include "pch.h"
#include "TlsTrust.h"
#include "TlsTrustRoots.h"
#include "Crypto.h"

#include <map>
#include <mutex>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

namespace ps
{
namespace TlsTrust
{

namespace
{

std::once_flag g_storeOnce;
HCERTSTORE g_embeddedStore = nullptr;

std::mutex g_cacheMutex;
std::map<std::wstring, bool> g_relaxCache;

void CreateEmbeddedStore()
{
    HCERTSTORE store = ::CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0,
                                      CERT_STORE_CREATE_NEW_FLAG, nullptr);
    if (store == nullptr)
        return;

    for (size_t i = 0; i < kEmbeddedRootCount; ++i)
    {
        PCCERT_CONTEXT cert = ::CertCreateCertificateContext(
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            kEmbeddedRoots[i].der, (DWORD)kEmbeddedRoots[i].length);
        if (cert == nullptr)
            continue;

        ::CertAddCertificateContextToStore(store, cert, CERT_STORE_ADD_ALWAYS, nullptr);
        ::CertFreeCertificateContext(cert);
    }

    g_embeddedStore = store;
}

void EnsureEmbeddedStore()
{
    std::call_once(g_storeOnce, CreateEmbeddedStore);
}

// 建立链引擎并校验：chainTrustAnchors != nullptr 时只用该存储作为信任锚（系统 Root 存储被忽略）
bool BuildAndVerify(PCCERT_CONTEXT cert, const std::wstring& host, HCERTSTORE chainTrustAnchors)
{
    CERT_CHAIN_ENGINE_CONFIG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cbSize = sizeof(cfg);
    cfg.dwFlags = CERT_CHAIN_CACHE_END_CERT;
    cfg.dwUrlRetrievalTimeout = 5000;   // 限制 AIA/CRL 下载等待，避免校验拖慢授权
    cfg.hExclusiveRoot = chainTrustAnchors;

    HCERTCHAINENGINE engine = nullptr;
    if (!::CertCreateCertificateChainEngine(&cfg, &engine))
        return false;

    CERT_CHAIN_PARA para;
    memset(&para, 0, sizeof(para));
    para.cbSize = sizeof(para);

    PCCERT_CHAIN_CONTEXT chain = nullptr;
    BOOL built = ::CertGetCertificateChain(engine, cert, nullptr, nullptr,
                                           &para, 0, nullptr, &chain);

    bool valid = false;
    if (built && chain != nullptr)
    {
        HTTPSPolicyCallbackData sslPolicy;
        memset(&sslPolicy, 0, sizeof(sslPolicy));
        sslPolicy.cbStruct = sizeof(sslPolicy);
        sslPolicy.dwAuthType = AUTHTYPE_SERVER;
        sslPolicy.pwszServerName = const_cast<LPWSTR>(host.c_str());

        CERT_CHAIN_POLICY_PARA policy;
        memset(&policy, 0, sizeof(policy));
        policy.cbSize = sizeof(policy);
        policy.pvExtraPolicyPara = &sslPolicy;

        CERT_CHAIN_POLICY_STATUS status;
        memset(&status, 0, sizeof(status));
        status.cbSize = sizeof(status);

        if (::CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status))
            valid = (status.dwError == 0);

        ::CertFreeCertificateChain(chain);
    }

    ::CertFreeCertificateChainEngine(engine);
    return valid;
}

std::wstring CacheKey(const std::wstring& host, INTERNET_PORT port)
{
    wchar_t buf[16] = { 0 };
    swprintf_s(buf, L":%u", (unsigned)port);
    return host + buf;
}

}   // namespace

size_t EmbeddedRootCount()
{
    return kEmbeddedRootCount;
}

std::string EmbeddedRootSha256(size_t index)
{
    if (index >= kEmbeddedRootCount)
        return std::string();

    std::vector<unsigned char> der(kEmbeddedRoots[index].der,
                                   kEmbeddedRoots[index].der + kEmbeddedRoots[index].length);
    std::vector<unsigned char> hash = Crypto::Sha256(der);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(hash.size() * 2);
    for (unsigned char c : hash)
    {
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 0x0F]);
    }
    return out;
}

bool VerifyWithEmbeddedRootOnly(const CERT_CONTEXT* cert, const std::wstring& host)
{
    if (cert == nullptr)
        return false;

    EnsureEmbeddedStore();
    if (g_embeddedStore == nullptr)
        return false;

    return BuildAndVerify(const_cast<PCCERT_CONTEXT>(cert), host, g_embeddedStore);
}

bool VerifyWithSystemStoreOnly(const CERT_CONTEXT* cert, const std::wstring& host)
{
    if (cert == nullptr)
        return false;

    // hExclusiveRoot = nullptr 时使用系统默认 Root 存储（与 WinHTTP 的判定一致）
    return BuildAndVerify(const_cast<PCCERT_CONTEXT>(cert), host, nullptr);
}

int EvaluateCertificate(const CERT_CONTEXT* cert, const std::wstring& host)
{
    if (VerifyWithSystemStoreOnly(cert, host))
        return 0;
    if (VerifyWithEmbeddedRootOnly(cert, host))
        return 1;
    return -1;
}

PCCERT_CONTEXT ProbeServerCertificate(const std::wstring& host, INTERNET_PORT port)
{
    HINTERNET session = ::WinHttpOpen(L"PrintServer-TlsTrustProbe/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr)
        return nullptr;

    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 |
                      WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 |
                      WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    ::WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

    ::WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);

    PCCERT_CONTEXT result = nullptr;

    HINTERNET connect = ::WinHttpConnect(session, host.c_str(), port, 0);
    if (connect != nullptr)
    {
        HINTERNET request = ::WinHttpOpenRequest(connect, L"HEAD", L"/", nullptr,
                                                 WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 WINHTTP_FLAG_SECURE);
        if (request != nullptr)
        {
            // 探测连接只用于取回证书，可信与否由 VerifyWithEmbeddedRootOnly 判定
            DWORD ignoreFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                                SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                                SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                                SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            ::WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS,
                               &ignoreFlags, sizeof(ignoreFlags));

            if (::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     nullptr, 0, 0, 0))
            {
                ::WinHttpReceiveResponse(request, nullptr);

                PCCERT_CONTEXT cert = nullptr;
                DWORD len = sizeof(cert);
                if (::WinHttpQueryOption(request, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &len))
                    result = cert;
            }

            ::WinHttpCloseHandle(request);
        }
        ::WinHttpCloseHandle(connect);
    }

    ::WinHttpCloseHandle(session);
    return result;
}

bool HostNeedsEmbeddedRootRelax(const std::wstring& host, INTERNET_PORT port)
{
    std::wstring key = CacheKey(host, port);

    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        auto it = g_relaxCache.find(key);
        if (it != g_relaxCache.end())
            return it->second;
    }

    PCCERT_CONTEXT cert = ProbeServerCertificate(host, port);
    if (cert == nullptr)
        return false;   // 探测失败不缓存：下次请求再试

    bool need = VerifyWithEmbeddedRootOnly(cert, host);
    ::CertFreeCertificateContext(cert);

    if (need)
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_relaxCache[key] = true;
    }

    return need;
}

}
}

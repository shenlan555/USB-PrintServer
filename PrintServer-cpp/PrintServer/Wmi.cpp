#include "pch.h"
#include "Wmi.h"

#include <comdef.h>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace ps
{

// ===========================================================================
// 字符串工具
// ===========================================================================

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty())
        return std::string();

    int need = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                     nullptr, 0, nullptr, nullptr);
    if (need <= 0)
        return std::string();

    std::string out((size_t)need, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                          &out[0], need, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
        return std::wstring();

    int need = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    if (need <= 0)
        return std::wstring();

    std::wstring out((size_t)need, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], need);
    return out;
}

std::string ToLowerAscii(const std::string& text)
{
    std::string out = text;
    for (char& c : out)
    {
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }
    return out;
}

bool ContainsIgnoreCaseAscii(const std::string& haystack, const std::string& needleLower)
{
    if (needleLower.empty())
        return true;

    std::string lower = ToLowerAscii(haystack);
    return lower.find(needleLower) != std::string::npos;
}

// ===========================================================================
// COM 初始化
// ===========================================================================

void WmiEnsureComInitialized()
{
    // RPC_E_CHANGED_MODE 表示本线程已用其它模型初始化过，可继续使用
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    UNREFERENCED_PARAMETER(hr);
}

namespace
{
// 进程级只需成功设置一次；之后调用会返回 RPC_E_TOO_LATE，忽略即可
void EnsureSecurityOnce()
{
    static volatile LONG s_done = 0;
    if (::InterlockedCompareExchange(&s_done, 1, 0) != 0)
        return;

    ::CoInitializeSecurity(
        nullptr, -1, nullptr, nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE, nullptr);
}
}

// ===========================================================================
// WmiQuery
// ===========================================================================

WmiQuery::WmiQuery()
{
    WmiEnsureComInitialized();
    EnsureSecurityOnce();

    IWbemLocator* pLocator = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IWbemLocator, (LPVOID*)&pLocator);
    if (FAILED(hr) || pLocator == nullptr)
        return;

    BSTR ns = ::SysAllocString(L"ROOT\\CIMV2");
    hr = pLocator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &m_pSvc);
    ::SysFreeString(ns);

    pLocator->Release();

    if (FAILED(hr) || m_pSvc == nullptr)
    {
        m_pSvc = nullptr;
        return;
    }

    ::CoSetProxyBlanket(m_pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                        nullptr, EOAC_NONE);
}

WmiQuery::~WmiQuery()
{
    if (m_pSvc != nullptr)
    {
        m_pSvc->Release();
        m_pSvc = nullptr;
    }
}

bool WmiQuery::ForEach(const std::wstring& wql,
                       const std::function<bool(IWbemClassObject*)>& row)
{
    if (m_pSvc == nullptr)
        return false;

    BSTR lang = ::SysAllocString(L"WQL");
    BSTR query = ::SysAllocString(wql.c_str());

    IEnumWbemClassObject* pEnum = nullptr;
    HRESULT hr = m_pSvc->ExecQuery(lang, query,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum);

    ::SysFreeString(lang);
    ::SysFreeString(query);

    if (FAILED(hr) || pEnum == nullptr)
        return false;

    bool keepGoing = true;
    while (keepGoing)
    {
        IWbemClassObject* pObj = nullptr;
        ULONG returned = 0;

        hr = pEnum->Next(WBEM_INFINITE, 1, &pObj, &returned);
        if (FAILED(hr) || returned == 0 || pObj == nullptr)
            break;

        keepGoing = row(pObj);
        pObj->Release();
    }

    pEnum->Release();
    return true;
}

bool WmiQuery::GetString(IWbemClassObject* obj, const wchar_t* name, std::wstring& out)
{
    if (obj == nullptr)
        return false;

    VARIANT vt;
    ::VariantInit(&vt);

    HRESULT hr = obj->Get(name, 0, &vt, nullptr, nullptr);
    bool ok = false;

    if (SUCCEEDED(hr))
    {
        if (vt.vt == VT_BSTR && vt.bstrVal != nullptr)
        {
            out = vt.bstrVal;
            ok = true;
        }
        else if (vt.vt == VT_NULL || vt.vt == VT_EMPTY)
        {
            ok = false;
        }
        else
        {
            // 少数属性可能是数字等类型，做一次文本转换（对应 C# 的 .ToString()）
            VARIANT tmp;
            ::VariantInit(&tmp);
            if (SUCCEEDED(::VariantChangeType(&tmp, &vt, 0, VT_BSTR)) && tmp.vt == VT_BSTR)
            {
                out = tmp.bstrVal;
                ok = true;
            }
            ::VariantClear(&tmp);
        }
    }

    ::VariantClear(&vt);
    return ok;
}

bool WmiQuery::GetBool(IWbemClassObject* obj, const wchar_t* name, bool& out)
{
    if (obj == nullptr)
        return false;

    VARIANT vt;
    ::VariantInit(&vt);

    HRESULT hr = obj->Get(name, 0, &vt, nullptr, nullptr);
    bool ok = false;

    if (SUCCEEDED(hr))
    {
        if (vt.vt == VT_BOOL)
        {
            out = (vt.boolVal != VARIANT_FALSE);
            ok = true;
        }
        else if (vt.vt == VT_I4 || vt.vt == VT_UI4 || vt.vt == VT_I2)
        {
            out = (vt.lVal != 0);
            ok = true;
        }
    }

    ::VariantClear(&vt);
    return ok;
}

}

#pragma once

// WMI 查询封装
//
// 对应 C# 的 ManagementObjectSearcher / ManagementObject。
// 用原生 COM（IWbemLocator / IWbemServices）实现，避免引入 ATL 依赖。

#include <string>
#include <functional>
#include <windows.h>
#include <wbemidl.h>

namespace ps
{

// 确保当前线程的 COM 已初始化（可重复调用）
void WmiEnsureComInitialized();

class WmiQuery
{
public:
    WmiQuery();
    ~WmiQuery();

    WmiQuery(const WmiQuery&) = delete;
    WmiQuery& operator=(const WmiQuery&) = delete;

    bool IsValid() const { return m_pSvc != nullptr; }

    // 执行 WQL 查询；对每一行调用 row 回调，回调返回 false 可提前中止
    bool ForEach(const std::wstring& wql,
                 const std::function<bool(IWbemClassObject*)>& row);

    // 读取字符串属性。属性不存在或为 NULL 时返回 false（对应 C# 的 obj["X"] == null）
    static bool GetString(IWbemClassObject* obj, const wchar_t* name, std::wstring& out);

    // 读取布尔属性。属性不存在返回 false（对应 C# Convert.ToBoolean(null) 的语义：
    // C# 里用 obj["X"] != null 判断过，缺失时不会取值）
    static bool GetBool(IWbemClassObject* obj, const wchar_t* name, bool& out);

private:
    IWbemServices* m_pSvc = nullptr;
};

// UTF-16 → UTF-8
std::string WideToUtf8(const std::wstring& text);
// UTF-8 → UTF-16
std::wstring Utf8ToWide(const std::string& text);
// 仅对 ASCII A-Z 做小写（对应硬件名关键字的匹配需求）
std::string ToLowerAscii(const std::string& text);
// 大小写不敏感的 ASCII 子串查找
bool ContainsIgnoreCaseAscii(const std::string& haystack, const std::string& needleLower);

}

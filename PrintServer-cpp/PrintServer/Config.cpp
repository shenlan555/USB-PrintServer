#include "pch.h"
#include "Config.h"
#include "Wmi.h"

#include <vector>

namespace ps
{
namespace Config
{

std::wstring GetExeDirectory()
{
    wchar_t path[MAX_PATH] = { 0 };
    DWORD len = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return L".";

    std::wstring full(path, len);
    size_t pos = full.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
        return L".";
    return full.substr(0, pos);
}

std::wstring GetConfigPath()
{
    wchar_t path[MAX_PATH] = { 0 };
    DWORD len = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return L"PrintServer.exe.config";

    return std::wstring(path, len) + L".config";
}

namespace
{

std::string ReadAllTextUtf8(const std::wstring& path)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::string();

    LARGE_INTEGER size = { 0 };
    if (!::GetFileSizeEx(h, &size) || size.QuadPart <= 0)
    {
        ::CloseHandle(h);
        return std::string();
    }

    std::vector<char> buf((size_t)size.QuadPart);
    DWORD read = 0;
    ::ReadFile(h, buf.data(), (DWORD)buf.size(), &read, nullptr);
    ::CloseHandle(h);

    buf.resize(read);

    // 跳过 UTF-8 BOM
    size_t start = 0;
    if (buf.size() >= 3 &&
        (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
    {
        start = 3;
    }

    return std::string(buf.begin() + start, buf.end());
}

bool WriteAllTextUtf8(const std::wstring& path, const std::string& text, bool withBom)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    bool ok = true;

    if (withBom)
    {
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        ok = (::WriteFile(h, bom, 3, &written, nullptr) != 0);
    }

    if (ok && !text.empty())
        ok = (::WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr) != 0);

    ::CloseHandle(h);
    return ok;
}

// 查找 key="..." 对应的值；在同一 <add ...> 元素内查找，避免跨元素误匹配
bool FindSettingValue(const std::string& xml, const std::string& key, std::string& value)
{
    std::string needle = "key=\"" + key + "\"";
    size_t pos = 0;

    while (true)
    {
        size_t keyPos = xml.find(needle, pos);
        if (keyPos == std::string::npos)
            return false;

        // 该元素的范围：从 keyPos 前最近的 '<' 到其后最近的 '>'
        size_t elemStart = xml.rfind('<', keyPos);
        size_t elemEnd = xml.find('>', keyPos);
        if (elemStart == std::string::npos || elemEnd == std::string::npos)
            return false;

        std::string element = xml.substr(elemStart, elemEnd - elemStart + 1);

        size_t vPos = element.find("value=\"");
        if (vPos != std::string::npos)
        {
            vPos += 7; // strlen("value=\"")
            size_t vEnd = element.find('"', vPos);
            if (vEnd != std::string::npos)
            {
                value = element.substr(vPos, vEnd - vPos);
                return true;
            }
        }

        pos = elemEnd + 1; // 继续向后找同名 key
    }
}

// 打印机名/端口可能含 & < > " 等字符，写入前必须转义，否则配置文件会被写坏
std::string EscapeXml(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        switch (text[i])
        {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:   out.push_back(text[i]); break;
        }
    }
    return out;
}

// 与 EscapeXml 配对：读取时还原，保证旧配置（未转义）也能正常读取
std::string UnescapeXml(const std::string& text)
{
    if (text.find('&') == std::string::npos)
        return text;

    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '&')
        {
            out.push_back(text[i]);
            continue;
        }

        if (text.compare(i, 5, "&amp;") == 0)       { out.push_back('&');  i += 4; }
        else if (text.compare(i, 4, "&lt;") == 0)   { out.push_back('<');  i += 3; }
        else if (text.compare(i, 4, "&gt;") == 0)   { out.push_back('>');  i += 3; }
        else if (text.compare(i, 6, "&quot;") == 0) { out.push_back('"');  i += 5; }
        else if (text.compare(i, 6, "&apos;") == 0) { out.push_back('\''); i += 5; }
        else                                        { out.push_back('&'); }
    }
    return out;
}

// 首次运行生成的内置默认配置（含 IPP 全套开关，现场可直接编辑）
const char* kDefaultConfigTemplate =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
    "<configuration>\r\n"
    "  <appSettings>\r\n"
    "    <!-- 打印机数量：0 表示兼容旧单打印机模式（PrinterName/Port） -->\r\n"
    "    <add key=\"PrinterCount\" value=\"0\"/>\r\n"
    "    <add key=\"PrinterName\" value=\"\"/>\r\n"
    "    <add key=\"Port\" value=\"9100\"/>\r\n"
    "    <!-- 控制 API 端口（客户端发现与驱动下载） -->\r\n"
    "    <add key=\"ApiPort\" value=\"9180\"/>\r\n"
    "    <!-- IPP 免驱打印（RFC 8010/8011，标准端口 631） -->\r\n"
    "    <add key=\"IppEnabled\" value=\"true\"/>\r\n"
    "    <add key=\"IppPort\" value=\"631\"/>\r\n"
    "    <add key=\"IppMdnsEnabled\" value=\"true\"/>\r\n"
    "    <add key=\"IppPdfRendering\" value=\"true\"/>\r\n"
    "    <add key=\"IppRenderDpi\" value=\"300\"/>\r\n"
    "    <add key=\"IppMaxDocumentMB\" value=\"64\"/>\r\n"
    "    <add key=\"IppDuplex\" value=\"false\"/>\r\n"
    "    <add key=\"IppColor\" value=\"false\"/>\r\n"
    "    <!-- 公共配置 -->\r\n"
    "    <add key=\"AutoStart\" value=\"false\"/>\r\n"
    "    <add key=\"LogRetentionDays\" value=\"15\"/>\r\n"
    "    <add key=\"ReceiveTimeout\" value=\"30\"/>\r\n"
    "  </appSettings>\r\n"
    "</configuration>\r\n";

} // namespace

std::string StripXmlComments(const std::string& xml)
{
    std::string out;
    out.reserve(xml.size());

    size_t pos = 0;
    while (pos < xml.size())
    {
        size_t start = xml.find("<!--", pos);
        if (start == std::string::npos)
        {
            out.append(xml, pos, std::string::npos);
            break;
        }

        out.append(xml, pos, start - pos);

        size_t end = xml.find("-->", start + 4);
        if (end == std::string::npos)
            break; // 注释未闭合，丢弃剩余部分

        pos = end + 3;
    }

    return out;
}

bool EnsureConfigFile()
{
    const std::wstring path = GetConfigPath();
    if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return false;

    return WriteAllTextUtf8(path, kDefaultConfigTemplate, false);
}

std::string GetSetting(const std::string& key, const std::string& defaultValue)
{
    std::string xml = ReadAllTextUtf8(GetConfigPath());
    if (xml.empty())
        return defaultValue;

    // 关键：先剥离注释，否则会读到被注释掉的 PrinterN_* 示例值
    std::string stripped = StripXmlComments(xml);

    std::string value;
    if (!FindSettingValue(stripped, key, value))
        return defaultValue;

    // 对应 C# 的 string.IsNullOrEmpty(value) ? defaultValue : value
    if (value.empty())
        return defaultValue;

    return UnescapeXml(value);
}

bool HasSetting(const std::string& key)
{
    std::string xml = ReadAllTextUtf8(GetConfigPath());
    if (xml.empty())
        return false;

    std::string stripped = StripXmlComments(xml);
    std::string value;
    return FindSettingValue(stripped, key, value);
}

bool SetSetting(const std::string& key, const std::string& value)
{
    std::string xml = ReadAllTextUtf8(GetConfigPath());
    if (xml.empty())
    {
        // 首次运行或配置文件被误删：先按模板生成，避免所有设置"静默保存失败"
        EnsureConfigFile();
        xml = ReadAllTextUtf8(GetConfigPath());
        if (xml.empty())
            return false;
    }

    const std::string escapedValue = EscapeXml(value);

    std::string stripped = StripXmlComments(xml);
    std::string needle = "key=\"" + key + "\"";
    size_t pos = stripped.find(needle);

    if (pos != std::string::npos)
    {
        // 已存在：替换其 value
        size_t elemStart = stripped.rfind('<', pos);
        size_t elemEnd = stripped.find('>', pos);
        if (elemStart == std::string::npos || elemEnd == std::string::npos)
            return false;

        std::string element = stripped.substr(elemStart, elemEnd - elemStart + 1);
        size_t vPos = element.find("value=\"");
        if (vPos == std::string::npos)
            return false;

        size_t absValueStart = elemStart + vPos + 7;
        size_t absValueEnd = stripped.find('"', absValueStart);
        if (absValueEnd == std::string::npos)
            return false;

        // 在原始（含注释）xml 中定位并替换
        std::string oldFragment = stripped.substr(absValueStart, absValueEnd - absValueStart);
        size_t rawPos = xml.find(needle);
        while (rawPos != std::string::npos)
        {
            size_t rawEnd = xml.find('>', rawPos);
            if (rawEnd == std::string::npos)
                break;
            size_t rawV = xml.find("value=\"", rawPos);
            if (rawV != std::string::npos && rawV < rawEnd)
            {
                size_t rawVs = rawV + 7;
                size_t rawVe = xml.find('"', rawVs);
                if (rawVe != std::string::npos && xml.substr(rawVs, rawVe - rawVs) == oldFragment)
                {
                    xml = xml.substr(0, rawVs) + escapedValue + xml.substr(rawVe);
                    return WriteAllTextUtf8(GetConfigPath(), xml, false);
                }
            }
            rawPos = xml.find(needle, rawEnd);
        }
        return false;
    }

    // 不存在：在 </appSettings> 之前插入
    size_t closeTag = xml.find("</appSettings>");
    if (closeTag == std::string::npos)
        return false;

    std::string line = "    <add key=\"" + key + "\" value=\"" + escapedValue + "\"/>\r\n  ";
    xml.insert(closeTag, line);

    return WriteAllTextUtf8(GetConfigPath(), xml, false);
}

}
}

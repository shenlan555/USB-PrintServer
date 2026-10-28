#include "pch.h"
#include "IppProtocol.h"

#include <string.h>

namespace ps
{
namespace Ipp
{

namespace
{

// ---------------------------------------------------------------------------
// 防御性上限（网络未授权输入，必须有硬边界）
// ---------------------------------------------------------------------------
const size_t kMaxGroups            = 32;
const size_t kMaxAttributesPerGroup = 512;
const size_t kMaxValuesPerAttribute = 512;
const size_t kMaxNameLength        = 256;
const size_t kMaxResponseBytes     = 4 * 1024 * 1024;   // 响应体硬上限

// 大端读写
void PutU16(std::vector<unsigned char>& buffer, unsigned short value)
{
    buffer.push_back((unsigned char)((value >> 8) & 0xFF));
    buffer.push_back((unsigned char)(value & 0xFF));
}

void PutU32(std::vector<unsigned char>& buffer, unsigned long value)
{
    buffer.push_back((unsigned char)((value >> 24) & 0xFF));
    buffer.push_back((unsigned char)((value >> 16) & 0xFF));
    buffer.push_back((unsigned char)((value >> 8) & 0xFF));
    buffer.push_back((unsigned char)(value & 0xFF));
}

// 把值解析成结构化；类型/长度不匹配时"容忍"并标记 malformed
void DecodeValue(unsigned char tag, const unsigned char* data, size_t length, AttributeValue& out)
{
    out.tag = tag;
    out.intValue = 0;
    out.boolValue = false;
    out.text.clear();
    out.malformed = false;

    switch (tag)
    {
    case Tag::Integer:
    case Tag::Enum:
        if (length == 4)
        {
            out.intValue = ((long long)data[0] << 24) | ((long long)data[1] << 16) |
                           ((long long)data[2] << 8) | (long long)data[3];
        }
        else
        {
            out.malformed = true;   // 容忍：按 0 处理，不拒绝整个请求
        }
        break;

    case Tag::Boolean:
        if (length == 1)
            out.boolValue = (data[0] != 0);
        else
            out.malformed = true;
        break;

    default:
        if (length > 0)
            out.text.assign((const char*)data, length);
        break;
    }
}

} // namespace

// ===========================================================================
// 读取器
// ===========================================================================

namespace
{

class Reader
{
public:
    Reader(const unsigned char* data, size_t size) : m_data(data), m_size(size), m_pos(0) {}

    bool ReadU8(unsigned char& value)
    {
        if (m_pos + 1 > m_size)
            return false;
        value = m_data[m_pos++];
        return true;
    }

    bool ReadU16(unsigned short& value)
    {
        if (m_pos + 2 > m_size)
            return false;
        value = (unsigned short)(((unsigned short)m_data[m_pos] << 8) | m_data[m_pos + 1]);
        m_pos += 2;
        return true;
    }

    bool ReadU32(unsigned long& value)
    {
        if (m_pos + 4 > m_size)
            return false;
        value = ((unsigned long)m_data[m_pos] << 24) | ((unsigned long)m_data[m_pos + 1] << 16) |
                ((unsigned long)m_data[m_pos + 2] << 8) | (unsigned long)m_data[m_pos + 3];
        m_pos += 4;
        return true;
    }

    bool ReadBytes(size_t count, const unsigned char*& out)
    {
        if (count == 0)
        {
            out = nullptr;
            return true;
        }
        if (count > m_size - m_pos)
            return false;
        out = m_data + m_pos;
        m_pos += count;
        return true;
    }

    size_t Remaining() const { return m_size - m_pos; }
    size_t Offset() const { return m_pos; }

private:
    const unsigned char* m_data;
    size_t m_size;
    size_t m_pos;
};

} // namespace

// ===========================================================================
// Request 查询
// ===========================================================================

const Attribute* Request::Find(unsigned char /*groupTag*/, const char* name) const
{
    if (name == nullptr)
        return nullptr;

    // 注意：按 RFC 8011，作业模板属性既可出现在 operation-attributes 组，
    // 也可出现在 job-attributes 组；因此这里跨组顺序查找（最兼容）。
    for (const Group& group : groups)
    {
        for (const Attribute& attribute : group.attributes)
        {
            if (_stricmp(attribute.name.c_str(), name) == 0)
                return &attribute;
        }
    }

    return nullptr;
}

std::string Request::GetText(unsigned char groupTag, const char* name,
                             const std::string& defaultValue) const
{
    const Attribute* attribute = Find(groupTag, name);
    if (attribute == nullptr || attribute->values.empty())
        return defaultValue;

    return attribute->values[0].text;
}

std::vector<std::string> Request::GetTextList(unsigned char groupTag, const char* name) const
{
    std::vector<std::string> out;

    const Attribute* attribute = Find(groupTag, name);
    if (attribute == nullptr)
        return out;

    for (const AttributeValue& value : attribute->values)
        out.push_back(value.text);

    return out;
}

bool Request::GetInt(unsigned char groupTag, const char* name, long long& out) const
{
    const Attribute* attribute = Find(groupTag, name);
    if (attribute == nullptr || attribute->values.empty())
        return false;

    out = attribute->values[0].intValue;
    return true;
}

bool Request::GetBool(unsigned char groupTag, const char* name, bool& out) const
{
    const Attribute* attribute = Find(groupTag, name);
    if (attribute == nullptr || attribute->values.empty())
        return false;

    out = attribute->values[0].boolValue;
    return true;
}

bool Request::Requested(unsigned char groupTag, const char* name) const
{
    if (name == nullptr)
        return false;

    std::vector<std::string> requested = GetTextList(groupTag, "requested-attributes");
    for (const std::string& item : requested)
    {
        if (_stricmp(item.c_str(), name) == 0)
            return true;
    }

    return false;
}

bool Request::WantsAllShown() const
{
    std::vector<std::string> requested = GetTextList(Tag::OperationAttributes, "requested-attributes");
    if (requested.empty())
        return true;

    for (const std::string& item : requested)
    {
        if (_stricmp(item.c_str(), "all") == 0 ||
            _stricmp(item.c_str(), "printer-description") == 0 ||
            _stricmp(item.c_str(), "job-template") == 0 ||
            _stricmp(item.c_str(), "document-description") == 0)
        {
            return true;
        }
    }

    return false;
}

// ===========================================================================
// 解析
// ===========================================================================

bool ParseRequest(const unsigned char* data, size_t size, Request& request)
{
    request = Request();

    if (data == nullptr || size < 8)
    {
        request.parseError = "请求体过短";
        return false;
    }

    Reader reader(data, size);

    unsigned char major = 0;
    unsigned char minor = 0;
    unsigned short operationId = 0;
    unsigned long requestId = 0;

    if (!reader.ReadU8(major) || !reader.ReadU8(minor) ||
        !reader.ReadU16(operationId) || !reader.ReadU32(requestId))
    {
        request.parseError = "请求头不完整";
        return false;
    }

    request.majorVersion = major;
    request.minorVersion = minor;
    request.operationId = operationId;
    request.requestId = requestId;

    Group* current = nullptr;
    size_t currentAttributeIndex = (size_t)-1;

    for (;;)
    {
        unsigned char tag = 0;
        if (!reader.ReadU8(tag))
        {
            request.parseError = "属性区未以 end-of-attributes 结束";
            return false;
        }

        if (tag == Tag::EndOfAttributes)
        {
            request.hasDocument = (reader.Remaining() > 0);
            request.documentOffset = reader.Offset();
            return true;
        }

        if (tag <= Tag::UnsupportedAttributes)
        {
            if (request.groups.size() >= kMaxGroups)
            {
                request.parseError = "属性组过多";
                return false;
            }

            request.groups.push_back(Group());
            current = &request.groups.back();
            current->tag = tag;
            currentAttributeIndex = (size_t)-1;
            continue;
        }

        if (current == nullptr)
        {
            request.parseError = "属性出现在任何属性组之前";
            return false;
        }

        // ---- 集合属性（media-col / media-size 等）----
        // 集合内部是"成员属性名 + 值"的平铺序列，成员名字段长度为 0。
        // 这里整体跳过（保留属性名、值留空），避免集合成员被当成同级属性。
        if (tag == Tag::Collection)
        {
            unsigned short nameLength = 0;
            if (!reader.ReadU16(nameLength) || nameLength > kMaxNameLength)
            {
                request.parseError = "集合属性名长度非法";
                return false;
            }

            const unsigned char* nameData = nullptr;
            if (!reader.ReadBytes(nameLength, nameData))
            {
                request.parseError = "集合属性名越界";
                return false;
            }

            unsigned short valueLength = 0;
            if (!reader.ReadU16(valueLength))
            {
                request.parseError = "集合属性值长度缺失";
                return false;
            }

            const unsigned char* valueData = nullptr;
            if (!reader.ReadBytes(valueLength, valueData))
            {
                request.parseError = "集合属性值越界";
                return false;
            }

            int depth = 1;
            while (depth > 0)
            {
                unsigned char innerTag = 0;
                if (!reader.ReadU8(innerTag))
                {
                    request.parseError = "集合未以 endCollection 结束";
                    return false;
                }

                if (innerTag == Tag::EndOfAttributes)
                {
                    request.parseError = "集合未闭合";
                    return false;
                }

                unsigned short innerNameLength = 0;
                if (!reader.ReadU16(innerNameLength) || innerNameLength > kMaxNameLength)
                {
                    request.parseError = "集合成员名长度非法";
                    return false;
                }

                const unsigned char* innerName = nullptr;
                if (!reader.ReadBytes(innerNameLength, innerName))
                {
                    request.parseError = "集合成员名越界";
                    return false;
                }

                unsigned short innerValueLength = 0;
                if (!reader.ReadU16(innerValueLength))
                {
                    request.parseError = "集合成员值长度缺失";
                    return false;
                }

                const unsigned char* innerValue = nullptr;
                if (!reader.ReadBytes(innerValueLength, innerValue))
                {
                    request.parseError = "集合成员值越界";
                    return false;
                }

                if (innerTag == Tag::Collection)
                    ++depth;
                else if (innerTag == Tag::EndCollection)
                    --depth;
            }

            if (current->attributes.size() < kMaxAttributesPerGroup)
            {
                Attribute attribute;
                attribute.name.assign((const char*)nameData, nameLength);

                AttributeValue placeholder;
                placeholder.tag = Tag::Collection;
                attribute.values.push_back(placeholder);

                current->attributes.push_back(attribute);
                currentAttributeIndex = current->attributes.size() - 1;
            }

            continue;
        }

        unsigned short nameLength = 0;
        if (!reader.ReadU16(nameLength) || nameLength > kMaxNameLength)
        {
            request.parseError = "属性名长度非法";
            return false;
        }

        const unsigned char* nameData = nullptr;
        if (!reader.ReadBytes(nameLength, nameData))
        {
            request.parseError = "属性名越界";
            return false;
        }

        unsigned short valueLength = 0;
        if (!reader.ReadU16(valueLength))
        {
            request.parseError = "属性值长度缺失";
            return false;
        }

        const unsigned char* valueData = nullptr;
        if (!reader.ReadBytes(valueLength, valueData))
        {
            request.parseError = "属性值越界";
            return false;
        }

        AttributeValue value;
        DecodeValue(tag, valueData, valueLength, value);

        if (nameLength == 0)
        {
            // 同名前一个属性的追加值
            if (currentAttributeIndex >= current->attributes.size())
                continue;   // 无宿主属性，忽略（容忍）

            Attribute& host = current->attributes[currentAttributeIndex];
            if (host.values.size() >= kMaxValuesPerAttribute)
                continue;

            host.values.push_back(value);
            continue;
        }

        if (current->attributes.size() >= kMaxAttributesPerGroup)
            continue;       // 超出上限：丢弃多余属性，而不是拒绝整个请求

        Attribute attribute;
        attribute.name.assign((const char*)nameData, nameLength);
        attribute.values.push_back(value);
        current->attributes.push_back(attribute);
        currentAttributeIndex = current->attributes.size() - 1;
    }
}

// ===========================================================================
// 响应编码
// ===========================================================================

ResponseBuilder::ResponseBuilder()
{
    m_buffer.reserve(4096);
}

void ResponseBuilder::SetAttributeFilter(AttributeFilter filter)
{
    m_filter = std::move(filter);
}

void ResponseBuilder::WriteU8(unsigned char value)
{
    m_buffer.push_back(value);
}

void ResponseBuilder::WriteU16(unsigned short value)
{
    PutU16(m_buffer, value);
}

void ResponseBuilder::WriteU32(unsigned long value)
{
    PutU32(m_buffer, value);
}

void ResponseBuilder::Begin(unsigned char major, unsigned char minor, unsigned short statusCode,
                            unsigned long requestId)
{
    m_buffer.clear();
    m_currentGroupNames.clear();
    m_status = statusCode;

    WriteU8(major);
    WriteU8(minor);
    WriteU16(statusCode);
    WriteU32(requestId);
}

void ResponseBuilder::BeginGroup(unsigned char groupTag)
{
    WriteU8(groupTag);
    m_currentGroupNames.clear();
}

bool ResponseBuilder::NameUsedInCurrentGroup(const char* name)
{
    if (name == nullptr)
        return false;

    for (const std::string& item : m_currentGroupNames)
    {
        if (item == name)
            return true;
    }
    return false;
}

void ResponseBuilder::AddRawValue(unsigned char valueTag, const char* name,
                                  const unsigned char* data, size_t length)
{
    // requested-attributes 过滤：客户端没要的属性不写进响应
    if (m_filter && name != nullptr && !m_filter(name))
        return;

    // RFC 8010 里 text / name / keyword / uri / charset / naturalLanguage /
    // mimeMediaType 这些字符串属性的长度下限是 1 个字节，长度为 0 属于非法编码。
    // 宽容的解析器（包括我们自己写的、以及一些第三方库）会放过它，
    // 但 Windows 的 IPP 实现据此判定整个响应非法 —— 客户端表现是
    // "Windows 无法连接到打印机"，并且会反复重试多个候选 URL 后放弃。
    // 所以这里直接丢弃空字符串属性，绝不发出非法值。
    switch (valueTag)
    {
    case Tag::Text:
    case Tag::Name:
    case Tag::Keyword:
    case Tag::Uri:
    case Tag::UriScheme:
    case Tag::Charset:
    case Tag::NaturalLanguage:
    case Tag::MimeMediaType:
        if (length == 0)
            return;
        break;
    default:
        break;
    }

    // 响应体硬上限：宁可少发属性，也不能让响应无限膨胀
    if (m_buffer.size() + length + 64 > kMaxResponseBytes)
        return;

    const char* safeName = (name != nullptr) ? name : "";
    bool repeated = NameUsedInCurrentGroup(safeName);
    size_t nameLength = repeated ? 0 : strlen(safeName);

    WriteU8(valueTag);
    WriteU16((unsigned short)nameLength);
    if (nameLength > 0)
        m_buffer.insert(m_buffer.end(), safeName, safeName + nameLength);

    WriteU16((unsigned short)length);
    if (length > 0 && data != nullptr)
        m_buffer.insert(m_buffer.end(), data, data + length);

    if (nameLength > 0)
        m_currentGroupNames.push_back(safeName);
}

void ResponseBuilder::AddKeyword(const char* name, const std::string& value)
{
    AddRawValue(Tag::Keyword, name, (const unsigned char*)value.data(), value.size());
}

void ResponseBuilder::AddText(const char* name, const std::string& value)
{
    AddRawValue(Tag::Text, name, (const unsigned char*)value.data(), value.size());
}

void ResponseBuilder::AddName(const char* name, const std::string& value)
{
    AddRawValue(Tag::Name, name, (const unsigned char*)value.data(), value.size());
}

void ResponseBuilder::AddUri(const char* name, const std::string& value)
{
    AddRawValue(Tag::Uri, name, (const unsigned char*)value.data(), value.size());
}

void ResponseBuilder::AddUriScheme(const char* name, const std::string& value)
{
    AddRawValue(Tag::UriScheme, name, (const unsigned char*)value.data(), value.size());
}

void ResponseBuilder::AddCharset(const char* name, const std::string& value)
{
    AddRawValue(Tag::Charset, name, (const unsigned char*)value.data(), value.size());
}

void ResponseBuilder::AddNaturalLanguage(const char* name, const std::string& value)
{
    AddRawValue(Tag::NaturalLanguage, name, (const unsigned char*)value.data(), value.size());
}

void ResponseBuilder::AddMimeType(const char* name, const std::string& value)
{
    AddRawValue(Tag::MimeMediaType, name, (const unsigned char*)value.data(), value.size());
}

void ResponseBuilder::AddInt(const char* name, long long value)
{
    unsigned char raw[4];
    raw[0] = (unsigned char)((value >> 24) & 0xFF);
    raw[1] = (unsigned char)((value >> 16) & 0xFF);
    raw[2] = (unsigned char)((value >> 8) & 0xFF);
    raw[3] = (unsigned char)(value & 0xFF);
    AddRawValue(Tag::Integer, name, raw, 4);
}

void ResponseBuilder::AddEnum(const char* name, long long value)
{
    unsigned char raw[4];
    raw[0] = (unsigned char)((value >> 24) & 0xFF);
    raw[1] = (unsigned char)((value >> 16) & 0xFF);
    raw[2] = (unsigned char)((value >> 8) & 0xFF);
    raw[3] = (unsigned char)(value & 0xFF);
    AddRawValue(Tag::Enum, name, raw, 4);
}

void ResponseBuilder::AddBool(const char* name, bool value)
{
    unsigned char raw = value ? 1 : 0;
    AddRawValue(Tag::Boolean, name, &raw, 1);
}

void ResponseBuilder::AddRange(const char* name, long long lower, long long upper)
{
    unsigned char raw[8];
    raw[0] = (unsigned char)((lower >> 24) & 0xFF);
    raw[1] = (unsigned char)((lower >> 16) & 0xFF);
    raw[2] = (unsigned char)((lower >> 8) & 0xFF);
    raw[3] = (unsigned char)(lower & 0xFF);
    raw[4] = (unsigned char)((upper >> 24) & 0xFF);
    raw[5] = (unsigned char)((upper >> 16) & 0xFF);
    raw[6] = (unsigned char)((upper >> 8) & 0xFF);
    raw[7] = (unsigned char)(upper & 0xFF);
    AddRawValue(Tag::RangeOfInteger, name, raw, 8);
}

void ResponseBuilder::AddResolution(const char* name, long long x, long long y, unsigned char units)
{
    unsigned char raw[9];
    raw[0] = (unsigned char)((x >> 24) & 0xFF);
    raw[1] = (unsigned char)((x >> 16) & 0xFF);
    raw[2] = (unsigned char)((x >> 8) & 0xFF);
    raw[3] = (unsigned char)(x & 0xFF);
    raw[4] = (unsigned char)((y >> 24) & 0xFF);
    raw[5] = (unsigned char)((y >> 16) & 0xFF);
    raw[6] = (unsigned char)((y >> 8) & 0xFF);
    raw[7] = (unsigned char)(y & 0xFF);
    raw[8] = units;
    AddRawValue(Tag::Resolution, name, raw, 9);
}

void ResponseBuilder::AddDateTime(const char* name, time_t utcSeconds)
{
    struct tm utc;
    memset(&utc, 0, sizeof(utc));

#if defined(_MSC_VER)
    if (gmtime_s(&utc, &utcSeconds) != 0)
        return;
#else
    if (gmtime_r(&utcSeconds, &utc) == nullptr)
        return;
#endif

    unsigned char raw[11];
    unsigned short year = (unsigned short)(utc.tm_year + 1900);
    raw[0] = (unsigned char)((year >> 8) & 0xFF);
    raw[1] = (unsigned char)(year & 0xFF);
    raw[2] = (unsigned char)(utc.tm_mon + 1);
    raw[3] = (unsigned char)utc.tm_mday;
    raw[4] = (unsigned char)utc.tm_hour;
    raw[5] = (unsigned char)utc.tm_min;
    raw[6] = (unsigned char)utc.tm_sec;
    raw[7] = 0;                       // deci-seconds
    raw[8] = (unsigned char)'+';      // 方向：本实现统一按 UTC 输出
    raw[9] = 0;                       // 与 UTC 的小时差
    raw[10] = 0;                      // 与 UTC 的分钟差
    AddRawValue(Tag::DateTime, name, raw, 11);
}

void ResponseBuilder::AddNoValue(const char* name)
{
    AddRawValue(Tag::NoValue, name, nullptr, 0);
}

void ResponseBuilder::AddOctetString(const char* name, const std::string& value)
{
    AddRawValue(Tag::OctetString, name, (const unsigned char*)value.data(), value.size());
}

void ResponseBuilder::EndAttributes()
{
    WriteU8(Tag::EndOfAttributes);
}

// ===========================================================================
// 版本
// ===========================================================================

bool IsSupportedVersion(int major, int minor)
{
    if (major == 1 && (minor == 0 || minor == 1))
        return true;
    if (major == 2 && minor == 0)
        return true;
    return false;
}

void ClampVersion(int& major, int& minor)
{
    // 高于本实现支持的版本：回退到 1.1（并保持可继续处理，而不是报错），
    // 这样即便客户端使用 2.1/2.2，也不会被直接拒绝。
    if (!IsSupportedVersion(major, minor))
    {
        major = 1;
        minor = 1;
    }
}

} // namespace Ipp
} // namespace ps

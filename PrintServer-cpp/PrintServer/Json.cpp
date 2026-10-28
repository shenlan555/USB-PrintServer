#include "pch.h"
#include "Json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmath>

namespace ps
{

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------

JsonValue JsonValue::Bool(bool v)   { JsonValue j; j.type = BOOL; j.b = v; return j; }
JsonValue JsonValue::Int(long long v){ JsonValue j; j.type = INT;  j.i = v; return j; }
JsonValue JsonValue::Dbl(double v)  { JsonValue j; j.type = DBL;  j.d = v; return j; }
JsonValue JsonValue::Array()        { JsonValue j; j.type = ARR; return j; }
JsonValue JsonValue::Object()       { JsonValue j; j.type = OBJ; return j; }

JsonValue JsonValue::Str(const std::string& utf8)
{
    JsonValue j;
    j.type = STR;
    j.s = utf8;
    return j;
}

void JsonValue::Set(const std::string& key, const JsonValue& v)
{
    type = OBJ;
    for (auto& kv : obj)
    {
        if (kv.first == key)
        {
            kv.second = v;
            return;
        }
    }
    obj.emplace_back(key, v);
}

bool JsonValue::Has(const std::string& key) const
{
    return Find(key) != nullptr;
}

const JsonValue* JsonValue::Find(const std::string& key) const
{
    if (type != OBJ)
        return nullptr;
    for (const auto& kv : obj)
    {
        if (kv.first == key)
            return &kv.second;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 取值
// ---------------------------------------------------------------------------

std::string JsonValue::AsString(const std::string& def) const
{
    if (type == STR)
        return s;
    if (type == NUL)
        return def;
    return ToConvertString();
}

bool JsonValue::AsBool(bool def) const
{
    switch (type)
    {
    case BOOL: return b;
    case INT:  return i != 0;
    case STR:  return _stricmp(s.c_str(), "true") == 0 || s == "1";
    default:   return def;
    }
}

long long JsonValue::AsInt(long long def) const
{
    switch (type)
    {
    case INT: return i;
    case DBL: return (long long)d;
    case STR: return _strtoi64(s.c_str(), nullptr, 10);
    case BOOL: return b ? 1 : 0;
    default: return def;
    }
}

// 对应 C# 的 Convert.ToString(object)：
//   null → ""，bool → "True"/"False"，数字 → 不变文化十进制文本
std::string JsonValue::ToConvertString() const
{
    char buf[64];
    switch (type)
    {
    case STR:  return s;
    case BOOL: return b ? "True" : "False";
    case INT:  _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%lld", i); return buf;
    case DBL:  _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.17g", d); return buf;
    case NUL:  return "";
    default:   return "";
    }
}

// ---------------------------------------------------------------------------
// 序列化
// ---------------------------------------------------------------------------

static void AppendUtf8(std::string& out, unsigned int cp)
{
    if (cp <= 0x7F)
    {
        out.push_back((char)cp);
    }
    else if (cp <= 0x7FF)
    {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
    else if (cp <= 0xFFFF)
    {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

// 与 JavaScriptSerializer 一致的字符串转义
static void AppendEscapedString(std::string& out, const std::string& s)
{
    out.push_back('"');
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        // JavaScriptSerializer 为防 XSS 额外转义这几个字符
        case '<':  out += "\\u003c"; break;
        case '>':  out += "\\u003e"; break;
        case '&':  out += "\\u0026"; break;
        case '\'': out += "\\u0027"; break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\\u%04x", c);
                out += buf;
            }
            else
            {
                out.push_back((char)c);
            }
            break;
        }
    }
    out.push_back('"');
}

// 输出能往返的最短十进制表示（对应 .NET 的 "R" 格式）
static void AppendDouble(std::string& out, double v)
{
    if (v != v || v == HUGE_VAL || v == -HUGE_VAL)
    {
        out += "0"; // NaN/Infinity 不是合法 JSON，按 0 处理
        return;
    }

    char buf[64];
    for (int prec = 15; prec <= 17; ++prec)
    {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.*g", prec, v);
        double back = strtod(buf, nullptr);
        if (back == v)
            break;
    }
    out += buf;
}

void JsonValue::SerializeTo(std::string& out) const
{
    switch (type)
    {
    case NUL:
        out += "null";
        break;
    case BOOL:
        out += b ? "true" : "false";
        break;
    case INT:
    {
        char buf[32];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%lld", i);
        out += buf;
        break;
    }
    case DBL:
        AppendDouble(out, d);
        break;
    case STR:
        AppendEscapedString(out, s);
        break;
    case ARR:
    {
        out.push_back('[');
        for (size_t n = 0; n < arr.size(); ++n)
        {
            if (n) out.push_back(',');
            arr[n].SerializeTo(out);
        }
        out.push_back(']');
        break;
    }
    case OBJ:
    {
        out.push_back('{');
        for (size_t n = 0; n < obj.size(); ++n)
        {
            if (n) out.push_back(',');
            AppendEscapedString(out, obj[n].first);
            out.push_back(':');
            obj[n].second.SerializeTo(out);
        }
        out.push_back('}');
        break;
    }
    }
}

std::string JsonValue::Serialize() const
{
    std::string out;
    out.reserve(256);
    SerializeTo(out);
    return out;
}

// ---------------------------------------------------------------------------
// 反序列化
// ---------------------------------------------------------------------------

namespace
{

class Parser
{
public:
    Parser(const std::string& text, std::string* error)
        : m_text(text), m_pos(0), m_error(error)
    {
    }

    bool Parse(JsonValue& out)
    {
        SkipWs();
        if (!ParseValue(out))
            return false;
        SkipWs();
        if (m_pos != m_text.size())
        {
            Fail("JSON 末尾存在多余内容");
            return false;
        }
        return true;
    }

private:
    const std::string& m_text;
    size_t m_pos;
    std::string* m_error;

    void Fail(const char* msg)
    {
        if (m_error && m_error->empty())
            *m_error = msg;
    }

    bool Eof() const { return m_pos >= m_text.size(); }
    char Cur() const { return m_pos < m_text.size() ? m_text[m_pos] : '\0'; }

    void SkipWs()
    {
        // 跳过 JSON 空白字符，并顺带跳过文件开头的 UTF-8 BOM
        while (m_pos < m_text.size())
        {
            unsigned char c = (unsigned char)m_text[m_pos];

            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                ++m_pos;
                continue;
            }

            if (c == 0xEF && m_pos + 2 < m_text.size() &&
                (unsigned char)m_text[m_pos + 1] == 0xBB &&
                (unsigned char)m_text[m_pos + 2] == 0xBF)
            {
                m_pos += 3;
                continue;
            }

            break;
        }
    }

    bool ParseValue(JsonValue& out)
    {
        if (Eof())
        {
            Fail("JSON 内容为空");
            return false;
        }

        char c = Cur();
        switch (c)
        {
        case '{': return ParseObject(out);
        case '[': return ParseArray(out);
        case '"': return ParseStringValue(out);
        case 't': return ParseLiteral("true", JsonValue::Bool(true), out);
        case 'f': return ParseLiteral("false", JsonValue::Bool(false), out);
        case 'n': return ParseLiteral("null", JsonValue(), out);
        default:  return ParseNumber(out);
        }
    }

    bool ParseLiteral(const char* lit, const JsonValue& value, JsonValue& out)
    {
        size_t len = strlen(lit);
        if (m_text.compare(m_pos, len, lit) != 0)
        {
            Fail("JSON 字面量无效");
            return false;
        }
        m_pos += len;
        out = value;
        return true;
    }

    bool ParseObject(JsonValue& out)
    {
        out = JsonValue::Object();
        ++m_pos; // '{'

        SkipWs();
        if (Cur() == '}')
        {
            ++m_pos;
            return true;
        }

        for (;;)
        {
            SkipWs();
            if (Cur() != '"')
            {
                Fail("JSON 对象的键必须是字符串");
                return false;
            }

            std::string key;
            if (!ParseString(key))
                return false;

            SkipWs();
            if (Cur() != ':')
            {
                Fail("JSON 对象缺少冒号");
                return false;
            }
            ++m_pos;

            SkipWs();
            JsonValue v;
            if (!ParseValue(v))
                return false;
            out.obj.emplace_back(key, v);

            SkipWs();
            if (Cur() == ',')
            {
                ++m_pos;
                continue;
            }
            if (Cur() == '}')
            {
                ++m_pos;
                return true;
            }
            Fail("JSON 对象格式错误");
            return false;
        }
    }

    bool ParseArray(JsonValue& out)
    {
        out = JsonValue::Array();
        ++m_pos; // '['

        SkipWs();
        if (Cur() == ']')
        {
            ++m_pos;
            return true;
        }

        for (;;)
        {
            SkipWs();
            JsonValue v;
            if (!ParseValue(v))
                return false;
            out.arr.push_back(v);

            SkipWs();
            if (Cur() == ',')
            {
                ++m_pos;
                continue;
            }
            if (Cur() == ']')
            {
                ++m_pos;
                return true;
            }
            Fail("JSON 数组格式错误");
            return false;
        }
    }

    bool ParseStringValue(JsonValue& out)
    {
        std::string s;
        if (!ParseString(s))
            return false;
        out = JsonValue::Str(s);
        return true;
    }

    bool ParseString(std::string& out)
    {
        ++m_pos; // '"'
        out.clear();

        while (true)
        {
            if (Eof())
            {
                Fail("JSON 字符串未闭合");
                return false;
            }

            unsigned char c = (unsigned char)m_text[m_pos++];
            if (c == '"')
                return true;

            if (c != '\\')
            {
                out.push_back((char)c);
                continue;
            }

            if (Eof())
            {
                Fail("JSON 转义序列不完整");
                return false;
            }

            char e = m_text[m_pos++];
            switch (e)
            {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/');  break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u':
            {
                unsigned int cp = 0;
                if (!ParseHex4(cp))
                    return false;

                // 代理对
                if (cp >= 0xD800 && cp <= 0xDBFF && m_pos + 1 < m_text.size() &&
                    m_text[m_pos] == '\\' && m_text[m_pos + 1] == 'u')
                {
                    size_t save = m_pos;
                    m_pos += 2;
                    unsigned int low = 0;
                    if (ParseHex4(low) && low >= 0xDC00 && low <= 0xDFFF)
                    {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    else
                    {
                        m_pos = save;
                    }
                }

                AppendUtf8(out, cp);
                break;
            }
            default:
                Fail("JSON 未知转义字符");
                return false;
            }
        }
    }

    bool ParseHex4(unsigned int& cp)
    {
        if (m_pos + 4 > m_text.size())
        {
            Fail("JSON \\u 转义不完整");
            return false;
        }
        cp = 0;
        for (int n = 0; n < 4; ++n)
        {
            char h = m_text[m_pos + n];
            cp <<= 4;
            if (h >= '0' && h <= '9')      cp |= (unsigned int)(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= (unsigned int)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= (unsigned int)(h - 'A' + 10);
            else
            {
                Fail("JSON \\u 转义含非法十六进制字符");
                return false;
            }
        }
        m_pos += 4;
        return true;
    }

    bool ParseNumber(JsonValue& out)
    {
        size_t start = m_pos;
        bool isDouble = false;

        if (Cur() == '-' || Cur() == '+')
            ++m_pos;

        while (!Eof())
        {
            char c = Cur();
            if (c >= '0' && c <= '9')
            {
                ++m_pos;
            }
            else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')
            {
                isDouble = true;
                ++m_pos;
            }
            else
            {
                break;
            }
        }

        if (m_pos == start)
        {
            Fail("JSON 值无效");
            return false;
        }

        std::string num = m_text.substr(start, m_pos - start);
        if (isDouble)
        {
            out = JsonValue::Dbl(strtod(num.c_str(), nullptr));
        }
        else
        {
            out = JsonValue::Int(_strtoi64(num.c_str(), nullptr, 10));
        }
        return true;
    }
};

} // namespace

JsonValue JsonValue::Parse(const std::string& utf8, std::string* error)
{
    if (error)
        error->clear();

    JsonValue result;
    Parser parser(utf8, error);
    if (!parser.Parse(result))
        return JsonValue(); // 失败返回 null（对应 C# 反序列化异常 → 上层判空）
    return result;
}

} // namespace ps

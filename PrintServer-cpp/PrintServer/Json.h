#pragma once

// 轻量 JSON 实现
//
// 目的：替代 C# 的 System.Web.Script.Serialization.JavaScriptSerializer，
//       用于授权协议收发与本地缓存，序列化格式刻意与其保持一致：
//         - 不输出多余空白
//         - 字符串转义 "" \\ \b \f \n \r \t 及 0x00-0x1F 为 \u00XX
//         - 额外转义 < > & ' 为 \u003c \u003e \u0026 \u0027（JavaScriptSerializer 的行为）
//         - 非 ASCII 字符原样输出为 UTF-8（与 JavaScriptSerializer 一致）
//         - 数字：整数不带小数点，double 使用最短往返表示

#include <string>
#include <vector>
#include <utility>

namespace ps
{

class JsonValue
{
public:
    enum Type { NUL, BOOL, INT, DBL, STR, ARR, OBJ };

    Type type = NUL;
    bool b = false;
    long long i = 0;
    double d = 0.0;
    std::string s;                                              // STR
    std::vector<JsonValue> arr;                                 // ARR
    std::vector<std::pair<std::string, JsonValue>> obj;         // OBJ（保持插入顺序）

    JsonValue() = default;

    // ---- 构造快捷方法 ----
    static JsonValue Bool(bool v);
    static JsonValue Int(long long v);
    static JsonValue Dbl(double v);
    static JsonValue Str(const std::string& utf8);
    static JsonValue Array();
    static JsonValue Object();

    // ---- 对象操作 ----
    void Set(const std::string& key, const JsonValue& v);
    bool Has(const std::string& key) const;
    const JsonValue* Find(const std::string& key) const;

    // ---- 取值（带默认值，对应 C# 里 Convert.ToString / ToBoolean 的宽松风格）----
    std::string AsString(const std::string& def = "") const;
    bool AsBool(bool def = false) const;
    long long AsInt(long long def = 0) const;

    // 对应 Convert.ToString(object)：null → ""，数字转文本，bool → "True"/"False"
    std::string ToConvertString() const;

    // ---- 序列化 / 反序列化 ----
    std::string Serialize() const;
    static JsonValue Parse(const std::string& utf8, std::string* error = nullptr);

private:
    void SerializeTo(std::string& out) const;
};

} // namespace ps

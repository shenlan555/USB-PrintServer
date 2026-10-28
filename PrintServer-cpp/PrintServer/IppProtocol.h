#pragma once

// IPP 协议编解码（RFC 8010：Encoding and Transport）
//
// 设计原则（"稳妥/兼容"优先）：
//   1. 解码器对输入完全不信任：每个长度字段都做边界检查，任何畸形输入都
//      通过 parseError 返回，绝不允许越界读、死循环或异常外泄。
//   2. 对"能容忍的不合规"一律容忍（例如 integer 长度不是 4 字节时给默认值），
//      只对"结构性错误"（长度越界、属性出现在组之外）报 bad-request。
//   3. 属性/值/组都有数量上限，防止恶意请求造成内存膨胀。
//
// 注意：本文件只负责 IPP 体（version + operation + request-id + 属性组 + 文档），
//       HTTP 承载在 IppService 中处理。

#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace ps
{
namespace Ipp
{

// ===========================================================================
// 常量（RFC 8010 §3.5 标签；RFC 8011 附录 A 操作；RFC 8011 §13 状态码）
// ===========================================================================

namespace Tag
{
// 分隔符
constexpr unsigned char OperationAttributes   = 0x01;
constexpr unsigned char JobAttributes         = 0x02;
constexpr unsigned char EndOfAttributes       = 0x03;
constexpr unsigned char PrinterAttributes     = 0x04;
constexpr unsigned char UnsupportedAttributes = 0x05;

// 特殊值
constexpr unsigned char Unsupported    = 0x10;   // unsupported（本对象不支持）
constexpr unsigned char Unknown        = 0x12;   // unknown（非本对象属性）
constexpr unsigned char NoValue        = 0x13;

// 通用值
constexpr unsigned char Integer        = 0x21;
constexpr unsigned char Boolean        = 0x22;
constexpr unsigned char Enum           = 0x23;
constexpr unsigned char OctetString    = 0x30;
constexpr unsigned char DateTime       = 0x31;
constexpr unsigned char Resolution     = 0x32;
constexpr unsigned char RangeOfInteger = 0x33;
constexpr unsigned char Text           = 0x41;   // textWithoutLanguage
constexpr unsigned char Name           = 0x42;   // nameWithoutLanguage
constexpr unsigned char Keyword        = 0x44;
constexpr unsigned char Uri            = 0x45;
constexpr unsigned char UriScheme      = 0x46;
constexpr unsigned char Charset        = 0x47;
constexpr unsigned char NaturalLanguage= 0x48;
constexpr unsigned char MimeMediaType  = 0x49;

// 集合（RFC 8010 §3.1.6）：media-col / media-size 等复杂属性使用
constexpr unsigned char Collection     = 0x34;   // begCollection
constexpr unsigned char EndCollection  = 0x37;   // endCollection
constexpr unsigned char MemberAttrName = 0x4A;   // memberAttrName（名字长度为 0）
}

namespace Op
{
constexpr unsigned short PrintJob             = 0x0002;
constexpr unsigned short ValidateJob          = 0x0004;
constexpr unsigned short CreateJob            = 0x0005;
constexpr unsigned short SendDocument         = 0x0006;
constexpr unsigned short CancelJob            = 0x0008;
constexpr unsigned short GetJobAttributes     = 0x0009;
constexpr unsigned short GetJobs              = 0x000A;
constexpr unsigned short GetPrinterAttributes = 0x000B;
constexpr unsigned short HoldJob              = 0x000C;
constexpr unsigned short ReleaseJob           = 0x000D;
constexpr unsigned short RestartJob           = 0x000E;
constexpr unsigned short PausePrinter         = 0x0010;
constexpr unsigned short ResumePrinter        = 0x0011;
constexpr unsigned short PurgeJobs            = 0x0012;
constexpr unsigned short SetPrinterAttributes = 0x0013;
constexpr unsigned short GetPrinterSupportedValues = 0x0014;
constexpr unsigned short CloseJob             = 0x003B;   // Create-Job + Send-Document 的收尾
constexpr unsigned short IdentifyPrinter      = 0x003C;
}

namespace Status
{
constexpr unsigned short Ok                                    = 0x0000;
constexpr unsigned short OkIgnoredOrSubstituted                = 0x0001;
constexpr unsigned short ClientErrorBadRequest                 = 0x0400;
constexpr unsigned short ClientErrorForbidden                  = 0x0401;
constexpr unsigned short ClientErrorNotAuthenticated           = 0x0402;
constexpr unsigned short ClientErrorNotPossible                = 0x0404;
constexpr unsigned short ClientErrorTimeout                    = 0x0405;
constexpr unsigned short ClientErrorNotFound                   = 0x0406;
constexpr unsigned short ClientErrorGone                       = 0x0407;
constexpr unsigned short ClientErrorRequestEntityTooLarge      = 0x0408;
constexpr unsigned short ClientErrorRequestValueTooLong        = 0x0409;
constexpr unsigned short ClientErrorDocumentFormatNotSupported = 0x040A;
constexpr unsigned short ClientErrorAttributesOrValuesNotSupported = 0x040B;
constexpr unsigned short ClientErrorUriSchemeNotSupported      = 0x040C;
constexpr unsigned short ClientErrorCharsetNotSupported        = 0x040D;
constexpr unsigned short ClientErrorConflict                   = 0x040E;
constexpr unsigned short ClientErrorCompressionNotSupported    = 0x040F;
constexpr unsigned short ClientErrorDocumentFormatError        = 0x0410;
constexpr unsigned short ServerErrorInternalError              = 0x0500;
constexpr unsigned short ServerErrorOperationNotSupported      = 0x0501;
constexpr unsigned short ServerErrorServiceUnavailable         = 0x0502;
constexpr unsigned short ServerErrorVersionNotSupported        = 0x0503;
constexpr unsigned short ServerErrorDeviceError                = 0x0504;
constexpr unsigned short ServerErrorTemporaryError             = 0x0505;
constexpr unsigned short ServerErrorNotAcceptingJobs           = 0x0506;
constexpr unsigned short ServerErrorBusy                       = 0x0507;
constexpr unsigned short ServerErrorJobCanceled                = 0x0508;
constexpr unsigned short ServerErrorDocumentFormatError        = 0x050A;
}

// ===========================================================================
// 解析结果模型
// ===========================================================================

struct AttributeValue
{
    unsigned char tag = Tag::NoValue;
    long long intValue = 0;        // Integer / Enum
    bool boolValue = false;        // Boolean
    std::string text;              // 其余按原始字节保存
    bool malformed = false;        // 类型与长度不匹配（已容忍）
};

struct Attribute
{
    std::string name;
    std::vector<AttributeValue> values;
};

struct Group
{
    unsigned char tag = 0;
    std::vector<Attribute> attributes;
};

struct Request
{
    int majorVersion = 1;
    int minorVersion = 1;
    unsigned short operationId = 0;
    unsigned long requestId = 0;
    std::vector<Group> groups;

    // 文档数据在原始请求体中的偏移（= end-of-attributes 之后）
    size_t documentOffset = 0;
    bool hasDocument = false;

    // 非空表示解析失败（调用方据此返回 client-error-bad-request）
    std::string parseError;

    const Attribute* Find(unsigned char groupTag, const char* name) const;
    std::string GetText(unsigned char groupTag, const char* name,
                        const std::string& defaultValue = std::string()) const;
    std::vector<std::string> GetTextList(unsigned char groupTag, const char* name) const;
    bool GetInt(unsigned char groupTag, const char* name, long long& out) const;
    bool GetBool(unsigned char groupTag, const char* name, bool& out) const;

    // 是否显式要求了某属性（用于 requested-attributes 处理）
    bool Requested(unsigned char groupTag, const char* name) const;
    // 客户端是否要求"全部属性"（requested-attributes 含 all / printer-description 等组名）
    bool WantsAllShown() const;
};

// 解析 IPP 请求；成功返回 true。失败时 request.parseError 给出原因。
bool ParseRequest(const unsigned char* data, size_t size, Request& request);

// ===========================================================================
// 响应编码
// ===========================================================================

class ResponseBuilder
{
public:
    ResponseBuilder();

    void Begin(unsigned char major, unsigned char minor, unsigned short statusCode,
               unsigned long requestId);

    void BeginGroup(unsigned char groupTag);

    // 基础类型
    void AddKeyword(const char* name, const std::string& value);
    void AddText(const char* name, const std::string& value);
    void AddName(const char* name, const std::string& value);
    void AddUri(const char* name, const std::string& value);
    void AddUriScheme(const char* name, const std::string& value);
    void AddCharset(const char* name, const std::string& value);
    void AddNaturalLanguage(const char* name, const std::string& value);
    void AddMimeType(const char* name, const std::string& value);
    void AddInt(const char* name, long long value);
    void AddEnum(const char* name, long long value);
    void AddBool(const char* name, bool value);
    void AddRange(const char* name, long long lower, long long upper);
    void AddResolution(const char* name, long long x, long long y, unsigned char units);
    void AddDateTime(const char* name, time_t utcSeconds);
    void AddNoValue(const char* name);
    void AddOctetString(const char* name, const std::string& value);

    void EndAttributes();

    // 属性过滤（用于 requested-attributes）：
    // 只对 BeginGroup 之后写入的属性生效，传空 function 表示不过滤。
    // 返回 false 的属性会被整体丢弃（不会写入响应体）。
    using AttributeFilter = std::function<bool(const char* name)>;
    void SetAttributeFilter(AttributeFilter filter);

    const std::vector<unsigned char>& Bytes() const { return m_buffer; }
    size_t Size() const { return m_buffer.size(); }
    // 当前响应的状态码（handler 内部替换 builder 后仍能取到真实状态）
    unsigned short Status() const { return m_status; }

private:
    void WriteU8(unsigned char value);
    void WriteU16(unsigned short value);
    void WriteU32(unsigned long value);
    void AddRawValue(unsigned char valueTag, const char* name,
                     const unsigned char* data, size_t length);
    bool NameUsedInCurrentGroup(const char* name);

    std::vector<unsigned char> m_buffer;
    std::vector<std::string> m_currentGroupNames;
    AttributeFilter m_filter;
    unsigned short m_status = 0;
};

// 版本号是否在本实现支持的范围内（1.0 / 1.1 / 2.0）
bool IsSupportedVersion(int major, int minor);
// 把请求版本回退到本实现支持的版本（返回是否发生了回退）
void ClampVersion(int& major, int& minor);

} // namespace Ipp
} // namespace ps

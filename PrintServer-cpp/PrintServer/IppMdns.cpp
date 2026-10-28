#include "pch.h"
#include "IppMdns.h"

#include "LogHelper.h"
#include "PrintService.h"   // NetworkHelper::EnsureWinsock

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <map>
#include <mutex>
#include <string.h>
#include <thread>
#include <vector>

namespace ps
{
namespace IppMdns
{

namespace
{

const char* const kMulticastGroup = "224.0.0.251";
const unsigned short kMdnsPort = 5353;

const unsigned short kTypeA = 1;
const unsigned short kTypePtr = 12;
const unsigned short kTypeTxt = 16;
const unsigned short kTypeSrv = 33;
const unsigned short kClassIn = 1;

const unsigned long kTtlHost = 120;        // A / SRV
const unsigned long kTtlService = 4500;    // PTR / TXT

enum RecordKind
{
    kRecordA = 0,
    kRecordPtr,
    kRecordSrv,
    kRecordTxt
};

struct Record
{
    RecordKind kind = kRecordA;
    std::string owner;             // 便于查找（小写）
    std::string ownerDisplay;      // 原样大小写
    unsigned long ttl = kTtlService;

    unsigned long ipv4 = 0;        // A
    std::string targetName;        // PTR 目标（原样）
    unsigned short priority = 0;   // SRV
    unsigned short weight = 0;
    unsigned short port = 0;
    std::string targetHost;        // SRV 目标（原样）
    std::vector<std::string> txt;  // TXT 内容
};

std::string ToLowerAscii(const std::string& text)
{
    std::string out = text;
    for (char& ch : out)
    {
        if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch - 'A' + 'a');
    }
    return out;
}

std::vector<std::string> SplitLabels(const std::string& name)
{
    std::vector<std::string> labels;
    std::string current;

    for (char ch : name)
    {
        if (ch == '.')
        {
            if (!current.empty())
                labels.push_back(current);
            current.clear();
        }
        else
        {
            current.push_back(ch);
        }
    }
    if (!current.empty())
        labels.push_back(current);

    return labels;
}

// DNS-SD 实例名里不能出现未转义的 '.'，统一替换掉
// 解析 IPv4 文本（避免使用已废弃的 inet_addr）
bool ParseIpv4(const std::string& text, in_addr& out)
{
    out.s_addr = htonl(INADDR_ANY);
    return ::inet_pton(AF_INET, text.c_str(), &out) == 1;
}

std::string SanitizeInstanceName(const std::string& name)
{
    std::string out;
    for (char ch : name)
    {
        if (ch == '.')
            out.push_back('-');
        else if (ch == ' ' || ch == '/' || ch == '\\' || ch == ':' || ch == ',')
            out.push_back('-');
        else
            out.push_back(ch);
    }

    while (!out.empty() && out.back() == '-')
        out.pop_back();

    if (out.empty())
        out = "printer";

    return out;
}

// ------------------------------------------------------------------ 报文写入
class DnsWriter
{
public:
    void Reset()
    {
        m_data.clear();
        m_offsets.clear();
    }

    const std::vector<unsigned char>& Data() const { return m_data; }
    size_t Size() const { return m_data.size(); }

    void WriteU8(unsigned char value) { m_data.push_back(value); }

    void WriteU16(unsigned short value)
    {
        m_data.push_back((unsigned char)((value >> 8) & 0xFF));
        m_data.push_back((unsigned char)(value & 0xFF));
    }

    void WriteU32(unsigned long value)
    {
        m_data.push_back((unsigned char)((value >> 24) & 0xFF));
        m_data.push_back((unsigned char)((value >> 16) & 0xFF));
        m_data.push_back((unsigned char)((value >> 8) & 0xFF));
        m_data.push_back((unsigned char)(value & 0xFF));
    }

    // 写域名；使用后缀压缩（不压缩的话两条打印机的应答会超过 UDP 512 字节）
    void WriteName(const std::string& name)
    {
        std::vector<std::string> labels = SplitLabels(name);

        for (size_t i = 0; i < labels.size(); ++i)
        {
            std::string suffix;
            for (size_t j = i; j < labels.size(); ++j)
            {
                if (j > i)
                    suffix.push_back('.');
                suffix += ToLowerAscii(labels[j]);
            }

            std::map<std::string, unsigned short>::const_iterator it = m_offsets.find(suffix);
            if (it != m_offsets.end())
            {
                WriteU16((unsigned short)(0xC000 | it->second));
                return;
            }

            // 记录偏移（只在 14 位可寻址范围内才可被引用）
            if (m_data.size() < 0x3FFF)
                m_offsets[suffix] = (unsigned short)m_data.size();

            const std::string& label = labels[i];
            if (label.empty() || label.size() > 63)
                break;

            m_data.push_back((unsigned char)label.size());
            m_data.insert(m_data.end(), label.begin(), label.end());
        }

        m_data.push_back(0);   // 根
    }

    void PatchU16(size_t offset, unsigned short value)
    {
        if (offset + 1 >= m_data.size())
            return;
        m_data[offset] = (unsigned char)((value >> 8) & 0xFF);
        m_data[offset + 1] = (unsigned char)(value & 0xFF);
    }

private:
    std::vector<unsigned char> m_data;
    std::map<std::string, unsigned short> m_offsets;
};

// ------------------------------------------------------------------ 报文解析
class DnsReader
{
public:
    DnsReader(const unsigned char* data, size_t size) : m_data(data), m_size(size) {}

    bool ReadU16(size_t offset, unsigned short& value) const
    {
        if (offset + 1 >= m_size)
            return false;
        value = (unsigned short)(((unsigned short)m_data[offset] << 8) | m_data[offset + 1]);
        return true;
    }

    // 解析域名（含压缩指针），返回消耗的字节数
    size_t ReadName(size_t offset, std::string& out) const
    {
        out.clear();
        size_t position = offset;
        size_t consumed = 0;
        bool jumped = false;
        int guard = 0;

        for (;;)
        {
            if (++guard > 64 || position >= m_size)
                break;

            unsigned char length = m_data[position];

            if (length == 0)
            {
                if (!jumped)
                    consumed += 1;
                break;
            }

            if ((length & 0xC0) == 0xC0)
            {
                unsigned short pointer = 0;
                if (!ReadU16(position, pointer))
                    break;
                pointer &= 0x3FFF;
                if (!jumped)
                {
                    consumed += 2;
                    jumped = true;
                }
                if (pointer >= m_size)
                    break;
                position = pointer;
                continue;
            }

            if (position + 1 + length > m_size)
                break;

            if (!out.empty())
                out.push_back('.');
            out.append((const char*)m_data + position + 1, length);

            position += 1 + length;
            if (!jumped)
                consumed += 1 + length;
        }

        return consumed;
    }

private:
    const unsigned char* m_data;
    size_t m_size;
};

// ------------------------------------------------------------------ 广播实例
class Responder
{
public:
    bool Start(const std::string& hostName, const std::string& address,
               const std::vector<PrinterService>& services, std::string& error);
    void Stop();
    bool IsRunning() const { return m_running.load(); }

private:
    void BuildRecords(const std::string& hostName, const std::string& address,
                      const std::vector<PrinterService>& services);
    void Run();
    void SendAll(bool isAnnouncement, unsigned short queryId,
                 const std::vector<std::pair<std::string, unsigned short> >& questions);
    void SendUnsolicited();

    SOCKET m_socket = INVALID_SOCKET;
    std::thread m_thread;
    std::atomic<bool> m_running;
    std::string m_hostFqdn;                 // <host>.local
    std::vector<Record> m_records;
};

void Responder::BuildRecords(const std::string& hostName, const std::string& address,
                             const std::vector<PrinterService>& services)
{
    m_records.clear();
    m_hostFqdn = hostName + ".local";

    // A 记录
    {
        Record record;
        record.kind = kRecordA;
        record.owner = ToLowerAscii(m_hostFqdn);
        record.ownerDisplay = m_hostFqdn;
        record.ttl = kTtlHost;

        unsigned long a = 0;
        if (::inet_pton(AF_INET, address.c_str(), &a) == 1)
            record.ipv4 = a;
        m_records.push_back(record);
    }

    for (size_t i = 0; i < services.size(); ++i)
    {
        const PrinterService& service = services[i];
        std::string instance = SanitizeInstanceName(service.name);

        // ---------- IPP ----------
        std::string ippService = "_ipp._tcp.local";
        std::string ippFull = instance + "." + ippService;

        {
            Record record;
            record.kind = kRecordPtr;
            record.owner = ToLowerAscii(ippService);
            record.ownerDisplay = ippService;
            record.targetName = ippFull;
            m_records.push_back(record);
        }

        // IPP Everywhere / AirPrint 靠这个子类型订阅
        {
            Record record;
            record.kind = kRecordPtr;
            record.owner = "_universal._sub._ipp._tcp.local";
            record.ownerDisplay = "_universal._sub._ipp._tcp.local";
            record.targetName = ippFull;
            m_records.push_back(record);
        }

        {
            Record record;
            record.kind = kRecordSrv;
            record.owner = ToLowerAscii(ippFull);
            record.ownerDisplay = ippFull;
            record.ttl = kTtlHost;
            record.port = (unsigned short)service.ippPort;
            record.targetHost = m_hostFqdn;
            m_records.push_back(record);
        }

        {
            Record record;
            record.kind = kRecordTxt;
            record.owner = ToLowerAscii(ippFull);
            record.ownerDisplay = ippFull;

            record.txt.push_back("txtvers=1");
            record.txt.push_back("qtotal=1");
            record.txt.push_back("rp=" + service.ippPath);
            record.txt.push_back("ty=" + service.makeAndModel);
            record.txt.push_back("product=(" + service.makeAndModel + ")");
            record.txt.push_back("note=" + service.name);
            record.txt.push_back("pdl=" + service.pdl);
            record.txt.push_back(std::string("Color=") + (service.color ? "T" : "F"));
            record.txt.push_back(std::string("Duplex=") + (service.duplex ? "T" : "F"));
            record.txt.push_back("PaperMax=legal-A4");
            record.txt.push_back("kind=document");
            record.txt.push_back("TLS=no");
            record.txt.push_back("air=none");
            record.txt.push_back("adminurl=http://" + address + ":" +
                                 std::to_string(service.ippPort) + "/" + service.ippPath);
            if (!service.uuid.empty())
                record.txt.push_back("UUID=" + service.uuid);
            m_records.push_back(record);
        }

        // ---------- RAW（可选）----------
        if (service.rawPort > 0)
        {
            std::string rawService = "_pdl-datastream._tcp.local";
            std::string rawFull = instance + "." + rawService;

            {
                Record record;
                record.kind = kRecordPtr;
                record.owner = ToLowerAscii(rawService);
                record.ownerDisplay = rawService;
                record.targetName = rawFull;
                m_records.push_back(record);
            }

            {
                Record record;
                record.kind = kRecordSrv;
                record.owner = ToLowerAscii(rawFull);
                record.ownerDisplay = rawFull;
                record.ttl = kTtlHost;
                record.port = (unsigned short)service.rawPort;
                record.targetHost = m_hostFqdn;
                m_records.push_back(record);
            }

            {
                Record record;
                record.kind = kRecordTxt;
                record.owner = ToLowerAscii(rawFull);
                record.ownerDisplay = rawFull;
                record.txt.push_back("txtvers=1");
                record.txt.push_back("qtotal=1");
                record.txt.push_back("ty=" + service.makeAndModel);
                record.txt.push_back("pdl=" + service.pdl);
                record.txt.push_back(std::string("Color=") + (service.color ? "T" : "F"));
                record.txt.push_back(std::string("Duplex=") + (service.duplex ? "T" : "F"));
                record.txt.push_back("note=" + service.name);
                m_records.push_back(record);
            }
        }
    }
}

bool Responder::Start(const std::string& hostName, const std::string& address,
                      const std::vector<PrinterService>& services, std::string& error)
{
    error.clear();

    if (IsRunning())
        return true;

    if (services.empty())
    {
        error = "没有可广播的打印机";
        return false;
    }

    if (hostName.empty() || address.empty())
    {
        error = "缺少主机名或本机地址";
        return false;
    }

    NetworkHelper::EnsureWinsock();

    BuildRecords(hostName, address, services);
    m_running.store(true);

    m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET)
    {
        m_running.store(false);
        error = "创建 UDP 套接字失败";
        return false;
    }

    BOOL reuse = TRUE;
    ::setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(kMdnsPort);

    if (::bind(m_socket, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR)
    {
        int code = ::WSAGetLastError();
        ::closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        m_running.store(false);
        error = "绑定 UDP 5353 失败（错误 " + std::to_string(code) +
                "，可能有其它 mDNS 程序独占）";
        return false;
    }

    // 加入组播组（指定本机对外地址所在接口）
    ip_mreq membership;
    memset(&membership, 0, sizeof(membership));

    in_addr groupAddress;
    if (!ParseIpv4(kMulticastGroup, groupAddress))
    {
        ::closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        m_running.store(false);
        error = "组播地址解析失败";
        return false;
    }
    membership.imr_multiaddr = groupAddress;

    in_addr interfaceAddress;
    if (!ParseIpv4(address, interfaceAddress))
        interfaceAddress.s_addr = htonl(INADDR_ANY);
    membership.imr_interface = interfaceAddress;

    if (::setsockopt(m_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&membership,
                     sizeof(membership)) == SOCKET_ERROR)
    {
        // 加入组播失败不是致命错误：仍然可以主动广播（很多客户端靠主动公告发现）
        Log::WriteLog("[mDNS] 加入组播组失败（错误 " + std::to_string(::WSAGetLastError()) +
                      "），仅做主动公告");
    }

    DWORD ttl = 255;
    ::setsockopt(m_socket, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));

    BOOL loop = TRUE;
    ::setsockopt(m_socket, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));

    // 指定组播出口接口，避免多网卡时从错误网卡发出
    in_addr outInterface;
    outInterface.s_addr = membership.imr_interface.s_addr;
    ::setsockopt(m_socket, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&outInterface,
                 sizeof(outInterface));

    m_thread = std::thread([this]() { Run(); });

    return true;
}

// 写一条资源记录（owner / type / class / ttl / rdlength / rdata）
void WriteRecord(DnsWriter& writer, const Record& record)
{
    unsigned short recordType = (record.kind == kRecordA)     ? kTypeA
                                : (record.kind == kRecordPtr) ? kTypePtr
                                : (record.kind == kRecordSrv) ? kTypeSrv
                                                              : kTypeTxt;

    writer.WriteName(record.ownerDisplay);
    writer.WriteU16(recordType);
    writer.WriteU16(kClassIn);
    writer.WriteU32(record.ttl);

    size_t lengthOffset = writer.Size();
    writer.WriteU16(0);              // RDLENGTH 占位
    size_t rdataStart = writer.Size();

    switch (record.kind)
    {
    case kRecordA:
        writer.WriteU32(record.ipv4);
        break;
    case kRecordPtr:
        writer.WriteName(record.targetName);
        break;
    case kRecordSrv:
        writer.WriteU16(record.priority);
        writer.WriteU16(record.weight);
        writer.WriteU16(record.port);
        writer.WriteName(record.targetHost);
        break;
    case kRecordTxt:
        for (size_t t = 0; t < record.txt.size(); ++t)
        {
            std::string trimmed = record.txt[t];
            if (trimmed.size() > 255)
                trimmed.resize(255);
            writer.WriteU8((unsigned char)trimmed.size());
            for (char ch : trimmed)
                writer.WriteU8((unsigned char)ch);
        }
        break;
    default:
        break;
    }

    writer.PatchU16(lengthOffset, (unsigned short)(writer.Size() - rdataStart));
}

void Responder::Stop()
{
    if (!IsRunning() && m_socket == INVALID_SOCKET)
        return;

    // 退出前先发 Goodbye（RFC 6762 §10.1，TTL=0）：
    // 否则客户端会按 TTL 缓存最长 4500 秒（75 分钟），
    // 表现为"服务已经停了，打印机列表里还在，点进去连不上"。
    if (m_socket != INVALID_SOCKET && !m_records.empty())
    {
        for (size_t i = 0; i < m_records.size(); ++i)
            m_records[i].ttl = 0;

        std::vector<std::pair<std::string, unsigned short> > noQuestions;
        SendAll(true, 0, noQuestions);
    }

    m_running.store(false);

    if (m_socket != INVALID_SOCKET)
    {
        // 关闭套接字以唤醒阻塞中的 recvfrom
        ::closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }

    if (m_thread.joinable())
        m_thread.join();

    m_records.clear();
}

void Responder::SendAll(bool isAnnouncement, unsigned short queryId,
                        const std::vector<std::pair<std::string, unsigned short> >& questions)
{
    if (m_socket == INVALID_SOCKET || m_records.empty())
        return;

    DnsWriter writer;
    writer.Reset();

    writer.WriteU16(isAnnouncement ? 0 : queryId);
    writer.WriteU16(isAnnouncement ? 0x8400 : 0x8400);   // QR=1, AA=1
    writer.WriteU16((unsigned short)questions.size());
    writer.WriteU16(0);                                  // ANCOUNT 稍后回填
    writer.WriteU16(0);
    writer.WriteU16(0);

    for (size_t i = 0; i < questions.size(); ++i)
    {
        writer.WriteName(questions[i].first);
        writer.WriteU16(questions[i].second);
        writer.WriteU16(kClassIn);
    }

    // ---- 选出答案记录 ----
    std::vector<size_t> answers;
    for (size_t i = 0; i < m_records.size(); ++i)
    {
        const Record& record = m_records[i];
        unsigned short recordType = (record.kind == kRecordA)     ? kTypeA
                                    : (record.kind == kRecordPtr) ? kTypePtr
                                    : (record.kind == kRecordSrv) ? kTypeSrv
                                                                  : kTypeTxt;

        bool wanted = isAnnouncement;
        if (!wanted)
        {
            for (size_t q = 0; q < questions.size(); ++q)
            {
                // 名称不区分大小写；type 255 = ANY
                if (questions[q].first == record.owner &&
                    (questions[q].second == recordType || questions[q].second == 255))
                {
                    wanted = true;
                    break;
                }
            }
        }

        if (wanted)
            answers.push_back(i);
    }

    // ---- 选出附加记录 ----
    // DNS-SD 要求应答 PTR 时一并给出目标的 SRV/TXT（以及 SRV 主机的 A），
    // 客户端（尤其 Windows）一次就能拿全，不必再补查。
    std::vector<size_t> additionals;
    if (!isAnnouncement)
    {
        for (size_t a = 0; a < answers.size(); ++a)
        {
            const Record& answer = m_records[answers[a]];

            std::string related;
            if (answer.kind == kRecordPtr)
                related = ToLowerAscii(answer.targetName);
            else if (answer.kind == kRecordSrv)
                related = ToLowerAscii(answer.targetHost);
            else
                continue;

            for (size_t i = 0; i < m_records.size(); ++i)
            {
                if (m_records[i].owner != related)
                    continue;
                if (std::find(answers.begin(), answers.end(), i) != answers.end())
                    continue;
                if (std::find(additionals.begin(), additionals.end(), i) != additionals.end())
                    continue;
                additionals.push_back(i);
            }
        }
    }

    // 报文头 12 字节：ID(0) FLAGS(2) QDCOUNT(4) ANCOUNT(6) NSCOUNT(8) ARCOUNT(10)
    writer.PatchU16(6, (unsigned short)answers.size());
    writer.PatchU16(10, (unsigned short)additionals.size());

    for (size_t a = 0; a < answers.size(); ++a)
        WriteRecord(writer, m_records[answers[a]]);

    for (size_t a = 0; a < additionals.size(); ++a)
        WriteRecord(writer, m_records[additionals[a]]);

    sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(kMdnsPort);

    in_addr targetAddress;
    if (!ParseIpv4(kMulticastGroup, targetAddress))
        return;
    target.sin_addr = targetAddress;

    const std::vector<unsigned char>& data = writer.Data();
    ::sendto(m_socket, (const char*)data.data(), (int)data.size(), 0, (sockaddr*)&target,
             sizeof(target));
}

void Responder::SendUnsolicited()
{
    std::vector<std::pair<std::string, unsigned short> > noQuestions;
    SendAll(true, 0, noQuestions);
}

void Responder::Run()
{
    // RFC 6762：启动时连发几次公告，提高被发现概率
    for (int i = 0; i < 3 && m_running.load(); ++i)
    {
        SendUnsolicited();
        ::Sleep(300);
    }

    time_t lastAnnounce = ::time(nullptr);

    for (;;)
    {
        if (!m_running.load())
            break;

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(m_socket, &readSet);

        timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int ready = ::select(0, &readSet, nullptr, nullptr, &timeout);

        if (!m_running.load())
            break;

        if (ready > 0 && FD_ISSET(m_socket, &readSet))
        {
            unsigned char buffer[2048];
            sockaddr_in from;
            int fromLength = sizeof(from);
            int received = ::recvfrom(m_socket, (char*)buffer, sizeof(buffer), 0, (sockaddr*)&from,
                                      &fromLength);

            if (received >= 12)
            {
                DnsReader reader(buffer, (size_t)received);
                unsigned short flags = 0;
                unsigned short questionCount = 0;
                unsigned short id = 0;
                reader.ReadU16(0, id);
                reader.ReadU16(2, flags);
                reader.ReadU16(4, questionCount);

                // 只处理查询（QR=0）
                bool isQuery = (flags & 0x8000) == 0;
                if (isQuery && questionCount > 0 && questionCount <= 16)
                {
                    size_t position = 12;
                    std::vector<std::pair<std::string, unsigned short> > questions;
                    bool parseFailed = false;

                    for (unsigned short q = 0; q < questionCount; ++q)
                    {
                        std::string name;
                        size_t consumed = reader.ReadName(position, name);
                        if (consumed == 0)
                        {
                            parseFailed = true;
                            break;
                        }
                        position += consumed;

                        unsigned short type = 0;
                        if (!reader.ReadU16(position, type))
                        {
                            parseFailed = true;
                            break;
                        }
                        position += 4;   // type + class

                        questions.push_back(std::make_pair(ToLowerAscii(name), type));
                    }

                    if (!parseFailed && !questions.empty())
                        SendAll(false, id, questions);
                }
            }
        }

        // 周期性主动公告。缩短到 20 秒：客户端打开「添加打印机」时通常是持续浏览，
        // 这样即使某些客户端不发查询（或我们的组播接收被同端口的其它 mDNS 程序影响），
        // 也能在 20 秒内被发现。
        time_t now = ::time(nullptr);
        if (now - lastAnnounce >= 20)
        {
            SendUnsolicited();
            lastAnnounce = now;
        }
    }
}

Responder g_responder;

} // namespace

bool Start(const std::string& hostName, const std::string& address,
           const std::vector<PrinterService>& services, std::string& error)
{
    return g_responder.Start(hostName, address, services, error);
}

void Stop()
{
    g_responder.Stop();
}

bool IsRunning()
{
    return g_responder.IsRunning();
}

} // namespace IppMdns
} // namespace ps

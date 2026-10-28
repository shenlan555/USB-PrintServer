#include "pch.h"
#include "LanScanner.h"

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "Wmi.h"    // ps::WideToUtf8

namespace ps
{

namespace
{

std::once_flag g_winsockOnce;

// 本机的一个 IPv4 单播地址
struct LocalAddress
{
    std::string text;
    unsigned char address[4] = { 0, 0, 0, 0 };
    bool hasMask = false;
    unsigned char mask[4] = { 0, 0, 0, 0 };
};

// 对应 NetworkInterface.GetAllNetworkInterfaces() 中
// OperationalStatus == Up 且 NetworkInterfaceType != Loopback 的 IPv4 单播地址
void EnumerateLocalAddresses(std::vector<LocalAddress>& addresses)
{
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);

    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_PREFIX;

    ULONG result = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                          (IP_ADAPTER_ADDRESSES*)buffer.data(), &size);
    if (result == ERROR_BUFFER_OVERFLOW)
    {
        buffer.resize(size);
        result = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                        (IP_ADAPTER_ADDRESSES*)buffer.data(), &size);
    }

    if (result != NO_ERROR)
        return;

    for (IP_ADAPTER_ADDRESSES* adapter = (IP_ADAPTER_ADDRESSES*)buffer.data();
         adapter != nullptr; adapter = adapter->Next)
    {
        if (adapter->OperStatus != IfOperStatusUp)
            continue;

        // NetworkInterfaceType.Loopback
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;

        for (IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress;
             unicast != nullptr; unicast = unicast->Next)
        {
            if (unicast->Address.lpSockaddr == nullptr)
                continue;

            if (unicast->Address.lpSockaddr->sa_family != AF_INET)
                continue;

            sockaddr_in* ipv4 = (sockaddr_in*)unicast->Address.lpSockaddr;

            LocalAddress entry;
            memset(&entry.address, 0, sizeof(entry.address));
            memcpy(entry.address, &ipv4->sin_addr, 4);

            char text[32] = { 0 };
            _snprintf_s(text, sizeof(text), _TRUNCATE, "%u.%u.%u.%u",
                        entry.address[0], entry.address[1], entry.address[2], entry.address[3]);
            entry.text = text;

            // 对应 UnicastIPAddressInformation.IPv4Mask
            unsigned char prefix = unicast->OnLinkPrefixLength;
            if (prefix > 32)
                prefix = 32;

            entry.hasMask = true;
            for (int i = 0; i < 4; ++i)
            {
                int bits = (int)prefix - i * 8;
                if (bits >= 8)
                    entry.mask[i] = 0xFF;
                else if (bits <= 0)
                    entry.mask[i] = 0x00;
                else
                    entry.mask[i] = (unsigned char)(0xFF << (8 - bits));
            }

            addresses.push_back(entry);
        }
    }
}

// 返回 (子网前缀, 起始IP尾数, 结束IP尾数)
struct LocalNetwork
{
    std::string subnet;
    int start = 1;
    int end = 254;
};

// 对应 LanScanner.GetLocalNetworks()
std::vector<LocalNetwork> GetLocalNetworks()
{
    std::vector<LocalNetwork> networks;

    std::vector<LocalAddress> addresses;
    EnumerateLocalAddresses(addresses);

    for (const LocalAddress& entry : addresses)
    {
        // parts[0] + "." + parts[1] + "." + parts[2] + "."
        std::string subnet = std::to_string(entry.address[0]) + "." +
                             std::to_string(entry.address[1]) + "." +
                             std::to_string(entry.address[2]) + ".";

        if (entry.hasMask)
        {
            unsigned char networkBytes[4];
            unsigned char broadcastBytes[4];
            for (int i = 0; i < 4; ++i)
            {
                networkBytes[i] = (unsigned char)(entry.address[i] & entry.mask[i]);
                broadcastBytes[i] = (unsigned char)(networkBytes[i] | (unsigned char)~entry.mask[i]);
            }

            int start = networkBytes[3];
            int end = broadcastBytes[3];

            // 限制范围，最多扫描 /24 网段（254个IP）
            if (end - start > 254 || end <= start)
            {
                start = 1;
                end = 254;
            }

            LocalNetwork network;
            network.subnet = std::to_string(networkBytes[0]) + "." +
                             std::to_string(networkBytes[1]) + "." +
                             std::to_string(networkBytes[2]) + ".";
            network.start = (std::max)(1, start);
            network.end = (std::min)(254, end);
            networks.push_back(network);
        }
        else
        {
            LocalNetwork network;
            network.subnet = subnet;
            network.start = 1;
            network.end = 254;
            networks.push_back(network);
        }
    }

    return networks;
}

// 对应 ScanIpAsync：300ms 连接超时
bool TryConnect(const std::string& ip, int port)
{
    SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
        return false;

    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = ::htons((unsigned short)port);

    if (::inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1)
    {
        ::closesocket(sock);
        return false;
    }

    u_long nonBlocking = 1;
    ::ioctlsocket(sock, FIONBIO, &nonBlocking);

    bool connected = false;

    if (::connect(sock, (sockaddr*)&address, sizeof(address)) == 0)
    {
        connected = true;
    }
    else if (::WSAGetLastError() == WSAEWOULDBLOCK)
    {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(sock, &writeSet);

        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 300 * 1000;   // 300ms

        if (::select(0, nullptr, &writeSet, nullptr, &timeout) > 0)
        {
            int socketError = 0;
            int length = sizeof(socketError);
            if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&socketError, &length) == 0)
                connected = (socketError == 0);
        }
    }

    ::closesocket(sock);
    return connected;
}

// 对应 Dns.GetHostEntryAsync(ip).HostName；失败时用 IP 本身
std::string ResolveHostname(const std::string& ip)
{
    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    if (::inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1)
        return ip;

    char host[NI_MAXHOST] = { 0 };
    if (::getnameinfo((sockaddr*)&address, sizeof(address), host, sizeof(host),
                      nullptr, 0, NI_NAMEREQD) != 0)
        return ip;

    return host;
}

} // namespace

void EnsureWinsockInitialized()
{
    std::call_once(g_winsockOnce, []() {
        WSADATA data;
        ::WSAStartup(MAKEWORD(2, 2), &data);
    });
}

std::vector<ScanResult> ScanAsync(int apiPort, const ScanProgressFn& progress)
{
    std::vector<ScanResult> results;

    try
    {
        EnsureWinsockInitialized();

        std::vector<LocalNetwork> localNetworks = GetLocalNetworks();
        if (localNetworks.empty())
        {
            if (progress)
                progress("未检测到局域网连接");
            return results;
        }

        std::mutex lock;

        for (const LocalNetwork& network : localNetworks)
        {
            if (progress)
            {
                char message[256] = { 0 };
                _snprintf_s(message, sizeof(message), _TRUNCATE,
                            "正在扫描网段 %s* (共%d个IP)...",
                            network.subnet.c_str(), network.end - network.start + 1);
                progress(message);
            }

            // 分批并发扫描，每批50个IP
            for (int batchStart = network.start; batchStart <= network.end; batchStart += 50)
            {
                int batchEnd = (std::min)(batchStart + 49, network.end);

                std::vector<std::thread> batch;
                for (int i = batchStart; i <= batchEnd; ++i)
                {
                    std::string ip = network.subnet + std::to_string(i);

                    batch.push_back(std::thread([&, ip]() {
                        if (!TryConnect(ip, apiPort))
                            return;

                        {
                            std::lock_guard<std::mutex> guard(lock);
                            ScanResult result;
                            result.ipAddress = ip;
                            result.isReachable = true;
                            result.apiPort = apiPort;
                            results.push_back(result);
                        }

                        if (progress)
                            progress("  发现: " + ip);
                    }));
                }

                for (std::thread& worker : batch)
                {
                    if (worker.joinable())
                        worker.join();
                }
            }
        }

        // 尝试解析主机名
        for (ScanResult& result : results)
        {
            result.hostname = ResolveHostname(result.ipAddress);
        }

        if (progress)
        {
            char message[256] = { 0 };
            _snprintf_s(message, sizeof(message), _TRUNCATE,
                        "扫描完成，发现 %d 台打印服务器", (int)results.size());
            progress(message);
        }
    }
    catch (const std::exception& ex)
    {
        if (progress)
            progress(std::string("扫描异常: ") + ex.what());
    }
    catch (...)
    {
        if (progress)
            progress("扫描异常: 未知错误");
    }

    return results;
}

std::vector<std::string> GetLocalSubnetPrefixes()
{
    std::vector<std::string> prefixes;

    std::vector<LocalAddress> addresses;
    EnumerateLocalAddresses(addresses);

    for (const LocalAddress& entry : addresses)
    {
        std::string prefix;

        if (entry.hasMask)
        {
            unsigned char networkBytes[4];
            for (int i = 0; i < 4; ++i)
                networkBytes[i] = (unsigned char)(entry.address[i] & entry.mask[i]);

            prefix = std::to_string(networkBytes[0]) + "." +
                     std::to_string(networkBytes[1]) + "." +
                     std::to_string(networkBytes[2]) + ".";
        }
        else
        {
            prefix = std::to_string(entry.address[0]) + "." +
                     std::to_string(entry.address[1]) + "." +
                     std::to_string(entry.address[2]) + ".";
        }

        if (std::find(prefixes.begin(), prefixes.end(), prefix) == prefixes.end())
            prefixes.push_back(prefix);
    }

    return prefixes;
}

} // namespace ps

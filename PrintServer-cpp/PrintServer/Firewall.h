#pragma once

// Windows 防火墙入站规则
//
// 服务以 LocalSystem 身份运行，系统不会弹「是否允许访问网络」的提示，
// 默认入站全拦：局域网里的客户端既扫不到 9180，也连不上 9100/631，
// 打印表现为「客户端没反应、服务端日志一条都没有」。
// 首次安装/启动时把规则补上，避免用户手动放行。

#include <string>

namespace ps
{
namespace Firewall
{

struct Result
{
    bool ok = false;        // 规则当前可用（已存在或本次创建成功）
    bool created = false;   // 本次是否真的新建了规则
    std::string detail;     // 供日志输出的说明
};

// 程序级放行：对该 exe 的全部本地端口放行入站（TCP/UDP）
Result EnsureProgramRule(const std::wstring& exePath);

// 端口级放行：portsSpec 形如 L"631,9100-9199"（程序路径变更时仍然有效）
Result EnsureTcpPortsRule(const std::wstring& portsSpec);

// 当前进程是否具备管理员权限（创建防火墙规则需要）
bool IsElevated();

// 一条命令跑完并取回输出（不弹窗），供内部与诊断使用
std::string RunHidden(const std::wstring& commandLine, DWORD& exitCode);

}
}

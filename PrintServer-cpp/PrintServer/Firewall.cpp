#include "pch.h"
#include "Firewall.h"
#include "Wmi.h"

#include <string>
#include <vector>
#include <windows.h>

namespace ps
{
namespace Firewall
{

namespace
{

const wchar_t* kProgramRuleName = L"USB-PrintServer";
const wchar_t* kPortRuleName = L"USB-PrintServer Ports";

std::string AnsiToUtf8(const std::vector<char>& bytes)
{
    if (bytes.empty())
        return std::string();

    int wideLength = ::MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (wideLength <= 0)
        return std::string();

    std::wstring wide(wideLength, L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), &wide[0], wideLength);

    return WideToUtf8(wide);
}

// netsh 提示「没有匹配的规则」时视为不存在
bool OutputMeansNoRule(const std::string& output)
{
    return output.find("No rules match") != std::string::npos ||
           output.find("no rules match") != std::string::npos ||
           output.find("没有与指定条件相匹配的规则") != std::string::npos ||
           output.find("没有与指定条件相符的规则") != std::string::npos;
}

bool RuleExists(const wchar_t* ruleName, std::string& output)
{
    DWORD exitCode = 0;
    std::wstring command = std::wstring(L"netsh advfirewall firewall show rule name=\"") +
                           ruleName + L"\"";

    output = RunHidden(command, exitCode);
    if (OutputMeansNoRule(output))
        return false;

    // 输出里出现规则名（本地化环境可能没有英文标题）即认为存在
    return output.find(WideToUtf8(ruleName)) != std::string::npos;
}

Result EnsureRule(const wchar_t* ruleName, const std::wstring& addCommand)
{
    Result result;

    std::string showOutput;
    if (RuleExists(ruleName, showOutput))
    {
        result.ok = true;
        result.detail = "入站规则已存在";
        return result;
    }

    DWORD exitCode = 0;
    std::string addOutput = RunHidden(addCommand, exitCode);

    if (exitCode != 0)
    {
        result.ok = false;
        result.detail = "创建入站规则失败（退出码 " + std::to_string(exitCode) + "）：" +
                        addOutput;
        return result;
    }

    std::string verifyOutput;
    result.ok = RuleExists(ruleName, verifyOutput);
    result.created = result.ok;
    result.detail = result.ok ? "已创建入站规则" : "规则创建后未查询到，可能被组策略覆盖";
    return result;
}

} // namespace

bool IsElevated()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    TOKEN_ELEVATION elevation;
    memset(&elevation, 0, sizeof(elevation));
    DWORD returned = 0;

    bool elevated = false;
    if (::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned))
        elevated = (elevation.TokenIsElevated != 0);

    ::CloseHandle(token);
    return elevated;
}

std::string RunHidden(const std::wstring& commandLine, DWORD& exitCode)
{
    exitCode = (DWORD)-1;

    SECURITY_ATTRIBUTES attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!::CreatePipe(&readPipe, &writePipe, &attributes, 0))
        return std::string();

    ::SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;

    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    PROCESS_INFORMATION process;
    memset(&process, 0, sizeof(process));

    BOOL created = ::CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                    CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);

    ::CloseHandle(writePipe);

    if (!created)
    {
        ::CloseHandle(readPipe);
        return std::string();
    }

    std::vector<char> bytes;
    char buffer[4096];
    DWORD read = 0;
    while (::ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0)
        bytes.insert(bytes.end(), buffer, buffer + read);

    ::WaitForSingleObject(process.hProcess, 30000);

    DWORD code = 0;
    if (::GetExitCodeProcess(process.hProcess, &code))
        exitCode = code;

    ::CloseHandle(readPipe);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);

    return AnsiToUtf8(bytes);
}

Result EnsureProgramRule(const std::wstring& exePath)
{
    if (exePath.empty())
    {
        Result result;
        result.detail = "可执行文件路径为空";
        return result;
    }

    std::wstring command = std::wstring(L"netsh advfirewall firewall add rule name=\"") +
                           kProgramRuleName + L"\" dir=in action=allow program=\"" + exePath +
                           L"\" enable=yes profile=any";

    return EnsureRule(kProgramRuleName, command);
}

Result EnsureTcpPortsRule(const std::wstring& portsSpec)
{
    if (portsSpec.empty())
    {
        Result result;
        result.detail = "端口范围为空";
        return result;
    }

    std::wstring command = std::wstring(L"netsh advfirewall firewall add rule name=\"") +
                           kPortRuleName + L"\" dir=in action=allow protocol=TCP localport=" +
                           portsSpec + L" enable=yes profile=any";

    return EnsureRule(kPortRuleName, command);
}

}
}

#include "pch.h"
#include "ServiceHost.h"

#include <string>
#include <vector>
#include <stdexcept>

#include "LogHelper.h"
#include "Config.h"

namespace ps
{

ServiceHost* ServiceHost::s_instance = nullptr;

namespace
{

// ERROR_SERVICE_SPECIFIC_ERROR：.NET ServiceBase 在 ExitCode != 0 时使用
const DWORD kErrorServiceSpecificError = 1066;

// 对应 C# Server_OnLog 中的 EventLog.SourceExists / CreateEventSource("PrintServer", "Application")
void EnsureEventSource()
{
    HANDLE log = ::RegisterEventSourceW(nullptr, L"PrintServer");
    if (log != nullptr)
    {
        ::DeregisterEventSource(log);
        return;
    }

    // 源不存在：创建注册表登记（需要管理员权限，失败静默忽略）
    const wchar_t* subKey =
        L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\PrintServer";

    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, nullptr, 0,
                          KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    const wchar_t* module = L"%SystemRoot%\\System32\\PrintServer.exe";
    ::RegSetValueExW(key, L"EventMessageFile", 0, REG_EXPAND_SZ,
                     reinterpret_cast<const BYTE*>(module),
                     (DWORD)((wcslen(module) + 1) * sizeof(wchar_t)));

    DWORD types = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_INFORMATION_TYPE;
    ::RegSetValueExW(key, L"TypesSupported", 0, REG_DWORD,
                     reinterpret_cast<const BYTE*>(&types), sizeof(types));

    ::RegCloseKey(key);
}

// 对应 C# 的 int.Parse(ConfigurationHelper.GetSetting("ApiPort", "9180"))
// 解析失败时返回 false（C# 会抛 FormatException，调用方按启动失败处理）
bool ParseApiPort(const std::string& text, int& port)
{
    if (text.empty())
        return false;

    char* end = nullptr;
    long value = ::strtol(text.c_str(), &end, 10);

    if (end == text.c_str() || (end != nullptr && *end != '\0'))
        return false;

    port = (int)value;
    return true;
}

} // namespace

ServiceHost::ServiceHost()
    : m_statusHandle(nullptr)
    , m_stopEvent(nullptr)
    , m_exitCode(0)
{
    memset(&m_status, 0, sizeof(m_status));
    m_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;

    // 对应 C# 构造函数：serviceEventLog.Source = "PrintServer"; Log = "Application";
    EnsureEventSource();
}

ServiceHost::~ServiceHost()
{
    OnStop();

    if (m_stopEvent != nullptr)
    {
        ::CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
}

// ===========================================================================
// ServiceBase.Run 等价实现
// ===========================================================================

bool ServiceHost::Run()
{
    s_instance = this;

    SERVICE_TABLE_ENTRYW table[2];
    memset(table, 0, sizeof(table));
    table[0].lpServiceName = const_cast<LPWSTR>(L"PrintServer");
    table[0].lpServiceProc = &ServiceHost::ServiceMainStatic;

    if (!::StartServiceCtrlDispatcherW(table))
        return false;

    return true;
}

DWORD ServiceHost::StartStandalone()
{
    OnStart();
    return m_exitCode;
}

void ServiceHost::StopStandalone()
{
    OnStop();
}

void WINAPI ServiceHost::ServiceMainStatic(DWORD argc, LPWSTR* argv)
{
    if (s_instance != nullptr)
        s_instance->ServiceMain(argc, argv);
}

void ServiceHost::ServiceMain(DWORD /*argc*/, LPWSTR* /*argv*/)
{
    m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

    m_statusHandle = ::RegisterServiceCtrlHandlerExW(
        L"PrintServer", &ServiceHost::HandlerExStatic, this);

    if (m_statusHandle == nullptr)
        return;

    SetStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    OnStart();

    if (m_exitCode != 0)
    {
        // 对应 C#：ExitCode 非 0 时 ServiceBase 以 ERROR_SERVICE_SPECIFIC_ERROR 上报
        m_status.dwWin32ExitCode = kErrorServiceSpecificError;
        m_status.dwServiceSpecificExitCode = m_exitCode;
        SetStatus(SERVICE_STOPPED, kErrorServiceSpecificError, 0);
        return;
    }

    SetStatus(SERVICE_RUNNING, NO_ERROR, 0);

    ::WaitForSingleObject(m_stopEvent, INFINITE);

    SetStatus(SERVICE_STOP_PENDING, NO_ERROR, 3000);
    OnStop();
    SetStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

DWORD WINAPI ServiceHost::HandlerExStatic(DWORD control, DWORD eventType,
                                          LPVOID eventData, LPVOID context)
{
    ServiceHost* self = static_cast<ServiceHost*>(context);
    if (self == nullptr)
        return ERROR_CALL_NOT_IMPLEMENTED;

    return self->HandlerEx(control, eventType, eventData);
}

DWORD ServiceHost::HandlerEx(DWORD control, DWORD /*eventType*/, LPVOID /*eventData*/)
{
    switch (control)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        if (m_stopEvent != nullptr)
            ::SetEvent(m_stopEvent);
        return NO_ERROR;

    case SERVICE_CONTROL_INTERROGATE:
        SetStatus(m_status.dwCurrentState, m_status.dwWin32ExitCode, 0);
        return NO_ERROR;

    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void ServiceHost::SetStatus(DWORD currentState, DWORD win32ExitCode, DWORD waitHint)
{
    if (m_statusHandle == nullptr)
        return;

    if (currentState == SERVICE_START_PENDING || currentState == SERVICE_STOP_PENDING ||
        currentState == SERVICE_CONTINUE_PENDING || currentState == SERVICE_PAUSE_PENDING)
    {
        ++m_status.dwCheckPoint;
    }
    else
    {
        m_status.dwCheckPoint = 0;
    }

    m_status.dwCurrentState = currentState;
    m_status.dwWin32ExitCode = win32ExitCode;
    m_status.dwWaitHint = waitHint;

    m_status.dwControlsAccepted = 0;
    if (currentState != SERVICE_STOPPED && currentState != SERVICE_STOP_PENDING)
        m_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

    ::SetServiceStatus(m_statusHandle, &m_status);
}

// ===========================================================================
// 启动 / 停止（对应 C# OnStart / OnStop）
// ===========================================================================

void ServiceHost::OnStart()
{
    try
    {
        std::vector<PrinterConfig> configs = ConfigHelper::GetPrinterConfigs();
        if (configs.empty())
        {
            std::string errorMsg = "没有配置任何打印机，服务启动失败";
            WriteEventLog(errorMsg, true);
            Log::WriteLog("[服务] " + errorMsg);

            m_exitCode = 1066;
            OnStop();
            return;
        }

        Log::WriteLog("[服务] 已启动，共 " + std::to_string(configs.size()) + " 台打印机");

        std::string addresses;
        {
            std::vector<std::string> all = NetworkHelper::GetAllNetworkAddresses();
            for (size_t i = 0; i < all.size(); ++i)
            {
                if (i > 0)
                    addresses += ", ";
                addresses += all[i];
            }
        }

        char hostName[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
        DWORD hostNameLen = _countof(hostName);
        if (!::GetComputerNameA(hostName, &hostNameLen))
            hostName[0] = '\0';

        Log::WriteLog("[服务] 启动 " + std::to_string(configs.size()) + " 台打印机 (主机:" +
                      hostName + " IP:" + addresses + ")");

        for (size_t i = 0; i < configs.size(); ++i)
        {
            const PrinterConfig& config = configs[i];

            std::unique_ptr<TcpPrintServer> server(
                new TcpPrintServer(config.name, config.port));
            server->OnLog = [this](const std::string& message) { ServerOnLog(message); };

            m_servers.push_back(std::move(server));
        }

        // C# 为每台打印机单独起线程执行 Start()；此处 TcpPrintServer::Start 内部
        // 只做 bind/listen 并启动监听线程，本身不会阻塞，因此逐个启动等价。
        for (size_t i = 0; i < m_servers.size(); ++i)
            m_servers[i]->Start();

        // 启动 HTTP API 服务
        int apiPort = 9180;
        std::string apiPortText = ConfigHelper::GetSetting("ApiPort", "9180");
        if (!ParseApiPort(apiPortText, apiPort))
            throw std::runtime_error("ApiPort 配置无效: " + apiPortText);

        m_apiServer.reset(new HttpApiServer(configs, apiPort));
        m_apiServer->OnLog = [this](const std::string& message) { ServerOnLog(message); };
        m_apiServer->Start();

        // 启动 IPP 服务（与 RAW 并行）。
        // 关键：IPP 启动失败绝不能让 RAW 打印与控制 API 受影响，因此单独兜住异常，
        // 只记录警告，不改变退出码。
        std::string ippEnabledText = ConfigHelper::GetSetting("IppEnabled", "true");
        std::string ippEnabledLower;
        for (char c : ippEnabledText)
            ippEnabledLower.push_back((char)tolower((unsigned char)c));

        bool ippEnabled = !(ippEnabledLower == "false" || ippEnabledLower == "0" ||
                            ippEnabledLower == "no" || ippEnabledLower == "off");

        if (ippEnabled)
        {
            int ippPort = 631;
            std::string ippPortText = ConfigHelper::GetSetting("IppPort", "631");
            if (!ParseApiPort(ippPortText, ippPort) || ippPort <= 0 || ippPort > 65535)
                ippPort = 631;

            try
            {
                m_ippServer = std::make_shared<IppServer>(configs, ippPort);
                m_ippServer->OnLog = [this](const std::string& message) { ServerOnLog(message); };
                m_ippServer->Start();
            }
            catch (const std::exception& ippError)
            {
                std::string warning = "IPP 服务启动失败（RAW 打印与控制 API 不受影响）: " +
                                      std::string(ippError.what());
                Log::WriteLog("[服务] " + warning);
                WriteEventLog(warning, false);
                m_ippServer.reset();
            }
        }
        else
        {
            Log::WriteLog("[服务] IPP 服务已在配置中关闭 (IppEnabled=false)");
        }
    }
    catch (const std::exception& ex)
    {
        std::string errorMsg = "服务启动失败: " + std::string(ex.what());
        WriteEventLog(errorMsg, true);
        Log::WriteLog("[服务] " + errorMsg);

        // C# 此处 rethrow，异常最终由 ServiceBase 记录后终止进程。
        // 原生实现选择向 SCM 上报失败（退出码非 0）后干净退出，
        // 避免弹出「程序已停止工作」崩溃对话框，SCM 观察到的结果一致。
        m_exitCode = kErrorServiceSpecificError;
    }
}

void ServiceHost::OnStop()
{
    // 先停 IPP，再停控制 API（IppServer 不调用 HttpTerminate，由 HttpApiServer 统一终结）
    if (m_ippServer != nullptr)
    {
        m_ippServer->Stop();
        m_ippServer.reset();
    }

    if (m_apiServer != nullptr)
    {
        m_apiServer->Stop();
        m_apiServer.reset();
    }

    for (size_t i = 0; i < m_servers.size(); ++i)
        m_servers[i]->Stop();

    m_servers.clear();
}

void ServiceHost::ServerOnLog(const std::string& message)
{
    // 对应 C#：先尝试写事件日志（失败静默），再无条件写文件日志
    EnsureEventSource();
    WriteEventLog(message, false);

    Log::WriteLog(message);
}

} // namespace ps

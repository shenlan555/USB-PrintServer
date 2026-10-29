#pragma once

// Windows 服务宿主（对应 C# PrintServerService.cs 中的 PrintServerWindowsService : ServiceBase）
//
// 服务名 "PrintServer"，支持停止（CanStop=true），不支持暂停（CanPauseAndContinue=false）。
// AutoLog 语义：启动/停止时写一条 Application 日志。
//
// 启动流程（严格对应 C# OnStart）：
//   1. 未配置任何打印机 → 写事件日志、退出码 1066、停止服务
//   3. ConfigurationHelper.GetPrinterConfigs() → 为空则写事件日志、退出码 1066、停止服务
//   4. 每台打印机起一个 TcpPrintServer 线程
//   5. 启动 HttpApiServer（端口取 App.config 的 ApiPort，默认 9180）

#include <string>
#include <vector>
#include <memory>
#include <windows.h>

#include "PrintService.h"
#include "HttpApiService.h"
#include "IppService.h"

namespace ps
{

class ServiceHost
{
public:
    ServiceHost();
    ~ServiceHost();

    ServiceHost(const ServiceHost&) = delete;
    ServiceHost& operator=(const ServiceHost&) = delete;

    // 对应 C# ServiceBase.Run(ServicesToRun)：连接 SCM 并阻塞到服务停止
    bool Run();

    // 诊断用：不经 SCM 直接执行启动主体，返回退出码（0 表示启动成功）
    DWORD StartStandalone();

    // 诊断用：停止并释放所有打印机服务与 HTTP API
    void StopStandalone();

private:
    static void WINAPI ServiceMainStatic(DWORD argc, LPWSTR* argv);
    static DWORD WINAPI HandlerExStatic(DWORD control, DWORD eventType, LPVOID eventData, LPVOID context);

    void ServiceMain(DWORD argc, LPWSTR* argv);
    DWORD HandlerEx(DWORD control, DWORD eventType, LPVOID eventData);

    void OnStart();
    void OnStop();
    void ServerOnLog(const std::string& message);

    void SetStatus(DWORD currentState, DWORD win32ExitCode, DWORD waitHint);

    SERVICE_STATUS_HANDLE m_statusHandle;
    SERVICE_STATUS m_status;
    HANDLE m_stopEvent;

    std::vector<std::unique_ptr<TcpPrintServer>> m_servers;
    std::unique_ptr<HttpApiServer> m_apiServer;
    std::shared_ptr<IppServer> m_ippServer;     // 与 RAW 并行的 IPP 服务

    // 对应 C# OnStart 中 this.ExitCode 的赋值（无可用配置时为 1066）
    DWORD m_exitCode;

    static ServiceHost* s_instance;
};

} // namespace ps

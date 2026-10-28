#pragma once

// IPP 服务（RFC 8011 模型 + RFC 8010 承载），与 RAW(9100) 并行运行。
//
// 设计要点（"稳妥/兼容"优先）：
//   * 每个请求都在独立线程处理：提交文档不阻塞 Get-Job-Attributes 轮询。
//   * 请求体边收边落盘（不整包进内存），并有硬性大小上限。
//   * 打印在每台打印机各自的 worker 线程串行执行，作业提交立即返回。
//   * 任何内部异常都会被兜住并转成 server-error-internal-error，
//     HTTP 层始终返回 200 + 合法 IPP 体（否则客户端会一直等到超时）。
//   * IPP 启动失败不影响 RAW 与控制 API（由上层 try/catch 兜底）。

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "IppProtocol.h"
#include "IppRaster.h"
#include "PrintService.h"

namespace ps
{

// 对应 RFC 8011 §5.3.7 的 job-state 枚举值
enum IppJobState
{
    IppJobPending            = 3,
    IppJobPendingHeld        = 4,
    IppJobProcessing         = 5,
    IppJobProcessingStopped  = 6,
    IppJobCanceled           = 7,
    IppJobAborted            = 8,
    IppJobCompleted          = 9
};

struct IppJob
{
    int id = 0;
    int printerIndex = 0;
    std::string name;
    std::string userName;
    std::string documentFormat;
    int state = IppJobPending;
    std::string stateReasons = "job-incoming";
    long long createdUpTime = 0;
    long long processingUpTime = 0;
    long long completedUpTime = 0;
    long long documentBytes = 0;
    size_t documentOffset = 0;      // 文档在临时文件中的起始偏移
    std::string tempPathUtf8;       // 文档落盘路径（作业结束时删除）
    bool documentAttached = false;
    int copies = 1;                 // 份数（1..99）
    std::atomic<bool> cancelRequested;

    IppJob() : cancelRequested(false) {}
};

using IppJobPtr = std::shared_ptr<IppJob>;

class IppServer : public std::enable_shared_from_this<IppServer>
{
public:
    IppServer(const std::vector<PrinterConfig>& printers, int port);
    ~IppServer();

    IppServer(const IppServer&) = delete;
    IppServer& operator=(const IppServer&) = delete;

    void Start();
    void Stop();

    bool IsRunning() const { return m_running.load(); }
    int Port() const { return m_port; }

    std::function<void(const std::string&)> OnLog;

private:
    struct PrinterRuntime
    {
        std::mutex mutex;
        std::condition_variable condition;
        std::deque<IppJobPtr> queue;
        std::thread worker;
        bool stopRequested = false;
        std::atomic<int> currentJobId;

        std::vector<std::string> pdlFormats;   // 该打印机可接受的 PDL MIME 列表
        int dpiX = 600;                        // 打印机原生分辨率（启动时查询，用于属性声明）
        int dpiY = 600;

        // 驱动真实能力（彩色/双面/纸张/分辨率），启动时探测一次。
        // IPP 的 color-supported / sides-supported / media-supported 都来自这里，
        // 而不是硬编码或配置猜——彩色机、针式机的纸张列表差异很大。
        IppRaster::PrinterCapabilities caps;
        std::string makeAndModel;              // 启动时缓存（避免每次查询都走 WMI）
        std::string manufacturer;
        std::string deviceId;                  // printer-device-id
        std::string uuid;                      // urn:uuid:...（同一台机器上保持稳定）

        PrinterRuntime() : currentJobId(0) {}
    };

    // ---- 生命周期 ----
    void ListenLoop();
    void WorkerLoop(int printerIndex);
    void StartWorkers();
    void StopWorkers();
    void Log(const std::string& message);

    // ---- 请求处理 ----
    void HandleRequestThread(uintptr_t requestId, const std::string& path,
                             const std::string& query, const std::string& contentType,
                             const std::string& clientIp, bool hasEntityBody);
    void DispatchOperation(const Ipp::Request& request, const std::string& clientIp,
                           const std::wstring& bodyFilePath, size_t totalBodyBytes,
                           bool bodyComplete, bool& bodyClaimedOut,
                           std::vector<unsigned char>& responseBytes, int& ippStatusOut);

    // ---- 操作实现 ----
    void HandleGetPrinterAttributes(const Ipp::Request& request, int printerIndex,
                                    Ipp::ResponseBuilder& builder);
    void HandleValidateJob(const Ipp::Request& request, int printerIndex,
                           Ipp::ResponseBuilder& builder);
    void HandlePrintJob(const Ipp::Request& request, int printerIndex,
                        const std::wstring& bodyFilePath, size_t totalBodyBytes,
                        bool& bodyClaimed, Ipp::ResponseBuilder& builder);
    void HandleCreateJob(const Ipp::Request& request, int printerIndex,
                         Ipp::ResponseBuilder& builder);
    void HandleSendDocument(const Ipp::Request& request, int printerIndex,
                            const std::wstring& bodyFilePath, size_t totalBodyBytes,
                            bool& bodyClaimed, Ipp::ResponseBuilder& builder);
    void HandleGetJobs(const Ipp::Request& request, int printerIndex,
                       Ipp::ResponseBuilder& builder);
    void HandleGetJobAttributes(const Ipp::Request& request, int printerIndex,
                                Ipp::ResponseBuilder& builder);
    void HandleCancelJob(const Ipp::Request& request, int printerIndex,
                         Ipp::ResponseBuilder& builder);
    // Create-Job + Send-Document(last-document=false) 之后的收尾操作（RFC 8011 §4.3.4）
    void HandleCloseJob(const Ipp::Request& request, int printerIndex,
                        Ipp::ResponseBuilder& builder);
    // Hold-Job / Release-Job（hold=true 表示挂起）
    void HandleHoldJob(const Ipp::Request& request, int printerIndex, bool hold,
                       Ipp::ResponseBuilder& builder);
    // Restart-Job：已完成作业重新排队（文档已释放时返回 not-possible）
    void HandleRestartJob(const Ipp::Request& request, int printerIndex,
                          Ipp::ResponseBuilder& builder);
    void HandlePurgeJobs(const Ipp::Request& request, int printerIndex,
                         Ipp::ResponseBuilder& builder);
    void HandleIdentifyPrinter(const Ipp::Request& request, int printerIndex,
                               Ipp::ResponseBuilder& builder);
    void HandlePauseResume(const Ipp::Request& request, int printerIndex, bool pause,
                           Ipp::ResponseBuilder& builder);

    // ---- 属性输出 ----
    void AddOperationAttributes(Ipp::ResponseBuilder& builder) const;
    void AddPrinterAttributes(Ipp::ResponseBuilder& builder, int printerIndex) const;
    void AddJobAttributes(Ipp::ResponseBuilder& builder, const IppJobPtr& job) const;

    // ---- 作业管理 ----
    IppJobPtr CreateJob(int printerIndex, const std::string& name, const std::string& userName,
                        const std::string& documentFormat);
    IppJobPtr FindJob(int jobId);
    // 找本打印机上"已创建但还没收到文档"的作业（Send-Document 容错用）
    IppJobPtr FindPendingJobWithoutDocument(int printerIndex);
    void EnqueueJob(const IppJobPtr& job);
    void RemoveJobFiles(const IppJobPtr& job);
    int QueuedCount(int printerIndex) const;
    int PrinterState(int printerIndex) const;
    bool IsFormatSupported(int printerIndex, const std::string& format) const;
    std::string PrinterUriFor(int printerIndex, const std::string& address) const;
    // 作业 URI 统一用 ipp://host:port/jobs/<id>（与 CUPS / Windows IPP 客户端一致）
    std::string JobUriFor(const IppJobPtr& job) const;

    // ---- 文档提交（Phase B：按格式分流）----
    // 把作业自带的文档段落地成独立文件（PDF 渲染需要文件从 %PDF 开始）
    std::wstring MaterializeDocument(const IppJobPtr& job, const wchar_t* extension,
                                     std::string& error) const;
    // 把作业文档打到指定打印机：PDF 走栅格化+XPS，其余走 RAW 直通
    bool PrintJobDocument(const IppJobPtr& job, int printerIndex, bool& canceled,
                          std::string& error);

    // ---- 工具 ----
    int ResolvePrinterIndex(const std::string& printerUri) const;
    int ResolveJobId(const std::string& jobUri) const;
    std::string SanitizeName(const std::string& name) const;
    std::wstring MakeJobTempPath() const;
    long long UpTime() const;

    std::vector<PrinterConfig> m_printers;
    std::vector<std::unique_ptr<PrinterRuntime>> m_runtimes;
    int m_port;
    std::wstring m_prefix;          // URL 前缀，例如 http://+:631/

    std::atomic<bool> m_running;
    std::thread m_listenThread;
    std::atomic<int> m_activeRequests;
    std::mutex m_drainMutex;
    std::condition_variable m_drainCondition;

    uintptr_t m_queue;              // HTTP.sys 请求队列句柄

    mutable std::mutex m_jobsMutex;
    std::map<int, IppJobPtr> m_jobs;
    int m_nextJobId;

    long long m_startTime;
    size_t m_maxDocumentBytes;
    bool m_duplexSupported;
    bool m_colorSupported;
    bool m_pdfRenderingEnabled;     // 是否具备并启用 PDF 免驱渲染
    int m_renderDpi;                // PDF/栅格渲染目标分辨率
    bool m_mdnsEnabled;             // 是否做 mDNS/DNS-SD 广播（自动发现）
    std::string m_primaryAddress;   // 首个本机 IPv4，用于拼 job-uri
    // 按位表示第 N 台打印机是否被暂停。用 64 位：稳定槽位允许有空洞，
    // 编号可能远大于打印机台数，int（32 位）移位会越界。
    std::atomic<unsigned long long> m_pausedMask;
};

} // namespace ps

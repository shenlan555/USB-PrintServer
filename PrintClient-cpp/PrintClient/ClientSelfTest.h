#pragma once

// 命令行自检模式（仅供开发与现场诊断，不显示界面）
//
// 用法: PrintClient.exe /clienttest <server> <apiPort> [printerIndex] [reportFile] [--install [installName]]
//   连接测试 → 获取打印机信息 → 打印完整打印机表 → 下载驱动包 → 解压
//   报告默认写入 %TEMP%\client_selftest_report.txt（UTF-8 带 BOM）
//
//   --install：在完成上述检查后，走真实安装路径创建打印机（需要管理员权限），
//              随后独立校验并清理（删除打印机队列与端口，不动驱动库）。
//              安装名为空时使用 "<打印机名>(共享)"，与界面里的默认值一致。

#include <string>
#ifdef _DEBUG

namespace ps
{

// 命令行自检参数
struct ClientSelfTestOptions
{
    std::wstring server;
    int apiPort = 0;
    int printerIndex = 0;
    std::wstring reportPath;

    // 是否执行真实安装 + 独立校验 + 清理
    bool install = false;
    // 安装（打印机队列）名称；为空时用 "<printerName>(共享)"
    std::wstring installName;
};

// 返回失败项数量（0 表示全部通过）
int RunClientSelfTest(const ClientSelfTestOptions& options);

} // namespace ps
#endif // _DEBUG

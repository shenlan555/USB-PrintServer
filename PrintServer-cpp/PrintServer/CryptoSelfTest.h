#pragma once

#include <string>
#ifdef _DEBUG

namespace ps
{
// 加密/JSON 等价性自检
//
// 读取由 C# 版生成的测试向量文件，在 C++ 侧重算并交叉解密，
// 输出 PASS/FAIL 报告。用于确保原生实现与 .NET 版字节级一致，
// 从而保证老授权文件与 PHP 服务端继续可用。
//
// 返回 0 表示全部通过。
int RunCryptoSelfTest(const std::wstring& vectorFilePath, const std::wstring& reportPath);

// TCP 打印服务端到端自测（双栈监听 / 收数据 / 落盘 / RAW 打印尝试）
// 返回 0 表示全部通过。
int RunTcpSelfTest(const std::wstring& reportPath);

// HTTP API 端到端自测（/api/info、/api/test、404、OPTIONS 预检）
// 返回 0 表示全部通过。
int RunApiSelfTest(const std::wstring& reportPath);

// 服务宿主端到端自测（授权门禁 / 打印机端口 / HTTP API / 停止释放资源）
// 返回 0 表示全部通过。
int RunServiceSelfTest(const std::wstring& reportPath);

}
#endif // _DEBUG

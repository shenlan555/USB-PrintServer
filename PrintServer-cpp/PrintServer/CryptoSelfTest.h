#pragma once

#include <string>
#ifdef _DEBUG

namespace ps
{
// 加密/JSON 等价性自检
//
// 读取测试向量文件（PrintServer-cpp/testdata/crypto_vectors.txt），在 C++ 侧重算并交叉解密，
// 输出 PASS/FAIL 报告。期望值由独立实现（.NET）算出，用于确认算法约定逐字节一致，
// 从而保证既有的 .lic 离线文件与 PHP 服务端继续可用。
//
// 返回 0 表示全部通过。
int RunCryptoSelfTest(const std::wstring& vectorFilePath, const std::wstring& reportPath);

// TCP 打印服务端到端自测（双栈监听 / 收数据 / 落盘 / RAW 打印尝试）
// 返回 0 表示全部通过。
int RunTcpSelfTest(const std::wstring& reportPath);

// HTTP API 端到端自测（/api/info、/api/test、404、OPTIONS 预检）
// 返回 0 表示全部通过。
int RunApiSelfTest(const std::wstring& reportPath);

// 服务宿主端到端自测（启动门禁 / 打印机端口 / HTTP API / 停止释放资源）
// 返回 0 表示全部通过。
int RunServiceSelfTest(const std::wstring& reportPath);

}
#endif // _DEBUG

# print-soft（网络打印服务器 / 打印客户端）

把接在 Windows 电脑上的 USB / 本地打印机，变成一台 **IPP 网络打印机**：局域网内其他电脑像添加普通网络打印机一样直接连接，无需共享账户密码，支持 PDF 免驱打印、多打印机共享、Windows 服务模式运行。

> 本仓库为 **开源免费版**：已移除全部商业授权与反调试机制（详见下文「授权说明」），可自由使用、修改与编译。

## 仓库结构

```
print-soft/
├── PrintServer-cpp/    服务端（装在共享打印机的电脑上）
├── PrintClient-cpp/    客户端（装在需要连接打印机的电脑上，辅助自动装驱动）
└── 说明日志.txt        版本更新日志（v1.2.0 ~ v1.4.2）
```

## 主要功能（v1.4.2）

- 完整 IPP 打印服务（RFC 8010/8011，端口 631），GUI 模式与服务模式同时启用
- mDNS 自动发现 + RAW 9100 端口映射（9100 → /ipp/print/9100，9101 → /ipp/print/9101）
- PDF 渲染引擎采用 PDFium（BSD-3-Clause，运行时动态加载 `pdfium.dll`）
- 打印机能力（彩色/纸张/双面/分辨率）直接读取驱动能力，针式机连续纸、彩色机不再被误判为黑白 A4
- 输出纸张跟随 PDF 页面尺寸；大幅面按面积限制渲染分辨率，避免高 DPI 撑爆内存
- 驱动按 DriverStore 整包抓取，SHA256 清单 + RSA-2048 签名，客户端验签通过才安装
- `/api/driver` 接口 HMAC-SHA256 + 时间戳鉴权，防局域网随意抓取与重放
- 客户端启动自动提权（UAC），驱动/打印机安装回读系统状态校验

## 编译

- IDE：Visual Studio 2022（Windows 桌面 C++ 工作负载）
- 分别打开 `PrintServer-cpp/PrintServer.sln` 与 `PrintClient-cpp/PrintClient.sln` 编译
- 也可使用各目录下的 `编译.bat`
- `pdfium.dll` 必须与 `PrintServer.exe` 同目录（仓库已附带一份；如需更新版本请从 PDFium 官方发布获取）
- Win7 需安装 KB2999226，否则 PDF 免驱打印不可用（自动降级为 PDL 直通，RAW 打印不受影响）

## 授权说明（重要变更）

本项目原为商业授权软件，开源版做了如下处理：

1. `PrintServer-cpp/PrintServer/BuildConfig.h` 中新增总开关 `LicenseEnabled = false`：
   - 授权校验（在线/离线激活、定时复验、license.dat 缓存）全部短路，启动即视为已授权；
   - 反调试/黑名单/惩罚模块（AntiDebug）已**从工程中整体删除**（源文件与编译项均已移除），程序不再对用户环境做任何检测；
   - 授权服务器地址与授权加密密钥已替换为占位符，不会外联任何服务器。
2. PHP 授权管理平台（license-server）与密钥归档**不在本仓库中**。
3. 如需恢复商业授权：把 `LicenseEnabled` 改回 `true`，填写授权服务器地址与密钥，并补回 LicenseHelper 中被短路的分支（原实现代码均已保留）。
4. 驱动包 RSA 签名与 `/api/driver` HMAC 鉴权属于**分发安全功能而非授权**，开源版保留；请注意 `DriverSigning.cpp` 中内嵌的 RSA 私钥/公钥是原项目的密钥对，正式对外发布前建议使用自己的密钥对替换（替换后服务端与客户端需一起重编）。

## 许可证

见 [LICENSE](LICENSE)。

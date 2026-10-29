# USB-PrintServer（网络打印服务器 / 打印客户端）

把接在 Windows 电脑上的 USB / 本地打印机，变成一台 **RAW/IPP 网络打印机**：局域网内其他电脑像添加普通网络打印机一样直接连接，无需共享账户密码，支持 PDF 免驱打印、多打印机共享、Windows 服务模式运行。

## 仓库结构

```
USB-PrintServer/
├── .github/workflows/  GitHub Actions 自动编译
├── PrintServer-cpp/    服务端（装在共享打印机的电脑上）
├── PrintClient-cpp/    客户端（装在需要连接打印机的电脑上，辅助自动装驱动）
└── 说明日志.txt        版本更新日志（v1.2.0 ~ v1.4.2）
```

## 主要功能（v1.4.2）

- 完整 IPP 打印服务（RFC 8010/8011，端口 631），GUI 模式与服务模式同时启用
- PDF 渲染引擎采用 PDFium（BSD-3-Clause，运行时动态加载 `pdfium.dll`）
- 打印机能力（彩色/纸张/双面/分辨率）直接读取驱动能力，针式机连续纸、彩色机不再被误判为黑白 A4
- 输出纸张跟随 PDF 页面尺寸；大幅面按面积限制渲染分辨率，避免高 DPI 撑爆内存
- 驱动按 DriverStore 整包抓取，SHA256 清单 + RSA-2048 签名，客户端验签通过才安装
- `/api/driver` 接口 HMAC-SHA256 + 时间戳鉴权，防局域网随意抓取与重放
- 客户端启动自动提权（UAC），驱动/打印机安装回读系统状态校验

## 编译

### 方式一：GitHub Actions 自动编译（推荐）

每次 push 或提交 PR 都会自动编译服务端与客户端（x64 Release），产物在 **Actions → 对应运行 → Artifacts** 下载：

| 构件 | 内容 |
|---|---|
| `PrintServer-x64` | `PrintServer.exe` + `pdfium.dll` |
| `PrintClient-x64` | `PrintClient.exe` |

工作流位于 `.github/workflows/build.yml`，使用 `windows-2022` 镜像 + MSBuild（v143 工具集，含 MFC）。
也可在 Actions 页面手动触发（`workflow_dispatch`）。

镜像自带 MFC 时「Ensure MSVC toolset and MFC」直接秒过，只有确实缺失才调用 `vs_installer` 补装（见
`.github/actions/ensure-mfc`）。除编译外还有一个 `Self-test (x64 Debug)` 作业，跑加密向量、HTTP API、
TCP 打印三条自测链路，任一失败则本次运行标红；各条目的报告会作为 `selftest-reports` 构件上传。

### 方式二：本地编译

- IDE：Visual Studio 2022（需勾选「MFC 支持」与「C++ 桌面开发」）
- 分别打开 `PrintServer-cpp/PrintServer.sln` 与 `PrintClient-cpp/PrintClient.sln` 编译
- 也可使用各目录下的 `编译.bat`
- 工程不再写死工具集与 SDK 版本：工具集取 IDE 默认（VS2022 → `v143`，VS2019 → `v142`），
  `WindowsTargetPlatformVersion` 为 `10.0`（取本机已装的最新 10.x SDK），因此无需手工改项目属性
- `pdfium.dll` 必须与 `PrintServer.exe` 同目录（仓库已附带一份；如需更新版本请从 PDFium 官方发布获取）
- Win7 需安装 KB2999226，否则 PDF 免驱打印不可用（自动降级为 PDL 直通，RAW 打印不受影响）

### 冒烟自测（仅 Debug 构建）

Debug 版带命令行自测入口（Release 交付版已剥离，避免被当成免授权启动入口）。全部通过时进程退出码为 0，
任一条目失败为 1，可直接接进脚本或 CI：

```bat
PrintServer.exe /cryptotest PrintServer-cpp\testdata\crypto_vectors.txt crypto_report.txt
PrintServer.exe /apitest api_report.txt
PrintServer.exe /tcptest tcp_report.txt
```

- `/cryptotest`：MD5 / HMAC-SHA256 / AES-256-CBC / DPAPI 作用域 / 离线 .lic 链路，以及 App.config 读取；
  期望值由独立实现（.NET）按 `key = SHA256(secret)`、`iv = SHA256(secret + "iv_salt")` 前 16 字节计算，
  与源码实现互为验证。DPAPI 密文绑定具体机器，向量文件里留空表示跳过该条交叉解密（仍做本机往返校验）
- `/apitest`：本地起 HTTP API（端口 19180），验证 `/api/test`、`/api/info`、404、OPTIONS 预检
- `/tcptest`：本地起 RAW 打印服务（端口 19100），验证 IPv4/IPv6 接入、收数据与失败路径日志

## 使用说明

- **软件完全免费**，无需激活、注册或联网验证，下载解压即可使用
- 服务端需以管理员身份运行（用于注册 Windows 服务、安装打印驱动）
- 首次使用：服务端选择要共享的打印机 → 安装服务 → 启动服务；客户端在局域网内扫描或输入服务端电脑名/IP，按提示自动安装驱动与打印机
- 驱动包内置 RSA 签名校验，用于防止驱动包被篡改；`/api/driver` 接口带 HMAC 时间戳鉴权，防止局域网内随意抓取

## 许可证

见 [LICENSE](LICENSE)。

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

### 方式二：本地编译

- IDE：Visual Studio 2022（需勾选「MFC 支持」与「C++ 桌面开发」）
- 分别打开 `PrintServer-cpp/PrintServer.sln` 与 `PrintClient-cpp/PrintClient.sln` 编译
- 也可使用各目录下的 `编译.bat`
- 工程默认工具集为 v142；若本机未安装，可在项目属性中改为 v143（源码兼容）
- `pdfium.dll` 必须与 `PrintServer.exe` 同目录（仓库已附带一份；如需更新版本请从 PDFium 官方发布获取）
- Win7 需安装 KB2999226，否则 PDF 免驱打印不可用（自动降级为 PDL 直通，RAW 打印不受影响）

## 使用说明

- **软件完全免费**，无需激活、注册或联网验证，下载解压即可使用
- 服务端需以管理员身份运行（用于注册 Windows 服务、安装打印驱动）
- 首次使用：服务端选择要共享的打印机 → 安装服务 → 启动服务；客户端在局域网内扫描或输入服务端电脑名/IP，按提示自动安装驱动与打印机
- 驱动包内置 RSA 签名校验，用于防止驱动包被篡改；`/api/driver` 接口带 HMAC 时间戳鉴权，防止局域网内随意抓取
- 正式对外发布时，建议将 `DriverSigning.cpp` 中内嵌的 RSA 密钥对替换为自己的（服务端与客户端需一起重新编译）

## 许可证

见 [LICENSE](LICENSE)。

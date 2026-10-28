#pragma once

// 编译时固定的授权配置
//
// 对应 C# 版 AssemblyInfo.cs 里的程序集特性（AssemblyLicenseUrl /
// AssemblySecretKey / AssemblyQQGroup / AssemblyLicenseValidationInterval），
// 语义相同：编译期写死，运行时不可通过配置文件修改。
// 若要更换授权服务器，改这里后重新编译。
namespace ps
{
namespace BuildConfig
{
    // 版本号（须与 PrintServer.rc 的 VS_VERSION_INFO 保持一致）
    inline constexpr const char* VersionA = "1.4.2";

    // 程序集元数据（对应 AssemblyInfo.cs 的 AssemblyProduct/AssemblyCompany/AssemblyDescription）
    inline constexpr const char* ProductName = "网络打印服务器";
    inline constexpr const char* CompanyName = "浪里个浪";
    inline constexpr const char* Description = "将本地打印机共享为网络打印机的服务程序";

    // ===== 授权总开关 =====
    // 开源免费版：授权系统整体关闭（LicenseHelper / AntiDebug / LicenseDlg 均被短路），
    // 程序启动即视为已授权，不再联网验证、不再生成 license.dat、不再执行反调试惩罚。
    // 如需恢复商业授权，改回 true 并填写下方密钥后重新编译。
    inline constexpr bool LicenseEnabled = false;

    // 授权服务器 API 地址（LicenseEnabled=false 时不会被使用）
    inline constexpr const char* LicenseUrl = "";

    // C# CryptoHelper.DecodeLicenseUrl() 中读取失败时的兜底地址
    inline constexpr const char* LicenseUrlFallback = "";

    // 授权加密密钥（.lic 离线文件与 HMAC 验签使用）——开源版已移除真实密钥
    inline constexpr const char* SecretKey = "REMOVED-FREE-VERSION";

    // C# LicenseHelper.GetDefaultSecretKey() 中读取失败时的兜底密钥
    inline constexpr const char* SecretKeyFallback = "REMOVED-FREE-VERSION";

    // 官方 QQ 交流群号（显示在授权激活窗口底部）
    inline constexpr const char* QQGroup = "782326498";

    // 授权验证间隔（小时）
    inline constexpr int LicenseValidationIntervalHours = 1;

    // /api/driver 下载鉴权共享密钥（HMAC-SHA256）。
    // 与客户端 PrintClient/BuildConfig.h 必须保持一致。
    // 注意：这是对称密钥、随客户端一起分发，只能挡掉局域网上的随意抓取与扫描，
    //       不构成强认证；驱动包的防篡改由清单的 RSA 签名负责。
    inline constexpr const char* ApiAuthSecret =
        "vsM2qFQq2wD1oTWd+bVfF27H0GHQ7Qk4klbTlgRXmDr0TswxiKdQg8BdOGkGgGzL";

    // /api/driver 鉴权允许的时间偏差（秒），超出即视为重放
    inline constexpr int ApiAuthMaxSkewSeconds = 300;
}
}

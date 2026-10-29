#pragma once

// 编译期常量：版本号、产品信息、接口鉴权参数。
// 全部在编译时写死，运行时不可通过配置文件修改；要改就改这里后重新编译。
namespace ps
{
namespace BuildConfig
{
    // 版本号（须与 PrintServer.rc 的 VS_VERSION_INFO 保持一致）
    inline constexpr const char* VersionA = "1.4.3";

    // 产品元数据
    inline constexpr const char* ProductName = "网络打印服务器";
    inline constexpr const char* CompanyName = "浪里个浪";
    inline constexpr const char* Description = "将本地打印机共享为网络打印机的服务程序";

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

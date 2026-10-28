#pragma once

// App.config 读取（对应 C# 的 ConfigurationManager.AppSettings）
//
// 保持与 .NET 完全相同的文件格式（PrintServer.exe.config），
// 这样客户现场已有的配置文件可直接沿用，无需重新配置打印机。

#include <string>

namespace ps
{
namespace Config
{
    // 程序所在目录（不含末尾反斜杠）
    std::wstring GetExeDirectory();

    // 对应 ConfigurationManager 读取的配置文件：<exe>.config
    std::wstring GetConfigPath();

    // 读取 appSettings 中的项；不存在返回 defaultValue（UTF-8 传入传出）
    //
    // 注意：会先剥离 XML 注释。App.config 中 PrinterN_* 的示例值是注释掉的，
    //       若不剥离注释会被误读为真实配置。
    std::string GetSetting(const std::string& key, const std::string& defaultValue);

    // 首次运行（配置文件不存在）时按内置模板生成 <exe>.config。
    // 返回 true 表示本次新建；已存在则返回 false 且不改动现场配置。
    //
    // 之所以不再由构建脚本复制 App.config：现场升级 exe 时不会被模板覆盖，
    // 也不会出现"程序目录只读/权限不足导致配置文件复制失败"的半启动状态。
    bool EnsureConfigFile();

    // 写入 appSettings（保留注释与其它节点），供 GUI 保存设置使用
    bool SetSetting(const std::string& key, const std::string& value);

    // 键是否存在（对应 C# AppSettings.Settings[key] != null）
    bool HasSetting(const std::string& key);

    // 供内部与测试使用：剥离 XML 注释
    std::string StripXmlComments(const std::string& xml);
}
}

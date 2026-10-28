#pragma once

// 目标系统：Windows 7 SP1 / Server 2008 R2 及以上
//
// 必须显式定义 WINVER / _WIN32_WINNT，否则会使用 Windows SDK 的默认值，
// 导致生成的程序调用 Win8+ 才有的 API，在 Win7 上直接无法启动。
#define WINVER          0x0601
#define _WIN32_WINNT    0x0601
#define _WIN32_IE       0x0800
#define NTDDI_VERSION   0x06010000

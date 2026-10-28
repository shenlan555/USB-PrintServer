#pragma once

#include "targetver.h"

// 注意：不要在此处包含 winsock2.h，也不要定义 _WINSOCKAPI_。
// afxwin.h 会自行引入 WinSock2.h（并连带 iphlpapi.h / ws2ipdef.h），
// 若提前定义 _WINSOCKAPI_ 或引入旧版 winsock.h，MFC 会直接报错。
#define _AFX_ALL_WARNINGS

#include <afxwin.h>         // MFC 核心与标准组件（含 WinSock2、IP Helper）
#include <afxext.h>         // MFC 扩展
#include <afxdisp.h>        // MFC 自动化类

#include <afxdtctl.h>       // MFC 对 Internet Explorer 4 公共控件的支持
#ifndef _AFX_NO_AFXCMN_SUPPORT
#include <afxcmn.h>         // MFC 对 Windows 公共控件的支持
#endif

#include <afxdialogex.h>    // CDialogEx
#include <afxdlgs.h>        // CFileDialog / CColorDialog 等通用对话框
#include <afxcontrolbars.h> // MFC 支持功能区与控件条

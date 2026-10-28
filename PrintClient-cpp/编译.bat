@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
set w=80 & set h=30
powershell -command "$host.UI.RawUI.BufferSize = New-Object System.Management.Automation.Host.Size(%w%, 9999); $host.UI.RawUI.WindowSize = New-Object System.Management.Automation.Host.Size(%w%, %h%)" 2>nul
title 网络打印客户端(原生C++)编译 By:浪里个浪 https://www.itlog.vip
echo.
echo =========================================
echo   网络打印客户端 原生 C++ 编译工具
echo   MFC 静态链接 / 目标 Win7 SP1+ / x64
echo   https://www.itlog.vip
echo =========================================
echo.
echo 正在关闭旧进程...
taskkill /f /im PrintClient.exe >nul 2>&1
echo.

:: 定位 Visual Studio（Build Tools 或 IDE）
:: 注意：v142 与 v143 的组件 ID 不同（本机只装了 14.29 = v142），
::       若用 -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 会误判为"未安装"，
::       因此先取实例路径，再自行校验 MSBuild 是否存在，最后兜底扫描安装目录。
set "VSPATH="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath 2^>nul`) do set "VSPATH=%%i"
)
if not "!VSPATH!"=="" (
    if not exist "!VSPATH!\MSBuild\Current\Bin\MSBuild.exe" (
        if exist "!VSPATH!\MSBuild\15.0\Bin\MSBuild.exe" (
            set "VSPATH=!VSPATH!"
        ) else (
            set "VSPATH="
        )
    )
)
if "!VSPATH!"=="" (
    for %%r in ("%ProgramFiles(x86)%\Microsoft Visual Studio" "%ProgramFiles%\Microsoft Visual Studio") do (
        if exist "%%~r" (
            for /f "delims=" %%i in ('dir /b /ad "%%~r" 2^>nul') do (
                if "!VSPATH!"=="" if exist "%%~r\%%i\MSBuild\Current\Bin\MSBuild.exe" set "VSPATH=%%~r\%%i"
            )
        )
    )
)
if "!VSPATH!"=="" (
    echo [错误] 未找到 Visual Studio / Build Tools，请安装"使用 C++ 的桌面开发"工作负载
    pause
    exit /b 1
)
echo 使用工具链: !VSPATH!
for /f "delims=" %%t in ('dir /b /ad "!VSPATH!\VC\Tools\MSVC" 2^>nul') do echo   MSVC 工具集: %%t
echo.

:: 定位 MSBuild
:: 注意：新版 Build Tools 可能不安装 vcvars64.bat，但 MSBuild 编译 vcxproj
::       会通过 C++ 目标自行定位工具集，无需预先初始化环境变量。
set "MSBUILD=!VSPATH!\MSBuild\Current\Bin\MSBuild.exe"
if not exist "!MSBUILD!" set "MSBUILD=!VSPATH!\MSBuild\15.0\Bin\MSBuild.exe"
if not exist "!MSBUILD!" (
    echo [错误] 未找到 MSBuild.exe: !MSBUILD!
    pause
    exit /b 1
)

:: 编译 Release x64
"%MSBUILD%" PrintClient.sln /nologo /m /p:Configuration=Release /p:Platform=x64
if errorlevel 1 goto :FAIL

echo.
echo ====================================
echo   编译成功！
echo ====================================
echo.
set "OUTDIR=bin\x64\Release"
echo 程序位置: %OUTDIR%\PrintClient.exe

:: 整理发布目录（与 C# 版一致：bin\PrintClient-v<版本>\）
for /f "usebackq tokens=*" %%v in (`powershell -NoProfile -Command "(Get-Item '%OUTDIR%\PrintClient.exe').VersionInfo.FileVersion"`) do set "VER=%%v"
if not "%VER%"=="" (
    set "RELDIR=bin\PrintClient-v%VER%"
    if not exist "!RELDIR!" mkdir "!RELDIR!"
    copy /y "%OUTDIR%\PrintClient.exe" "!RELDIR!\" >nul
    echo 发布目录: !RELDIR!
)
echo.
explorer %OUTDIR%
goto :END

:FAIL
echo.
echo ====================================
echo   编译失败，请检查错误信息！
echo ====================================

:END
pause

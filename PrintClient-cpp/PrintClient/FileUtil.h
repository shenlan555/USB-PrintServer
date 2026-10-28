#pragma once

// 文件/字符串公共工具
//
// 对应 C# 里散落的 Path / Directory / File / string 辅助逻辑。

#include <string>
#include <vector>

namespace ps
{

// ---- 路径 ----

// 对应 Path.Combine（两点之间补一个反斜杠，不重复补）
std::wstring PathCombine(const std::wstring& left, const std::wstring& right);

// 对应 Path.GetTempPath()
std::wstring GetTempDirectory();

// 对应 Path.GetFileName
std::wstring GetFileName(const std::wstring& path);

// 确保目录存在（对应 Directory.CreateDirectory）
bool EnsureDirectory(const std::wstring& path);

// 递归创建目录（对应 Directory.CreateDirectory 的深层路径）
bool CreateDirectoryTree(const std::wstring& path);

// 递归删除目录（对应 Directory.Delete(path, true)）
bool RemoveDirectoryTree(const std::wstring& path);

// 对应 Directory.Exists / File.Exists
bool DirectoryExists(const std::wstring& path);
bool FileExists(const std::wstring& path);

// 对应 File.Copy(source, dest, true)
bool CopyFileOverwrite(const std::wstring& source, const std::wstring& dest);

bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& bytes);
bool WriteFileBytes(const std::wstring& path, const std::vector<unsigned char>& bytes);
bool WriteFileTextUtf8(const std::wstring& path, const std::string& utf8Text);
bool WriteFileTextUtf8Bom(const std::wstring& path, const std::string& utf8Text);

// 文件大小（对应 new FileInfo(path).Length）；失败返回 -1
long long GetFileSize(const std::wstring& path);

// 递归枚举目录下的全部文件（对应 Directory.GetFiles(dir, pattern, AllDirectories)）
// pattern 例如 L"*.inf" 或 L"*.*"
void EnumerateFilesRecursive(const std::wstring& directory, const std::wstring& pattern,
                             std::vector<std::wstring>& files);

// ---- 字符串 ----

// 对应 Path.GetInvalidFileNameChars() → Replace(c, '_')
std::string SanitizeFileName(const std::string& name);
std::wstring SanitizeFileNameW(const std::wstring& name);

// 对应 MainForm.FormatSize（"{0:0.##} {1}"）
std::string FormatSize(long long bytes);

// 去掉首尾空白（ASCII）
std::string TrimAscii(const std::string& text);
std::wstring TrimWide(const std::wstring& text);

bool EqualsIgnoreCaseAscii(const std::string& text, const char* value);
bool StartsWithIgnoreCaseAscii(const std::string& text, const char* prefix);

// 按行拆分（\r\n、\r、\n 均视为分隔符，且丢弃结尾的空行）
std::vector<std::string> SplitLines(const std::string& text);

// 系统 ANSI 字节流 → UTF-8（对应 .NET RedirectStandardOutput 的默认解码）
std::string AnsiToUtf8(const std::vector<char>& bytes);

} // namespace ps

#ifndef CPPJIEBA_UNICODE_FILE_HPP
#define CPPJIEBA_UNICODE_FILE_HPP

#include <fstream>
#include <limits>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace cppjieba {

#ifdef _WIN32
inline bool Utf8ToWidePath(const std::string& path, std::wstring& widePath) {
  if (path.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  if (path.empty()) {
    widePath.clear();
    return true;
  }

  const int pathSize = static_cast<int>(path.size());
  const int wideSize = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      path.data(),
      pathSize,
      NULL,
      0);
  if (wideSize <= 0) {
    return false;
  }

  widePath.resize(static_cast<size_t>(wideSize));
  return MultiByteToWideChar(
             CP_UTF8,
             MB_ERR_INVALID_CHARS,
             path.data(),
             pathSize,
             &widePath[0],
             wideSize) == wideSize;
}
#endif

inline void OpenInputFile(std::ifstream& ifs, const std::string& path) {
#if defined(_MSC_VER) || (defined(__MINGW32__) && defined(__GNUC__) && __GNUC__ >= 9)
  // MSVC STL 提供 open(const wchar_t*)；MinGW GCC 9+ 的 libstdc++ 可将
  // const wchar_t* 经 std::filesystem::path 隐式转换打开。
  // 两者均支持任意 Unicode 路径。
  std::wstring widePath;
  if (Utf8ToWidePath(path, widePath)) {
    ifs.open(widePath.c_str());
    return;
  }
  ifs.setstate(std::ios::failbit);
  return;
#elif defined(_WIN32)
  // MinGW GCC 8 (Qt 5.15 MinGW 8.1 工具链)：libstdc++ 无 wchar_t 重载，且其
  // std::filesystem 在 Windows 上实现不完整不可用。退化为 ANSI 码页窄字符路径：
  // 中文 Windows（GBK）下中文路径可正常打开；纯 ASCII 路径不受影响。
  std::wstring widePath;
  if (Utf8ToWidePath(path, widePath)) {
    const int srcSize = static_cast<int>(widePath.size());
    const int narrowSize = WideCharToMultiByte(CP_ACP, 0, widePath.data(), srcSize,
                                               NULL, 0, NULL, NULL);
    if (narrowSize > 0) {
      std::string narrowPath(static_cast<size_t>(narrowSize), '\0');
      if (WideCharToMultiByte(CP_ACP, 0, widePath.data(), srcSize,
                              &narrowPath[0], narrowSize, NULL, NULL) == narrowSize) {
        ifs.open(narrowPath.c_str());
        return;
      }
    }
  }
  ifs.setstate(std::ios::failbit);
  return;
#else
  ifs.open(path.c_str());
#endif
}

} // namespace cppjieba

#endif

#pragma once

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace jdelphiast {

inline std::string pathToUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
  const auto& wide = path.native();
  if (wide.empty()) return {};
  const auto size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                        nullptr, 0, nullptr, nullptr);
  if (size <= 0) return "<unrepresentable-path>";
  std::string result(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          result.data(), size, nullptr, nullptr) <= 0)
    return "<unrepresentable-path>";
  return result;
#else
  const auto value = path.u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
#endif
}

inline std::string genericPathToUtf8(const std::filesystem::path& path) {
  auto result = pathToUtf8(path);
  for (auto& c : result) if (c == '\\') c = '/';
  return result;
}

inline std::filesystem::path pathFromSourceBytes(std::string_view value) {
#ifdef _WIN32
  auto convert = [&](UINT codePage, DWORD flags) -> std::filesystem::path {
    const auto size = MultiByteToWideChar(codePage, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(codePage, flags, value.data(), static_cast<int>(value.size()),
                            wide.data(), size) <= 0) return {};
    return std::filesystem::path(std::move(wide));
  };
  auto result = convert(CP_UTF8, MB_ERR_INVALID_CHARS);
  if (!result.empty()) return result;
  result = convert(CP_ACP, 0);
  return result.empty() ? std::filesystem::path(L"<unrepresentable-path>") : result;
#else
  return std::filesystem::path(value);
#endif
}

}  // namespace jdelphiast

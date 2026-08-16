#ifndef NRAW_SHA256_H
#define NRAW_SHA256_H

#include <cstddef>
#include <string>

namespace nraw {

// 计算 data[0,len) 的 SHA-256 摘要，返回 64 位小写十六进制字符串（FIPS 180-4）。
std::string sha256Hex(const void* data, size_t len);

// 计算文件的 SHA-256 摘要；打开/读取失败返回空串。
std::string sha256File(const std::string& path);

} // namespace nraw

#endif

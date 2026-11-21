/// @file png16.hpp
/// @brief PNG 编码：16-bit 灰度（高度图）与 8-bit RGB（预览/底图）
#ifndef IMAGING_PNG16_HPP
#define IMAGING_PNG16_HPP

#include <cstdint>
#include <filesystem>
#include <vector>

namespace imaging{
/// @brief 16-bit 灰度 PNG 编码（w×h，值 0..65535）
/// @details 16-bit 通道按 PNG 规范大端写入。返回完整 PNG 字节流；失败抛 std::runtime_error。
/// @param pix 16-bit 灰度值（行主序）
/// @param w 宽（像素）
/// @param h 高（像素）
/// @return 完整 PNG 字节流
std::vector<uint8_t> encodeGray16(const std::vector<uint16_t>& pix, int w, int h);

/// @brief 16-bit 灰度 PNG 写盘
/// @param path 输出路径（自动创建父目录）
/// @param pix 16-bit 灰度值（行主序）
/// @param w 宽（像素）
/// @param h 高（像素）
/// @return 成功返回 true
bool writeGray16(const std::filesystem::path& path, const std::vector<uint16_t>& pix, int w, int h);

/// @brief 8-bit RGB PNG 编码（w×h，行主序 3 字节/像素）
/// @param rgb RGB 字节流（行主序，3 字节/像素）
/// @param w 宽（像素）
/// @param h 高（像素）
/// @return 完整 PNG 字节流
std::vector<uint8_t> encodeRgb8(const std::vector<uint8_t>& rgb, int w, int h);
} // namespace imaging
#endif // IMAGING_PNG16_HPP

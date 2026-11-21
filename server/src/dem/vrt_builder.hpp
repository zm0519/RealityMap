/// @file vrt_builder.hpp
/// @brief DEM VRT 构建：递归扫描 GeoTIFF，校验（4326 + Int16）后镶嵌成一张逻辑大图
#ifndef DEM_VRT_BUILDER_HPP
#define DEM_VRT_BUILDER_HPP

#include <filesystem>
#include <string>
#include <vector>

namespace dem {

/// @brief 单个 DEM 文件的体检报告
struct FileReport
{
    std::string path;           ///< 文件路径
    bool ok{false};             ///< 通过校验并纳入镶嵌
    std::string reason;         ///< 未纳入原因（ok=false 时填）
    int width{0}, height{0};    ///< 像素尺寸
    double resDeg{0.0};         ///< 像元大小（度，取 |gt[1]|）
    int epsg{0};                ///< 坐标系授权码（读不到为 0）
    bool hasNodata{false};      ///< 是否有 NODATA 声明
    double nodata{0.0};         ///< NODATA 值（有声明时）
};
/// @brief VRT 构建结果
struct BuildResult
{
    std::filesystem::path vrtPath;  ///< 生成的 VRT（失败时为空）
    std::vector<FileReport> files;  ///< 每个输入文件的体检报告
    int includedCount{0};           ///< 纳入镶嵌的文件数
    int skippedCount{0};            ///< 跳过（未纳入）的文件数
};
/// @brief 构建 DEM VRT（虚拟栅格），把多幅 GeoTIFF 声明成一张逻辑大图
/// @param demRoot DEM 数据根目录（递归扫描 *.tif）
/// @param vrtOut 输出 VRT 路径
/// @return 构建结果（含各文件体检报告）
BuildResult buildDemVrt(const std::filesystem::path& demRoot,
                        const std::filesystem::path& vrtOut);
} // dem

#endif // DEM_VRT_BUILDER_HPP

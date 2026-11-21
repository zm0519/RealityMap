/// @file dem_sources.hpp
/// @brief DEM 数据源：打开 4326 VRT/GeoTIFF，按窗口/单点读高程（GDAL，线程安全）
#ifndef DEM_SOURCES_HPP
#define DEM_SOURCES_HPP

#include "geo_math.hpp"

#include <cmath>
#include <filesystem>
#include <mutex>
#include <vector>

#if defined(__has_include) && __has_include(<gdal.h>)
#include <gdal.h>
#else
#include <gdal/gdal.h>
#endif

namespace dem {
/// @brief GDEM v3 空洞哨兵值（文件未声明，按数据集知识显式处理为 NaN）
constexpr float GDEM_NODATA = -9999.0f;

/// @brief 读出的高程网格（行主序，首行=最北）
struct ElevationGrid
{
    int width{0}, height{0};                    ///< 像素宽/高
    geo::Bbox bbox{};                           ///< 实际读取的地理范围（度）
    std::vector<float> values;                  ///< 高程值（行主序，NaN=无数据）
    float minValid{0}, maxValid{0};             ///< 有效高程范围（米）
    long validCount{0};                         ///< 非 NaN 像素数
    /// @brief 判断 (r,c) 是否有有效数据
    bool has(long r, long c) const { return !std::isnan(values[r * width + c]); }
    /// @brief 取 (r,c) 处高程
    float at(long r, long c) const { return values[r * width + c]; }
};
/// @brief DEM 数据源（打开一个 4326 VRT/GeoTIFF，供窗口/单点读取）
class DemSource{
public:
    /// @brief 打开 VRT（或任意 4326 DEM 数据集）；失败抛 std::runtime_error
    /// @param vrtPath VRT/GeoTIFF 路径
    explicit DemSource(const std::filesystem::path& vrtPath);
    ~DemSource();
    DemSource(const DemSource&) = delete;               ///< GDAL 句柄不可复制
    DemSource & operator=(const DemSource&) = delete;

    /// @brief 读窗口：bbox 精确对齐（浮点 RasterIO + 双线性），输出 outW×outH
    /// @details 超出覆盖/空洞 → NaN；GDEM_NODATA → NaN；返回有效极值
    /// @param b 地理范围（度）
    /// @param outW 输出宽（像素）
    /// @param outH 输出高（像素）
    /// @return 读出网格（含有效极值/计数）
    ElevationGrid readWindow(const geo::Bbox& b, int outW, int outH) const;
    /// @brief 数据集地理覆盖范围
    /// @return [w,s,e,n]（度）
    geo::Bbox coverage() const;
    /// @brief 单点高程（WGS84 度）：1 像素 RasterIO；无覆盖/哨兵 → NaN
    /// @param lng 经度（度）
    /// @param lat 纬度（度）
    /// @return 高程（米），无数据返回 NaN
    float elevationAt(double lng, double lat) const;
private:
    mutable std::mutex mtx_;      ///< 保护 GDAL 句柄（GDAL 非线程安全）
    GDALDatasetH ds_{nullptr};    ///< GDAL 数据集句柄
    double gt_[6]{0};             ///< 地理变换参数（仿射）
    int rasterX_{0}, rasterY_{0}; ///< 栅格尺寸
};
} // namespace dem
#endif // DEM_SOURCES_HPP

/// @file resample.hpp
/// @brief 旋转仿射重采样：源网格 → 目标网格（像素 → 经纬度 2D 仿射 + 双线性/双三次插值）
#ifndef RESAMPLE_HPP
#define RESAMPLE_HPP

#include <vector>

namespace core{
/// @brief 源高程网格：地理范围 + Float32 值（行主序，首行=最北；NaN=无数据）
struct Grid
{
    int w{0}, h{0};                                   ///< 像素宽/高
    double wDeg{0}, sDeg{0}, eDeg{0}, nDeg{0};        ///< 覆盖范围（度，w/s/e/n）
    std::vector<float> v;                             ///< 高程值（行主序）
    /// @brief 取 (r,c) 处高程（不做越界检查）
    float at(int r, int c) const { return v[(std::size_t)r * w + c]; }
};
/// @brief 目标网格几何 = 2D 仿射（像素 → 经纬度）
struct TargetAffine
{
    int w{0}, h{0};                                   ///< 目标像素宽/高
    double lon0{0}, dLonDcol{0}, dLonDrow{0};         ///< 经度仿射（首像素 + 列/行步进）
    double lat0{0}, dLatDcol{0}, dLatDrow{0};         ///< 纬度仿射（首像素 + 列/行步进）
};
/// @brief 插值算法
enum class Interp { Bilinear, Bicubic };

/// @brief 逐目标像素采样，返回 w×h Float32（NaN 原样传递）
/// @details 边界策略：border-clamp（越界钳到边缘像素——DEM 窗口留有余量，这是最坏情况安全网）
/// @param src 源网格
/// @param dst 目标仿射（像素 → 经纬度）
/// @param interp 插值算法
/// @return 采样后的 w×h 高程值
std::vector<float> resample(const Grid& src, const TargetAffine& dst, Interp interp);
} // namespace core

#endif // RESAMPLE_HPP

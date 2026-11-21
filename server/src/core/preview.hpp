/// @file preview.hpp
/// @brief 预览渲染：高程网格 → RGB（分层设色 + Horn 山体阴影）
#ifndef CORE_PREVIEW_HPP
#define CORE_PREVIEW_HPP

#include <cstdint>
#include <vector>

namespace core {

/// @brief 预览渲染样式
struct PreviewStyle {
    double minElev = 0.0;       ///< 色带最低高程（米，自动时由调用方填实际值）
    double maxElev = 500.0;     ///< 色带最高高程（米）
    bool hillshade = true;      ///< 是否叠加 Horn 山体阴影
};

/// @brief 高程网格 → RGB 预览图（行主序 3 字节/像素）
/// @details 陆地：绿→棕→白的分层设色；可选 Horn 山体阴影（315°/45° 光源）。NaN 按最低色处理。
/// @param elev 高程网格（米，行主序）
/// @param w 宽（像素）
/// @param h 高（像素）
/// @param st 渲染样式
/// @return w×h×3 的 RGB 字节流
std::vector<uint8_t> renderPreview(const std::vector<float> &elev, int w, int h,
                                   const PreviewStyle &st);

} // namespace core
#endif // CORE_PREVIEW_HPP

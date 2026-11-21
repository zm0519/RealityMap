/// @file preview.cpp
/// @brief 预览渲染实现：绿→棕→白分层设色 + Horn 山体阴影
#include "core/preview.hpp"

#include <algorithm>
#include <cmath>

namespace core {

/// @brief 分层设色：t∈[0,1] → RGB（绿→棕→白，色标经验值与 gdaldem color-relief 常用方案同族）
/// @param t 归一化高程（0=最低，1=最高）
/// @param out 输出的 RGB 三元组
static void landColor(double t, uint8_t out[3]) {
    t = std::clamp(t, 0.0, 1.0);
    /// 三段插值：0.0 绿(60,140,60) 0.5 棕(150,130,80) 1.0 白(240,240,240)
    double r, g, b;
    if (t < 0.5) {
        const double k = t / 0.5;
        r = 60 + (150 - 60) * k;  g = 140 + (130 - 140) * k;  b = 60 + (80 - 60) * k;
    } else {
        const double k = (t - 0.5) / 0.5;
        r = 150 + (240 - 150) * k;  g = 130 + (240 - 130) * k;  b = 80 + (240 - 80) * k;
    }
    out[0] = (uint8_t)r; out[1] = (uint8_t)g; out[2] = (uint8_t)b;
}

std::vector<uint8_t> renderPreview(const std::vector<float> &elev, int w, int h,
                                   const PreviewStyle &st) {
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    const double elevSpan = st.maxElev - st.minElev;
    const auto e = [&](int y, int x) {
        return elev[(size_t)std::clamp(y, 0, h - 1) * w + std::clamp(x, 0, w - 1)];
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float v = e(y, x);
            uint8_t c[3];
            const double t = std::isnan(v) ? 0.0
                : (elevSpan > 0 ? (v - st.minElev) / elevSpan : 0.0);
            landColor(t, c);

            /// Horn 山体阴影（gdaldem 同款 3×3 算子，光源 315°/45°）
            double shade = 1.0;
            if (st.hillshade) {
                constexpr double cell = 30.0;   // DEM 尺度近似（预览用途）
                const double dzdx = ((e(y - 1, x + 1) + 2 * e(y, x + 1) + e(y + 1, x + 1))
                                     - (e(y - 1, x - 1) + 2 * e(y, x - 1) + e(y + 1, x - 1)))
                                    / (8.0 * cell);
                const double dzdy = ((e(y + 1, x - 1) + 2 * e(y + 1, x) + e(y + 1, x + 1))
                                     - (e(y - 1, x - 1) + 2 * e(y - 1, x) + e(y - 1, x + 1)))
                                    / (8.0 * cell);
                const double slope = std::atan(std::hypot(dzdx, dzdy) * cell);   // 弧度
                /// 315° 方位角（西北）、45° 高度角
                const double aspect = std::atan2(dzdy, -dzdx);
                const double zen = std::cos(1.5708 - slope)
                    * std::sin(0.7854) * std::cos(aspect - 4.7124)
                    + std::sin(1.5708 - slope) * std::cos(0.7854);
                shade = 0.6 + 0.5 * std::clamp(zen, 0.0, 1.0);
            }
            uint8_t *d = &rgb[(size_t)y * w * 3 + (size_t)x * 3];
            d[0] = (uint8_t)std::clamp(c[0] * shade, 0.0, 255.0);
            d[1] = (uint8_t)std::clamp(c[1] * shade, 0.0, 255.0);
            d[2] = (uint8_t)std::clamp(c[2] * shade, 0.0, 255.0);
        }
    }
    return rgb;
}

} // namespace core

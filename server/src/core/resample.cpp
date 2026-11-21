/// @file resample.cpp
/// @brief 重采样实现：双线性/双三次插值 + border-clamp 边界
#include "resample.hpp"

#include <algorithm>
#include <cmath>

namespace core{
/// @brief 单点双线性：fx/fy 为源网格浮点像素坐标
/// @param g 源网格
/// @param fx 源 X 像素坐标（浮点）
/// @param fy 源 Y 像素坐标（浮点）
/// @return 插值高程（米）
static float bilinear(const Grid& g, double fx, double fy){
    fx = std::clamp(fx, 0.0, (double)g.w - 1);
    fy = std::clamp(fy, 0.0, (double)g.h - 1);
    const int x0 = (int)fx, y0 = (int)fy;
    const int x1 = std::min(x0 + 1, g.w - 1), y1 = std::min(y0 + 1, g.h - 1);
    const double tx = fx - x0, ty = fy - y0;
    const float a = g.at(y0, x0), b = g.at(y0, x1);
    const float c = g.at(y1, x0), d = g.at(y1, x1);
    return (float)((1 - ty) * ((1 - tx) * a + tx * b) + ty * ((1 - tx) * c + tx * d));
}
/// @brief Mitchell-Netravali 权函数（B=C=1/3）
/// @param t 距采样点距离（像素）
/// @return Mitchell 权重
static double mitchellW(double t){
    constexpr double B = 1.0 / 3.0, C = 1.0 / 3.0;
    t = std::fabs(t);
    const double t2 = t * t, t3 = t2 * t;
    if(t <= 1.0) return ((12 - 9 * B - 6 * C) * t3 + (-18 + 12 * B + 6 * C) * t2 + (6 - 2 * B)) / 6.0;
    if(t <= 2.0) return ((-B - 6 * C) * t3 + (6 * B + 30 * C) * t2 + (-12 * B - 48 * C) * t + (8 * B + 24 * C)) / 6.0;
    return 0.0;
}
/// @brief 单点双三次：4×4 邻域加权和（border-clamp）
/// @param g 源网格
/// @param fx 源 X 像素坐标（浮点）
/// @param fy 源 Y 像素坐标（浮点）
/// @return 插值高程（米）
static float bicubic(const Grid& g, double fx, double fy){
    fx = std::clamp(fx, 0.0, (double)g.w - 1);
    fy = std::clamp(fy, 0.0, (double)g.h - 1);
    const int x0 = (int)std::floor(fx) - 1, y0 = (int)std::floor(fy) - 1;
    double sum = 0;
    for(int j=0; j<4; ++j){
        const int y = std::clamp(y0+j, 0, g.h - 1);
        const double wy = mitchellW(fy - (y0 + j));
        for(int i=0; i<4; ++i){
            const int x = std::clamp(x0+i, 0, g.w-1);
            sum += wy * mitchellW(fx - (x0 + i)) * g.at(y, x);
        }
    }
    return (float)sum;
}
std::vector<float> resample(const Grid& src, const TargetAffine& dst, Interp interp){
    std::vector<float> out((std::size_t)dst.w * dst.h);
    const double pxPerLon = (src.w - 1) / (src.eDeg - src.wDeg);
    const double pxPerLat = (src.h - 1) / (src.nDeg - src.sDeg);
    for(int r=0; r<dst.h; ++r){
        for(int c=0; c<dst.w; ++c){
            const double lon = dst.lon0 + dst.dLonDcol * c + dst.dLonDrow * r;
            const double lat = dst.lat0 + dst.dLatDcol * c + dst.dLatDrow * r;
            const double fx = (lon - src.wDeg) * pxPerLon;
            const double fy = (src.nDeg - lat) * pxPerLat;
            out[(std::size_t)r * dst.w + c] = interp == Interp::Bilinear ? bilinear(src, fx, fy) : bicubic(src, fx, fy);
        }
    }
    return out;
}
} // namespace core
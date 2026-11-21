/// @file heightmap_gen.cpp
/// @brief 高度图生成实现：旋转重采样、16-bit 灰度映射、worldmap 羽化嵌入
#include "heightmap_gen.hpp"

#include <algorithm>
#include <cmath>

namespace core{
/// @brief 目标像素(c,r) → 经纬度：以中心为原点的局部平面（米），旋转后映射到经纬度偏移
///        ENU 线性化：每米对应的度数在中心纬度取值（GeographicLib 精确比例，17.28km 内误差 < 0.1m）
/// @param p 生成参数（含 outRes/correction/sizeKm/angleDeg/lon/lat）
/// @param degPerMeterLon 中心纬度处经度每米度数
/// @param degPerMeterLat 中心纬度处纬度每米度数
/// @return 目标仿射（像素 → 经纬度）
static TargetAffine buildAffine(const GenParams& p, double degPerMeterLon, double degPerMeterLat){
    const double a = geo::deg2rad(p.angleDeg);
    const double cosA = std::cos(a), sinA = std::sin(a);
    /// terraining 同款步进语义：步长 = 尺寸 / (分辨率 - correction)
    ///   cs1: 1081px / correction=1 → 1080 步（2^n+1 边缘采样）
    ///   cs2: 4096px / correction=0 → 4096 步（2^n 步进）
    const double nStep = p.outRes - p.correction;
    const double step = p.sizeKm * 1000.0 / nStep;
    const double x0 = -(nStep / 2.0) * step, y0 = -(nStep / 2.0) * step;  // 首像素原点（左上）
    /// 旋转（顺时针角）：平面偏移 (px,py) → (px·cosA + py·sinA, -px·sinA + py·cosA)
    const double rx0 = x0 * cosA + y0 * sinA;
    const double ry0 = -x0 * sinA + y0 * cosA;
    const double rdx = step * cosA, rdy = -step * sinA;             // 列方向（东）旋转后
    const double cdx = step * sinA, cdy = step * cosA;              // 行方向（南）旋转后
    return {
        p.outRes, p.outRes,
        p.lon + rx0 * degPerMeterLon, rdx * degPerMeterLon, cdx * degPerMeterLon,
        p.lat + ry0 * degPerMeterLat, rdy * degPerMeterLat, cdy * degPerMeterLat,
    };
}
GenResult generateHeightmap(const Grid &src, const GenParams &p, double elevationScaleM)
{
    /// 1) ENU 比例：用测地线在中心点沿东/北各走 100km 精确标定（避免级数近似）
    double latN, lonE, dummy;
    geo::wgs84().Direct(p.lat, p.lon, 0, 100000.0, latN, dummy);
    geo::wgs84().Direct(p.lat, p.lon, 90, 100000.0, dummy, lonE);
    const double degPerMeterLat = (latN - p.lat) / 100000.0;
    const double degPerMeterLon = (lonE - p.lon) / 100000.0;

    /// 2) 旋转仿射重采样
    const auto affine = buildAffine(p, degPerMeterLon, degPerMeterLat);
    auto elev = resample(src, affine, p.interp);

    /// 3) 高程映射（含 % 缩放）
    GenResult r;
    r.res = p.outRes;
    r.elevationScaleM = elevationScaleM / (p.heightScalePct / 100.0);  // pct=垂直放大：越大山越高
    r.elevM = elev;
    r.pix16.resize((std::size_t)p.outRes * p.outRes);
    float mn = std::nanf(""), mx = std::nanf("");
    for(std::size_t i=0; i<elev.size(); ++i){
        const float e = elev[i];
        if(std::isnan(e)) { r.pix16[i] = 0; continue; }  // 水/无数据 → 0（海面）
        if(std::isnan(mn) || e < mn) mn = e;
        if(std::isnan(mx) || e > mx) mx = e;
        const double rel = (e - p.baseLevel) /r.elevationScaleM;
        if(rel >= 1.0) { r.pix16[i] = 65535; ++r.clippedCount; }
        else if(rel <= 0.0) r.pix16[i] = 0;
        else r.pix16[i] = (uint16_t)std::lround(rel * 65535.0);
    }
    r.minElev = mn; r.maxElev = mx;
    return r;
}

GenResult mapElevations(std::vector<float> elev, double baseLevel,
                        double elevationScaleM, double heightScalePct) {
    GenParams p;                 // 复用映射规则
    p.baseLevel = baseLevel;
    p.heightScalePct = heightScalePct;
    p.outRes = (int)std::sqrt((double)elev.size());
    /// 与 generateHeightmap 第 3 步完全同规则的映射
    GenResult r;
    r.res = p.outRes;
    r.elevationScaleM = elevationScaleM / (heightScalePct / 100.0);
    r.elevM = std::move(elev);
    r.pix16.resize(r.elevM.size());
    float mn = std::nanf(""), mx = std::nanf("");
    for (std::size_t i = 0; i < r.elevM.size(); ++i) {
        const float e = r.elevM[i];
        if (std::isnan(e)) { r.pix16[i] = 0; continue; }
        if (std::isnan(mn) || e < mn) mn = e;
        if (std::isnan(mx) || e > mx) mx = e;
        const double rel = (e - baseLevel) / r.elevationScaleM;
        if (rel >= 1.0) { r.pix16[i] = 65535; ++r.clippedCount; }
        else if (rel <= 0.0) r.pix16[i] = 0;
        else r.pix16[i] = (uint16_t)std::lround(rel * 65535.0);
    }
    r.minElev = mn; r.maxElev = mx;
    return r;
}

void embedHeightmapInWorld(std::vector<float> &worldElev, int wres,
                           const std::vector<float> &hElev, int hres, int featherPx) {
    /// 中心块 = wres/4（worldmap 4× 范围时，heightmap 恰占中心 1/4 边长）
    const int block = wres / 4;
    const int x0 = (wres - block) / 2, y0 = (wres - block) / 2;
    const int ratio = hres / block;                     // 4：1 盒滤波
    for (int by = 0; by < block; ++by) {
        for (int bx = 0; bx < block; ++bx) {
            /// 4×4 源块均值（NaN 视为 0 海面，与映射语义一致）
            double sum = 0;
            for (int j = 0; j < ratio; ++j)
                for (int i = 0; i < ratio; ++i) {
                    const float v = hElev[(std::size_t)(by * ratio + j) * hres + (bx * ratio + i)];
                    sum += std::isnan(v) ? 0.0f : v;
                }
            const double down = sum / (ratio * ratio);
            /// 边缘羽化系数：距块边像素数 → 0..1
            const double d = std::min({by + 1.0, block - by * 1.0, bx + 1.0, block - bx * 1.0});
            const double alpha = std::clamp(d / featherPx, 0.0, 1.0);
            float &w = worldElev[(std::size_t)(y0 + by) * wres + (x0 + bx)];
            w = (float)(alpha * down + (1.0 - alpha) * w);
        }
    }
}

void embedHeightmapInWorldOffset(std::vector<float> &worldElev, int wres,
                                 const std::vector<float> &hElev, int hres,
                                 int featherPx, int dxPx, int dyPx) {
    const int block = wres / 4;
    const int baseX = (wres - block) / 2 + dxPx;
    const int baseY = (wres - block) / 2 + dyPx;
    const int ratio = hres / block;
    for (int by = 0; by < block; ++by)
        for (int bx = 0; bx < block; ++bx) {
            const int wx = baseX + bx, wy = baseY + by;
            if (wx < 0 || wx >= wres || wy < 0 || wy >= wres) continue;
            double sum = 0;
            for (int j = 0; j < ratio; ++j)
                for (int i = 0; i < ratio; ++i) {
                    const float v = hElev[(std::size_t)(by * ratio + j) * hres + (bx * ratio + i)];
                    sum += std::isnan(v) ? 0.0f : v;
                }
            const double down = sum / (ratio * ratio);
            const double d = std::min({by + 1.0, block - by * 1.0, bx + 1.0, block - bx * 1.0});
            const double alpha = std::clamp(d / featherPx, 0.0, 1.0);
            float &w = worldElev[(std::size_t)wy * wres + wx];
            w = (float)(alpha * down + (1.0 - alpha) * w);
        }
}

} // namespace core


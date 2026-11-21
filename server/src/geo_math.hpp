/// @file geo_math.hpp
/// @brief 地理/瓦片数学：WGS84 测地线、Web 墨卡托瓦片坐标、覆盖范围计算
/// @details 精确测地一律走 GeographicLib（Karney WGS84，与 PROJ/PostGIS 同源）；
///          瓦片数学遵循 OSM 标准 XYZ 语义（含经度环绕、纬度 ±85.0511° 钳制、z 范围防护）。
#ifndef GEO_MATH_HPP
#define GEO_MATH_HPP

#include <cmath>
#include <algorithm>
#include <cassert>

#include <GeographicLib/Geodesic.hpp>

namespace geo{

constexpr double PI = 3.14159265358979323846;
/// @brief Web Mercator 纬度极限：超出此纬度墨卡托投影无定义（tanφ → ∞）
constexpr double MAX_MERCATOR_LAT = 85.051128779806604;

/// @brief 度转弧度
/// @param d 角度（度）
/// @return 弧度
inline constexpr double deg2rad(double d) { return d*PI/180.0; }
/// @brief 弧度转度
/// @param r 弧度
/// @return 角度（度）
inline constexpr double rad2deg(double r) { return r*180.0/PI; }
/// @brief 经度规范化到 [-180, 180)
/// @param lon 经度（度）
/// @return 归一化后经度（度）
inline double normalizeLon(double lon){
    double x = std::fmod(lon + 180.0, 360.0);
    if(x < 0) x += 360.0;
    return x - 180.0;
}
/// @brief 纬度钳制到墨卡托有效范围
/// @param lat 纬度（度）
/// @return 钳制后纬度（度，∈ [-MAX_MERCATOR_LAT, MAX_MERCATOR_LAT]）
inline double clampLat(double lat){
    return std::clamp(lat, -MAX_MERCATOR_LAT, MAX_MERCATOR_LAT);
}
/// @brief z 级瓦片总数（浮点 2^z；契约 z∈[0,30]）
/// @param z 缩放级别
/// @return 2^z（浮点）
inline double tileScale(int z){
    assert(z >= 0 && z <= 30 && "zoom 超出契约范围");
    return std::ldexp(1.0, z);
}
/// @brief 经度 → 瓦片坐标 X（返回浮点，覆盖计算由调用方取整）
/// @param lon 经度（度）
/// @param z 缩放级别
/// @return 全局瓦片 X 坐标（浮点）
inline double lonToTileX(double lon, int z){
    return (normalizeLon(lon) + 180.0) / 360.0 * tileScale(z);
}
/// @brief 纬度 → 瓦片坐标 Y（返回浮点，覆盖计算由调用方取整）
/// @param lat 纬度（度）
/// @param z 缩放级别
/// @return 全局瓦片 Y 坐标（浮点）
inline double latToTileY(double lat, int z){
    const double s = std::asinh(std::tan(deg2rad(clampLat(lat))));
    return (1.0 - s / PI) / 2.0 * tileScale(z);
}
/// @brief 瓦片坐标 X → 经度（返回瓦片西边缘经度）
/// @param x 全局瓦片 X 坐标（浮点）
/// @param z 缩放级别
/// @return 经度（度）
inline double tileXToLon(double x, int z){
    return x / tileScale(z) * 360.0 - 180.0;
}
/// @brief 瓦片坐标 Y → 纬度（返回瓦片北边缘纬度）
/// @param y 全局瓦片 Y 坐标（浮点）
/// @param z 缩放级别
/// @return 纬度（度）
inline double tileYToLat(double y, int z){
    const double r = PI * (1.0 - 2.0 * y / tileScale(z));
    return rad2deg(std::atan(std::sinh(r)));
}

/// @brief 地理包围盒（w=西经、s=南纬、e=东经、n=北纬，度）
struct Bbox { double w, s, e, n; };
/// @brief 单张瓦片的地理范围
/// @param x 瓦片 X 坐标
/// @param y 瓦片 Y 坐标
/// @param z 缩放级别
/// @return 该瓦片的 [w,s,e,n] 范围（度）
inline Bbox tileBbox(int x, int y, int z){
    return { tileXToLon(x, z), tileYToLat(y + 1, z), tileXToLon(x + 1, z), tileYToLat(y, z) };
}
/// @brief 覆盖某范围的瓦片区间
struct TileRange { int z, x0, x1, y0, y1; };
/// @brief 覆盖 bbox 的瓦片范围（floor 语义：半格也算覆盖）
/// @param b 地理包围盒
/// @param z 缩放级别
/// @return 覆盖的瓦片 X/Y 闭区间
inline TileRange tilesCovering(const Bbox& b, int z){
    return {
        z,
        static_cast<int>(std::floor(lonToTileX(b.w, z))),
        static_cast<int>(std::floor(lonToTileX(b.e, z))),
        static_cast<int>(std::floor(latToTileY(b.n, z))),
        static_cast<int>(std::floor(latToTileY(b.s, z))),
    };
}
/// @brief WGS84 测地线求解器（进程内单例）
/// @return GeographicLib::Geodesic（Karney WGS84）
inline const GeographicLib::Geodesic& wgs84(){
    static const GeographicLib::Geodesic g(
        GeographicLib::Constants::WGS84_a(), GeographicLib::Constants::WGS84_f()
    );
    return g;
}
/// @brief 选区（含旋转角）的地理外接框：用测地线沿四方位各走半个外接边长，取 min/max
/// @param lon 选区中心经度（度）
/// @param lat 选区中心纬度（度）
/// @param sizeKm 选区边长（km）
/// @param angleDeg 旋转角（度，顺时针）
/// @return 覆盖旋转正方形的 [w,s,e,n]（度）
inline Bbox coveringBbox(double lon, double lat, double sizeKm, double angleDeg = 0.0){
    const double a = deg2rad(angleDeg);
    const double coverKm = sizeKm * (std::fabs(std::cos(a)) + std::fabs(std::sin(a)));
    const double halfM = coverKm * 500.0;

    Bbox b{lon, lat, lon, lat};
    double lat2 = 0, lon2 = 0;
    for(const int az : {0, 90, 180, 270}){
        wgs84().Direct(lat, lon, az, halfM, lat2, lon2);
        b.s = std::min(b.s, lat2);
        b.n = std::max(b.n, lat2);
        b.w = std::min(b.w, lon2);
        b.e = std::max(b.e, lon2);
    }
    return b;
}
/// @brief 纬度每度米数（球面级数近似，仅用于交叉验证/粗略显示；精确计算走 wgs84()）
/// @param lat 纬度（度）
/// @return 米/度（北向）
inline double metersPerDegLat_series(double lat) {
    const double p = deg2rad(lat);
    return 111132.92 - 559.82 * std::cos(2 * p) + 1.175 * std::cos(4 * p) - 0.0023 * std::cos(6 * p);
}
/// @brief 经度每度米数（球面级数近似，仅用于交叉验证/粗略显示；精确计算走 wgs84()）
/// @param lat 纬度（度）
/// @return 米/度（东向）
inline double metersPerDegLon_series(double lat) {
    const double p = deg2rad(lat);
    return 111412.84 * std::cos(p) - 93.5 * std::cos(3 * p) + 0.118 * std::cos(5 * p);
}
} // namespace geo
#endif //GEO_MATH_HPP

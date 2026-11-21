/// @file heightmap_gen.hpp
/// @brief 高度图生成：DEM 重采样 → 16-bit 灰度映射；CS2 worldmap 羽化嵌入
#ifndef HEIGHTMAP_GEN_HPP
#define HEIGHTMAP_GEN_HPP

#include "resample.hpp"
#include "geo_math.hpp"

namespace core{
/// @brief 高度图生成参数
struct GenParams
{
    double lon{0}, lat{0};              ///< 选区中心（度）
    double sizeKm{17.280};              ///< 选区边长（km）
    double angleDeg{0};                 ///< 旋转角（度，顺时针）
    double baseLevel{0};                ///< 米：PNG 值 0 对应的高程（通常=海平面或区域最低点）
    double heightScalePct{100.0};       ///< 高程缩放（100% = 1:1）
    int outRes{1081};                   ///< 目标分辨率（像素）
    int correction{1};                  ///< 像素步进修正（来自 FormatSpec：cs1=1, cs2=0）
    Interp interp{Interp::Bicubic};     ///< 重采样插值算法
};
/// @brief 高度图生成结果
struct GenResult
{
    int res{0};                                          ///< 实际分辨率
    std::vector<float> elevM;                            ///< 采样后高程网格（米，NaN=水/无数据）——预览/统计用
    std::vector<uint16_t> pix16;                         ///< 映射后的 16-bit 高度图
    float minElev{0}, maxElev{0};                        ///< 有效高程范围（米）
    long clippedCount{0};                                ///< 超出满量程被钳顶的像素数（>0 时前端应提示）
    double elevationScaleM{1023.984375};                 ///< 实际生效的满量程（含 % 缩放）
};
/// @brief 从源网格生成高度图（DEM 层注入源；核心逻辑可测）
/// @param src 源高程网格
/// @param p 生成参数
/// @param elevationScaleM 格式基准满量程（米）
/// @return 生成结果（含 16-bit 映射）
GenResult generateHeightmap(const Grid &src, const GenParams &p, double elevationScaleM);

/// @brief 高程网格 → 16-bit 映射（worldmap 融合后二次映射用；与 generateHeightmap 同规则）
/// @param elev 高程网格（米）
/// @param baseLevel PNG 值 0 对应的高程
/// @param elevationScaleM 格式基准满量程（米）
/// @param heightScalePct 高程缩放（%）
/// @return 映射后的 16-bit 结果
GenResult mapElevations(std::vector<float> elev, double baseLevel,
                        double elevationScaleM, double heightScalePct);

/// @brief 把 heightmap 高程网格（hres²）以 4:1 盒滤波降采样嵌入 worldmap 高程网格（wres²）的中心块
/// @details 边缘 featherPx 线性羽化——对齐 terraining 的 blendMapsWithFeather 语义：
///          保证两图重叠区像素级一致，游戏 LOD 无缝。worldmap 与 heightmap 的真实范围比须为 4:1。
/// @param worldElev worldmap 高程网格（就地修改）
/// @param wres worldmap 边长（像素）
/// @param hElev heightmap 高程网格
/// @param hres heightmap 边长（像素）
/// @param featherPx 边缘羽化像素数
void embedHeightmapInWorld(std::vector<float> &worldElev, int wres,
                           const std::vector<float> &hElev, int hres, int featherPx);

/// @brief embedHeightmapInWorld 的带偏移版本：heightmap 块中心偏移 (dxPx, dyPx)
/// @param worldElev worldmap 高程网格（就地修改）
/// @param wres worldmap 边长（像素）
/// @param hElev heightmap 高程网格
/// @param hres heightmap 边长（像素）
/// @param featherPx 边缘羽化像素数
/// @param dxPx 块中心 X 偏移（像素）
/// @param dyPx 块中心 Y 偏移（像素）
void embedHeightmapInWorldOffset(std::vector<float> &worldElev, int wres,
                                 const std::vector<float> &hElev, int hres,
                                 int featherPx, int dxPx, int dyPx);
} // namespace core
#endif // HEIGHTMAP_GEN_HPP

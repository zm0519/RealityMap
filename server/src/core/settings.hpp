/// @file settings.hpp
/// @brief 目标格式预设表：CS1/CS2/Unity/UE 的尺寸、分辨率、满量程等参数
#ifndef SETTINGS_HPP
#define SETTINGS_HPP

#include <array>
#include <string>

namespace core{
/// @brief 单个目标格式的规格
struct FormatSpec
{
    std::string key;        ///< 格式键："cs1" / "cs2" / "unity" / "ue"
    std::string name;       ///< 显示名
    double defaultSizeKm;   ///< 默认选区边长（km）；0 = 用户指定（Unity/UE）
    int defaultRes;         ///< 默认输出分辨率（像素）
    double elevationScaleM; ///< 16-bit 满量程对应高程范围（米）
    bool dualImage;         ///< 是否双图（CS2: heightmap + worldmap）
    int correction;         ///< 像素步进修正：CS1=1（2^n+1 边缘采样），CS2=0（2^n 步进）
    double worldFactor;     ///< worldmap 覆盖倍率（CS2=4 → 229.376km 外部世界），0=无
};
/// @brief 按 key 查找格式规格
/// @param key 格式键（"cs1"/"cs2"/"unity"/"ue"）
/// @return 找到返回指针，否则 nullptr
inline const FormatSpec* findFormat(const std::string& key){
    static const std::array<FormatSpec, 4> table{{
        {"cs1",   "Cities: Skylines 1", 17.280, 1081, 1023.984375, false, 1, 0.0},
        {"cs2",   "Cities: Skylines 2", 57.344, 4096, 4096.0,      true,  0, 4.0},
        {"unity", "Unity",              0,      513,  512.0,       false, 1, 0.0},
        {"ue",    "Unreal Engine",      0,      505,  512.0,       false, 1, 0.0},
    }};
    for(const auto& f : table) if(f.key == key) return &f;
    return nullptr;
}
} // namespace core
#endif // SETTINGS_HPP

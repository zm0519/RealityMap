/// @file stitch.hpp
/// @brief 底图拼接：从本地 XYZ 瓦片库拼出与选区同范围的地图图像（Image Overlay 参考底图）
#ifndef IMAGING_STITCH_HPP
#define IMAGING_STITCH_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace imaging {

/// @brief 底图拼接参数
struct StitchParams {
    double lng = 0, lat = 0;             ///< 选区中心（度，与 heightmap 一致）
    double sizeKm = 57.344;              ///< 选区边长（km，与 heightmap 一致）
    int zoom = 15;                       ///< 瓦片层级
    std::vector<std::string> layers;     ///< 按序叠加的层，如 {"vec","cva"} / {"img","cva"}
    int outRes = 4096;                   ///< 输出分辨率（像素）
    std::filesystem::path tileRoot;      ///< data_root（瓦片库根目录）
    bool useTileCache = false;           ///< true=读 tile_cache/{layer}（天地图在线模式）；否则读 vec_tiles/cva/img_tiles
};
/// @brief 底图拼接结果
struct StitchResult {
    std::vector<uint8_t> rgb;            ///< 行主序 RGB（透明处为白）
    int res = 0;                         ///< 输出分辨率
    long missingTiles = 0;               ///< 缺失瓦片数（跳过留白）
};

/// @brief 就近采样合成选区底图；任一层任一瓦片缺失即留白计数。GDAL 解码（RGBA→RGB over 白底）。
/// @param p 拼接参数
/// @return 拼接结果（RGB + 缺失瓦片数）
StitchResult stitchMap(const StitchParams &p);

} // namespace imaging
#endif // IMAGING_STITCH_HPP

/// @file buildings.hpp
/// @brief OSM 建筑提取：PBF → 建筑轮廓 GeoJSON（两遍扫描）
#ifndef OSM_BUILDINGS_HPP
#define OSM_BUILDINGS_HPP

#include <filesystem>
#include <functional>
#include <string>

namespace osm {

/// @brief 从 OSM PBF 提取 bbox 内建筑轮廓 → GeoJSON FeatureCollection
/// @details properties: {name, height(米), levels, building}。
///          height 优先级：height 标签 > building:levels×3m > est_height > 默认 10m。
///          两遍扫描：①收集 bbox 内节点 ②收集引用这些节点的 building way。
///          边界处理：任一节点落在 bbox 内即收录（跨界建筑的 bbox 外节点被裁剪）。
/// @param pbf OSM PBF 文件路径
/// @param w 西经（度）
/// @param s 南纬（度）
/// @param e 东经（度）
/// @param n 北纬（度）
/// @param maxBuildings 最大建筑数（超出截断）
/// @param prog 进度回调（百分比, 阶段描述），可为空
/// @return GeoJSON FeatureCollection 文本
std::string buildingsGeoJson(const std::filesystem::path &pbf,
                             double w, double s, double e, double n,
                             size_t maxBuildings = 300000,
                             const std::function<void(int, const std::string &)> &prog = nullptr);

} // namespace osm
#endif // OSM_BUILDINGS_HPP

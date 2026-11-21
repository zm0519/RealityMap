/// @file roads.hpp
/// @brief OSM 路网提取：PBF → 标准 OSM XML（三遍扫描，含跨界外延节点）
#ifndef OSM_ROADS_HPP
#define OSM_ROADS_HPP

#include <filesystem>
#include <functional>
#include <string>

namespace osm {

/// @brief 从 OSM PBF 提取 bbox 内道路网 → OSM XML（Overpass /api/map 兼容格式）
/// @details 收录 highway=* 的 way（含其引用的节点），外加 railway=rail/tram/light_rail。
///          三遍扫描：①收集 bbox 内节点 ②收集道路 way 及其引用节点（含 bbox 外端点）③补拉 bbox 外节点坐标。
///          输出标准 OSM XML（<osm><node/><way>…</way></osm>），Network Importer 直接吃。
/// @param pbf OSM PBF 文件路径
/// @param w 西经（度）
/// @param s 南纬（度）
/// @param e 东经（度）
/// @param n 北纬（度）
/// @param maxWays 最大 way 数（超出截断）
/// @param prog 进度回调（百分比, 阶段描述），可为空
/// @return OSM XML 文本
std::string roadsOsmXml(const std::filesystem::path &pbf,
                        double w, double s, double e, double n,
                        size_t maxWays = 100000,
                        const std::function<void(int, const std::string &)> &prog = nullptr);

} // namespace osm
#endif // OSM_ROADS_HPP

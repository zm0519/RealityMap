/// @file buildings.cpp
/// @brief OSM 建筑提取实现：两遍扫描（节点 + building way）
#ifndef NOMINMAX
#define NOMINMAX               // libosmium 在 Windows 引入 windows.h，防 min/max 宏污染
#endif
#include "osm/buildings.hpp"

#include <osmium/handler.hpp>
#include <osmium/io/any_input.hpp>
#include <osmium/io/reader.hpp>
#include <osmium/visitor.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace osm {

namespace {

struct Bbox { double w, s, e, n; };

/// @brief 第一遍扫描：收集 bbox 内的节点（id → 坐标）
struct NodeCollector : public osmium::handler::Handler {
    const Bbox &bb;
    std::unordered_map<osmium::object_id_type, std::pair<double, double>> nodes;
    explicit NodeCollector(const Bbox &b) : bb(b) {}
    void node(const osmium::Node &n) {
        const auto loc = n.location();
        if (!loc) return;
        const double lon = loc.lon(), lat = loc.lat();
        if (lon >= bb.w && lon <= bb.e && lat >= bb.s && lat <= bb.n)
            nodes.emplace(n.id(), std::make_pair(lon, lat));
    }
};

/// @brief 第二遍扫描：收集 building way
struct WayCollector : public osmium::handler::Handler {
    const NodeCollector &nc;
    size_t cap;
    size_t count = 0;
    std::string out;                                    // GeoJSON 累积
    std::string overflowName;

    explicit WayCollector(const NodeCollector &c, size_t m) : nc(c), cap(m) {}

    static double parseHeight(const osmium::TagList &tags) {
        if (const char *h = tags.get_value_by_key("height"))
            return std::atof(h);
        if (const char *l = tags.get_value_by_key("building:levels"))
            return std::atof(l) * 3.0;
        if (const char *l = tags.get_value_by_key("est_height"))
            return std::atof(l);
        return 10.0;                                    // 无属性默认（普通多层）
    }

    void way(const osmium::Way &way) {
        if (count >= cap) return;
        const char *b = way.tags().get_value_by_key("building");
        if (!b) return;                                 // 非建筑
        /// 至少一个节点在 bbox 内
        bool inside = false;
        for (const auto &ref : way.nodes())
            if (nc.nodes.count(ref.ref())) { inside = true; break; }
        if (!inside) return;

        /// 几何（bbox 外节点裁剪掉，保证 GeoJSON 合法）
        std::string coords;
        size_t used = 0;
        for (const auto &ref : way.nodes()) {
            const auto it = nc.nodes.find(ref.ref());
            if (it == nc.nodes.end()) continue;
            char buf[96];
            std::snprintf(buf, sizeof buf, "[%.7f,%.7f],", it->second.first, it->second.second);
            coords += buf;
            ++used;
        }
        if (used < 4) return;                           // 建筑至少 4 个点（含闭合）
        coords.pop_back();                              // 去尾逗号

        const char *name = way.tags().get_value_by_key("name");
        const double h = parseHeight(way.tags());
        const char *levels = way.tags().get_value_by_key("building:levels");

        char head[512];
        std::snprintf(head, sizeof head,
            "{\"type\":\"Feature\",\"properties\":{\"name\":\"%s\",\"height\":%.1f,\"levels\":%s,\"building\":\"%s\"},"
            "\"geometry\":{\"type\":\"Polygon\",\"coordinates\":[[",
            name ? name : "", h, (levels ? levels : "null"), b);
        out += head;
        out += coords;
        out += "]]}},\n";
        ++count;
    }
};

} // namespace

std::string buildingsGeoJson(const std::filesystem::path &pbf,
                             double w, double s, double e, double n,
                             size_t maxBuildings,
                             const std::function<void(int, const std::string &)> &prog) {
    if (!std::filesystem::exists(pbf))
        throw std::runtime_error("OSM PBF 不存在: " + pbf.string() +
                                 "（见 docs/osm-download.md，放置到 data/osm/）");

    const Bbox bb{w, s, e, n};

    if (prog) prog(2, "扫描 PBF 第一遍：收集范围内节点");
    {
        osmium::io::Reader reader(pbf.string(), osmium::osm_entity_bits::node);
        NodeCollector nc(bb);
        osmium::apply(reader, nc);
        reader.close();
        if (prog) prog(50, "节点 " + std::to_string(nc.nodes.size()) + " 个，扫描 way");
        /// 第二遍挪出作用域前把结果带回
        WayCollector wc(nc, maxBuildings);
        {
            osmium::io::Reader reader2(pbf.string(), osmium::osm_entity_bits::way);
            osmium::apply(reader2, wc);
            reader2.close();
        }
        if (prog) prog(95, "建筑 " + std::to_string(wc.count) + " 栋，生成 GeoJSON");
        std::string json = "{\"type\":\"FeatureCollection\",\"features\":[\n";
        if (!wc.out.empty()) {
            json += wc.out;
            json.pop_back();                            // 去最后一个换行
            if (!json.empty() && json.back() == ',') json.pop_back();
        }
        json += "\n]}\n";
        return json;
    }
}

} // namespace osm

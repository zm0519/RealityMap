/// @file roads.cpp
/// @brief OSM 路网提取实现：三遍扫描（节点 + way + 外延节点）
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "osm/roads.hpp"

#include <osmium/handler.hpp>
#include <osmium/io/any_input.hpp>
#include <osmium/io/reader.hpp>
#include <osmium/visitor.hpp>

#include <cstdio>
#include <set>
#include <string>
#include <unordered_map>

namespace osm {

namespace {

struct Node { double lon, lat; };

/// @brief 第一遍扫描：收集 bbox 内节点（与 buildings 相同策略）
struct NodePass : public osmium::handler::Handler {
    double w, s, e, n;
    std::unordered_map<osmium::object_id_type, Node> nodes;
    NodePass(double bw, double bs, double be, double bn) : w(bw), s(bs), e(be), n(bn) {}
    void node(const osmium::Node &nd) {
        const auto loc = nd.location();
        if (!loc) return;
        const double lon = loc.lon(), lat = loc.lat();
        if (lon >= w && lon <= e && lat >= s && lat <= n)
            nodes.emplace(nd.id(), Node{lon, lat});
    }
};

/// @brief 第二遍扫描：收集 highway way + 其节点 ID（扩展集——道路端点可能出 bbox）
struct WayPass : public osmium::handler::Handler {
    const std::unordered_map<osmium::object_id_type, Node> &inner;   // bbox 内节点
    size_t cap;
    size_t count = 0;

    /// way 数据（先暂存，等所有 way 收集完后统一输出 XML）
    struct WayData {
        osmium::object_id_type id;
        std::vector<osmium::object_id_type> nodeRefs;
        std::vector<std::pair<std::string, std::string>> tags;
    };
    std::vector<WayData> ways;
    std::set<osmium::object_id_type> neededNodes;   // way 引用的所有节点（含 bbox 外的）

    explicit WayPass(const std::unordered_map<osmium::object_id_type, Node> &in, size_t m)
        : inner(in), cap(m) {}

    static bool isRoad(const osmium::TagList &tags) {
        if (tags.has_key("highway")) return true;
        const char *r = tags.get_value_by_key("railway");
        if (r && (std::string(r) == "rail" || std::string(r) == "tram" || std::string(r) == "light_rail")) return true;
        return false;
    }

    void way(const osmium::Way &way) {
        if (count >= cap) return;
        if (!isRoad(way.tags())) return;
        /// 至少一个节点在 bbox 内（或 bbox 边界穿越）
        bool inside = false;
        for (const auto &ref : way.nodes())
            if (inner.count(ref.ref())) { inside = true; break; }
        if (!inside) return;

        WayData wd;
        wd.id = way.id();
        for (const auto &ref : way.nodes()) {
            wd.nodeRefs.push_back(ref.ref());
            neededNodes.insert(ref.ref());
        }
        for (const auto &t : way.tags())
            wd.tags.push_back({t.key(), t.value()});
        ways.push_back(std::move(wd));
        ++count;
    }
};

/// @brief 第三遍扫描：补拉 bbox 外节点坐标（way 端点延伸出界的部分，跳过已在 bbox 内的节点避免重复）
struct OuterNodePass : public osmium::handler::Handler {
    const std::set<osmium::object_id_type> &needed;
    const std::unordered_map<osmium::object_id_type, Node> &inner;   // bbox 内节点
    std::unordered_map<osmium::object_id_type, Node> outer;
    explicit OuterNodePass(const std::set<osmium::object_id_type> &n,
                           const std::unordered_map<osmium::object_id_type, Node> &in)
        : needed(n), inner(in) {}
    void node(const osmium::Node &nd) {
        if (needed.count(nd.id()) == 0) return;
        if (inner.count(nd.id())) return;   // 已在 bbox 内，跳过（否则 node id 重复）
        const auto loc = nd.location();
        if (loc) outer.emplace(nd.id(), Node{loc.lon(), loc.lat()});
    }
};

std::string xmlEscape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

} // namespace

std::string roadsOsmXml(const std::filesystem::path &pbf,
                        double w, double s, double e, double n,
                        size_t maxWays,
                        const std::function<void(int, const std::string &)> &prog) {
    if (!std::filesystem::exists(pbf))
        throw std::runtime_error("OSM PBF 不存在: " + pbf.string());

    if (prog) prog(2, "扫描 PBF：收集范围内节点");
    NodePass np{w, s, e, n};
    { osmium::io::Reader r(pbf.string(), osmium::osm_entity_bits::node);
      osmium::apply(r, np); r.close(); }
    if (prog) prog(30, "节点 " + std::to_string(np.nodes.size()) + "，扫描道路 way");
    WayPass wp(np.nodes, maxWays);
    { osmium::io::Reader r(pbf.string(), osmium::osm_entity_bits::way);
      osmium::apply(r, wp); r.close(); }
    if (prog) prog(70, "道路 " + std::to_string(wp.count) + " 条，补拉外延节点");
    OuterNodePass onp(wp.neededNodes, np.nodes);
    { osmium::io::Reader r(pbf.string(), osmium::osm_entity_bits::node);
      osmium::apply(r, onp); r.close(); }
    if (prog) prog(90, "生成 OSM XML");

    /// 输出 OSM XML
    std::string xml;
    xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += "<osm version=\"0.6\" generator=\"realitymap-server\">\n";
    for (const auto &[id, nd] : np.nodes) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "  <node id=\"%lld\" lat=\"%.7f\" lon=\"%.7f\"/>\n",
                      (long long)id, nd.lat, nd.lon);
        xml += buf;
    }
    for (const auto &[id, nd] : onp.outer) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "  <node id=\"%lld\" lat=\"%.7f\" lon=\"%.7f\"/>\n",
                      (long long)id, nd.lat, nd.lon);
        xml += buf;
    }
    for (const auto &wd : wp.ways) {
        xml += "  <way id=\"" + std::to_string((long long)wd.id) + "\">\n";
        for (const auto &nr : wd.nodeRefs)
            xml += "    <nd ref=\"" + std::to_string((long long)nr) + "\"/>\n";
        for (const auto &[k, v] : wd.tags)
            xml += "    <tag k=\"" + xmlEscape(k) + "\" v=\"" + xmlEscape(v) + "\"/>\n";
        xml += "  </way>\n";
    }
    xml += "</osm>\n";
    return xml;
}

} // namespace osm

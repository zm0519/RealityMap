/// @file main.cpp
/// @brief realitymap-server 入口：HTTP 服务、各 API 端点与静态前端挂载
/// 职责：读 config.json → 扫描数据目录 → 启动 HTTP（/api/ping）。
#include "gdal_init.hpp"
#ifndef NOMINMAX
#define NOMINMAX           // windows.h 的 min/max 宏会污染 std::clamp/min/max
#endif
#include <curl/curl.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include "dem/vrt_builder.hpp"
#include "dem/dem_sources.hpp"
#include "core/heightmap_gen.hpp"
#include "core/settings.hpp"
#include "imaging/png16.hpp"
#include "imaging/zip.hpp"
#include "imaging/stitch.hpp"
#include "core/preview.hpp"
#include "net/tile_fetch.hpp"
#include "osm/buildings.hpp"
#include "osm/roads.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#if defined(__has_include) && __has_include(<gdal.h>)
#include <gdal.h>            // vcpkg 布局
#else
#include <gdal/gdal.h>       // 系统包布局（Linux 发行版）
#endif

#include "gdal_init.hpp"

#include <filesystem>
#ifdef _WIN32
#include <crtdbg.h>           // 仅 Windows（CRT 断言/错误处理）
#endif
#include <fstream>
#include <iostream>
#include <string>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <cmath>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

json load_config(const std::string &path) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "[warn] 配置文件不存在: " << path << "，使用内置默认值\n";
        return json{
            {"host", "127.0.0.1"},
            {"port", 8300},
            {"data_root", "D:/data"},
            {"tile_layers", json{{"vec", "vec_tiles"}, {"cva", "cva"}, {"img", "img_tiles"}}},
            {"dem_dir", "DEM"},
        };
    }
    return json::parse(f);
}

/// @brief 应用状态：配置 + 可热替换的 DEM 源
struct AppState
{
    json cfg;
    std::filesystem::path dataRoot, demDir, vrtPath, configPath;
    std::mutex srcMtx, cfgMtx;   // srcMtx 保护 src；cfgMtx 保护 cfg（运行期可改设置）
    std::shared_ptr<const dem::DemSource> src;
    dem::BuildResult lastScan;
};
std::shared_ptr<const dem::DemSource> currentSource(AppState& st){
    std::lock_guard<std::mutex> lk(st.srcMtx);
    return st.src;
}
/// @brief 扫描 + 建 VRT + 换源（启动和 rescan 共用）
/// @param st 应用状态（就地更新 lastScan 与 src）
void rebuildDem(AppState& st){
    st.lastScan = dem::buildDemVrt(st.demDir, st.vrtPath);
    auto fresh = std::make_shared<const dem::DemSource>(st.vrtPath.string());
    std::lock_guard<std::mutex> lk(st.srcMtx);
    st.src = std::move(fresh);
}
/// @brief 统一错误处理：handler 一律 throw，这里翻译成 HTTP
using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;
Handler guard(Handler h){
    return [h = std::move(h)](const httplib::Request& req, httplib::Response& res){
        try
        {
            h(req, res);
        }
        catch(const std::exception& e)
        {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
        
    };
}
/// @brief 参数解析 helper：从 JSON body 取 bbox（数组 [w,s,e,n] → geo::Bbox，带校验）
/// @param body 请求 JSON body
/// @return 解析出的 [w,s,e,n] 包围盒
geo::Bbox parseBbox(const json& body){
    if(!body.contains("bbox") || !body.at("bbox").is_array() || body.at("bbox").size() != 4){
        throw std::runtime_error("body.bbox 须为 [w,s,e,n] 四元数组");
    }
    const auto& a = body.at("bbox");
    geo::Bbox b{a[0], a[1], a[2], a[3]};
    if(!(b.w < b.e && b.s < b.n)) throw std::runtime_error("bbox 方向无效 须 w<e,s<n");
    if(b.w < -180 || b.e > 180 || b.s < -90 || b.n > 90){
        throw std::runtime_error("bbox 超出经纬度范围");
    }
    return b;
}
} // namespace

/// libcurl GET（geocode 代理等出网用途）
static std::string httpGet(const std::string &url, int timeoutSec = 15) {
    std::string out;
    if (CURL *c = curl_easy_init()) {
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)timeoutSec);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
            +[](char *p, size_t n, size_t m, void *u) -> size_t {
                static_cast<std::string *>(u)->append(p, n * m); return n * m; });
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
        curl_easy_perform(c);
        curl_easy_cleanup(c);
    }
    return out;
}

/// 底图自动层级：让瓦片地面分辨率 ≈ 输出图像素，并保证瓦片数不超上限
static int autoRoadZoom(double lat, double sizeKm, int outRes, long capTiles) {
    const double cosL = std::cos(lat * geo::PI / 180.0);
    const double target = sizeKm * 1000.0 / outRes;             // m/px
    int z = std::clamp((int)std::lround(std::log2(156543.03392 * cosL / target)), 3, 18);
    while (z > 3) {
        const double mpp = 156543.03392 * cosL / (double)(1 << z);
        const double n = std::ceil(sizeKm * 1000.0 / mpp / 256.0);
        if (n * n <= capTiles) break;
        --z;
    }
    return z;
}

/// ---------- 异步任务系统 ----------
struct GenOutput {
    int status = 200;
    std::string contentType = "application/json";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string error;
};

struct GenJob {
    std::string id;
    std::atomic<int> progress{0};
    std::string phase{"启动中"};
    std::mutex m;                       // 保护 phase/最终产出
    std::string status{"running"};      // running | done | error
    GenOutput out;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::thread th;
};
static std::map<std::string, std::shared_ptr<GenJob>> g_jobs;
static std::mutex g_jobsMtx;

int main(int argc, char **argv) {
    const std::string config_path = argc > 1 ? argv[1] : "server/config.json";
    json cfg = load_config(config_path);
    /// 环境变量优先：REALITYMAP_TIANDITU_KEY 覆盖 config.json 里的 tianditu_key
    /// （生产部署用 systemd Environment= / docker -e 注入，密钥不落盘）
    if (const char *envKey = std::getenv("REALITYMAP_TIANDITU_KEY"); envKey && *envKey)
        cfg["tianditu_key"] = envKey;

    const std::string host = cfg.value("host", "127.0.0.1");
    const int port = cfg.value("port", 8300);
    const fs::path data_root = cfg.value("data_root", "D:/data");
    const fs::path dem_dir = data_root / cfg.value("dem_dir", "DEM");

#ifdef _WIN32
    /// 无人值守服务器：CRT 断言/错误直接终止进程（Debug 弹窗会卡死整个服务且无人看见）
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(_CALL_REPORTFAULT, _CALL_REPORTFAULT);   // abort 生成 WER 报告而非对话框
#endif

    gdalInit();
    AppState st;
    st.cfg = cfg;
    st.configPath = config_path;
    st.dataRoot = data_root;
    st.demDir = dem_dir;
    st.vrtPath = st.demDir / "mosaic.vrt";
    /// 瓦片两种互斥来源：tile_source="local"（离线，读本地预下载）/"tianditu"（在线，需 key）
    const bool tileOnline = st.cfg.value("tile_source", "local") == "tianditu";
    net::TileFetcher::instance().configure(
        tileOnline ? st.cfg.value("tianditu_key", "") : "", st.dataRoot.string(), tileOnline);
    try
    {
        rebuildDem(st);
    }
    catch(const std::exception& e)
    {
        std::cerr << "[warn] DEM 未就绪：" << e.what() << "\n";
    }

    httplib::Server svr;
    svr.set_read_timeout(120, 0);
    svr.set_write_timeout(120, 0);
    svr.set_post_routing_handler([](const httplib::Request &req, httplib::Response &res) {
        /// 瓦片内容不可变 → 浏览器长缓存（当天内重访零请求）；HTML 开发期免缓存
        if (req.path.rfind("/tiles/", 0) == 0 && res.status == 200)
            res.set_header("Cache-Control", "public, max-age=86400");
        else if (res.get_header_value("Content-Type").rfind("text/html", 0) == 0 && res.status == 200)
            res.set_header("Cache-Control", "no-cache");
    });
    /// 瓦片服务：本地直读（local）或按需天地图（tianditu）；两种模式互斥，不回源混合
    svr.Get(R"(/tiles/([A-Za-z]+)/([0-9]+)/([0-9]+)/([0-9]+)\.png)",
        guard([&](const httplib::Request &req, httplib::Response &res) {
            auto &tf = net::TileFetcher::instance();
            const auto path = tf.pathOf(req.matches[1], std::stoi(req.matches[2]),
                                        std::stoi(req.matches[3]), std::stoi(req.matches[4]));
            if (path.empty()) { res.status = 404; return; }
            {   // 瓦片缺失/无效（含旧版 0 字节负缓存残留）→ 在线模式按需回源
                std::error_code ec;
                const bool usable = std::filesystem::exists(path, ec)
                    && std::filesystem::file_size(path, ec) > 0;
                if (!usable && tf.enabled())
                    tf.fetch(req.matches[1], std::stoi(req.matches[2]),
                             std::stoi(req.matches[3]), std::stoi(req.matches[4]));
            }
            if (!net::TileFetcher::usableFile(path)) { res.status = 404; return; }
            std::FILE *f = fopen(path.string().c_str(), "rb");
            if (!f) { res.status = 404; return; }
            std::string buf;
            char tmp[65536]; size_t n;
            while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf.append(tmp, n);
            fclose(f);
            res.set_content(buf, "image/png");
        }));

    /// 设置查询/更新（持久化到 config.json，重启仍生效）
    svr.Get("/api/settings", guard([&](const httplib::Request &, httplib::Response &res) {
        std::lock_guard<std::mutex> lk(st.cfgMtx);
        res.set_content(json{
            {"tile_source", st.cfg.value("tile_source", "local")},
            {"tianditu_key", st.cfg.value("tianditu_key", "")},
            {"geocode_enabled", st.cfg.value("geocode_enabled", true)},
        }.dump(), "application/json");
    }));
    svr.Post("/api/settings", guard([&](const httplib::Request &req, httplib::Response &res) {
        const json body = json::parse(req.body);
        std::string src, key;
        {
            std::lock_guard<std::mutex> lk(st.cfgMtx);
            if (body.contains("tile_source")) {
                src = body["tile_source"].get<std::string>();
                if (src != "local" && src != "tianditu") {
                    res.status = 400;
                    res.set_content(R"({"error":"tile_source 须为 local 或 tianditu"})", "application/json");
                    return;
                }
                st.cfg["tile_source"] = src;
            }
            if (body.contains("tianditu_key")) st.cfg["tianditu_key"] = body["tianditu_key"].get<std::string>();
            if (body.contains("geocode_enabled")) st.cfg["geocode_enabled"] = body["geocode_enabled"].get<bool>();

            src = st.cfg.value("tile_source", "local");
            key = st.cfg.value("tianditu_key", "");

            std::ofstream f(st.configPath);
            if (!f) {
                res.status = 500;
                res.set_content(R"({"error":"写配置失败"})", "application/json");
                return;
            }
            f << st.cfg.dump(2);
        }

        /// 应用运行期变化（tile_source / key）
        net::TileFetcher::instance().configure(
            (src == "tianditu") ? key : "", st.dataRoot.string(), src == "tianditu");
        res.set_content(json{{"ok", true}}.dump(), "application/json");
    }));

    /// 高程网格：POST /api/elevation {bbox:[w,s,e,n], resolution:N}
    svr.Post("/api/elevation", guard([&](const httplib::Request& req, httplib::Response& res){
        auto src = currentSource(st);
        if(!src) { res.status = 503; res.set_content(R"({"error":"DEM 未就绪"})", "application/json"); return; }
        const json body = json::parse(req.body);
        const geo::Bbox b = parseBbox(body);
        int n = body.value("resolution", 512);
        if(n < 1 || n > 4096) throw std::runtime_error("resolution 须在 1-4096");
        auto g = src->readWindow(b, n, n);
        res.set_header("X-Min-Elevation", std::to_string(g.minValid));
        res.set_header("X-Max-Elevation", std::to_string(g.maxValid));
        res.set_header("X-Valid-Count", std::to_string(g.validCount));
        res.set_header("Access-Control-Expose-Headers", "X-Min-Elevation, X-Max-Elevation, X-Valid-Count");
        res.set_content(std::string(reinterpret_cast<const char*>(g.values.data()),
                        g.values.size() * sizeof(float)),
                        "application/ocet-stream");
    }));
    /// 高程探针：POST /api/elevation/stats {bbox} → JSON（网格法，上限 2048²≈源分辨率）
    svr.Post("/api/elevation/stats", guard([&](const httplib::Request& req, httplib::Response& res){
        auto src = currentSource(st);
        if(!src) { res.status = 503; res.set_content(R"({"error":"DEM 未就绪"})", "application/json"); return; }
        const json body = json::parse(req.body);
        const geo::Bbox b = parseBbox(body);
        auto g = src->readWindow(b, 2048, 2048);
        json out{{"validRatio", g.validCount / double(g.width) / g.height}};
        if (g.validCount) { out["min"] = g.minValid; out["max"] = g.maxValid; }
        else              { out["min"] = nullptr;    out["max"] = nullptr; }
        res.set_content(out.dump(), "application/json");
    }));
    /// DEM 信息：GET /api/dem/info
    svr.Get("/api/dem/info", guard([&](const httplib::Request&, httplib::Response& res){
        auto src = currentSource(st);
        json j{{"ready", src != nullptr}, {"includedFiles", st.lastScan.includedCount},
                {"skippedFiles", st.lastScan.skippedCount}};
        if(src) { 
            const auto c = src->coverage();
            j["coverage"] = {c.w, c.s, c.e, c.n};
        }
        res.set_content(j.dump(), "application/json");
        
    }));
    /// 热重建：POST /api/admin/rescan
    svr.Post("/api/admin/rescan", guard([&](const httplib::Request&, httplib::Response& res){
        rebuildDem(st);
        res.set_content(R"({"ok":true})", "application/json");
    }));
    svr.Get("/api/ping", guard([&](const httplib::Request &, httplib::Response &res) {
        auto src = currentSource(st);
        res.set_content(json{{"status", "ok"}, {"dem_ready", src != nullptr}}.dump(),
                        "application/json");
    }));
    auto runHeightmapJob = [&](const json &body,
                               const std::function<void(int, const std::string &)> &prog) -> GenOutput {
        auto src = currentSource(st);
        GenOutput out;
        try {
        if(!src) { out.status = 503; out.body = R"({"error":"DEM 未就绪"})"; out.contentType = "application/json"; return out; }
        const std::string fmtKey = body.value("format", "cs1");
        const auto* fmt = core::findFormat(fmtKey);
        if (!fmt) throw std::runtime_error("未知格式: " + fmtKey + "（可用 cs1/cs2/unity/ue）");

        core::GenParams p;
        p.lon = body.at("lng"); p.lat = body.at("lat");
        p.sizeKm = body.value("sizeKm", fmt->defaultSizeKm);
        p.angleDeg = body.value("angle", 0.0);
        p.baseLevel = body.value("baseLevel", 0.0);
        p.heightScalePct = body.value("heightScalePct", 100.0);   // 语义：垂直放大倍率
        p.outRes = body.value("resolution", fmt->defaultRes);
        p.correction = fmt->correction;
        p.interp = body.value("interpolation", "bicubic") == "bilinear" ? core::Interp::Bilinear : core::Interp::Bicubic;

        /// ① 读源网格（coveringBbox 含角度外接，输出网格数≈原生分辨率）
        const auto bbox = geo::coveringBbox(p.lon, p.lat, p.sizeKm, p.angleDeg);
        const int srcRes = std::clamp((int)std::lround((bbox.e - bbox.w) / 0.000277) + 2, 64, 8192);
        if (prog) prog(8, "读取 DEM");
        const auto grid = src->readWindow(bbox, srcRes, srcRes);
        const core::Grid cg{srcRes, srcRes, bbox.w, bbox.s, bbox.e, bbox.n, grid.values};

        auto gen = core::generateHeightmap(cg, p, fmt->elevationScaleM);

        gen = core::mapElevations(std::move(gen.elevM), p.baseLevel,
                                  fmt->elevationScaleM, p.heightScalePct);
        if (prog) prog(22, "heightmap 完成");

        /// CS2：补生成 worldmap（4 倍范围的外部世界），双图打包 zip
        if (fmt->dualImage && fmt->worldFactor > 0) {
            core::GenParams wp = p;
            wp.sizeKm = p.sizeKm * fmt->worldFactor;      // 229.376km（sizeKm×4，比例恒定）
            const auto wbbox = geo::coveringBbox(p.lon, p.lat, wp.sizeKm, p.angleDeg);
            const int wsrcRes = std::clamp((int)std::lround((wbbox.e - wbbox.w) / 0.000277) + 2, 64, 16384);
            const auto wgrid = src->readWindow(wbbox, wsrcRes, wsrcRes);
            const core::Grid wcg{wsrcRes, wsrcRes, wbbox.w, wbbox.s, wbbox.e, wbbox.n, wgrid.values};
            auto wgen = core::generateHeightmap(wcg, wp, fmt->elevationScaleM);

            /// terraining 同款：把 heightmap 以 4:1 盒滤波羽化嵌入 worldmap 中心块，
            /// 两图重叠区像素级一致 → 游戏内可玩区边缘无接缝
            /// heightmap 居中嵌入世界图（0.375 是"选区内缩比例"非方向偏移——
            /// terraining 的 offset4cs2play 使 heightmap extent = 世界图中央 25%，天然居中）
            core::embedHeightmapInWorld(wgen.elevM, wgen.res, gen.elevM, gen.res, 24);
            wgen = core::mapElevations(std::move(wgen.elevM), p.baseLevel,
                                       fmt->elevationScaleM, p.heightScalePct);
            if (prog) prog(38, "worldmap 生成/融合完成");

            imaging::ZipWriter zip;
            zip.begin();
            const auto hpng = imaging::encodeGray16(gen.pix16, gen.res, gen.res);
            const auto wpng = imaging::encodeGray16(wgen.pix16, wgen.res, wgen.res);
            zip.addMem("heightmap_" + fmtKey + ".png", hpng.data(), hpng.size());
            zip.addMem("worldmap_" + fmtKey + ".png", wpng.data(), wpng.size());

            /// 矢量图/影像图：与高度图同范围，自动层级 + 按需回源
            auto &tf = net::TileFetcher::instance();
            const int rz = std::clamp(body.value("mapZoom", 15), 3, 20);  // 用户当前地图缩放，不搞别的
            long roadTotal = 0, roadDone = 0;
            for (const auto &style : {std::pair<std::string, std::vector<std::string>>{"vec", {"vec", "cva"}},
                                       {"img", {"img", "cva"}}}) {
                try {
                    if (tf.enabled()) {
                        const auto bb = geo::coveringBbox(p.lon, p.lat, p.sizeKm, 0.0);
                        const auto tr = geo::tilesCovering(bb, rz);
                        if (roadTotal == 0)
                            roadTotal = (long)(tr.x1 - tr.x0 + 1) * (tr.y1 - tr.y0 + 1)
                                        * (long)style.second.size() * 2;
                        for (const auto &L : style.second)
                            for (int x = tr.x0; x <= tr.x1; ++x)
                                for (int y = tr.y0; y <= tr.y1; ++y) {
                                    tf.fetch(L, rz, x, y);
                                    ++roadDone;
                                    if (prog && roadTotal > 0 && roadDone % 25 == 0)
                                        prog(38 + (int)(47.0 * roadDone / roadTotal),
                                             "底图回源 " + std::to_string(roadDone) + "/" + std::to_string(roadTotal));
                                }
                    }
                    imaging::StitchParams sp;
                    sp.lng = p.lon; sp.lat = p.lat; sp.sizeKm = p.sizeKm;
                    sp.zoom = rz; sp.outRes = p.outRes; sp.tileRoot = st.dataRoot;
                    sp.layers = style.second;
                    sp.useTileCache = tf.online();   // 在线模式读 tile_cache（与回源目录一致）
                    const auto road = imaging::stitchMap(sp);
                    const auto rpng = imaging::encodeRgb8(road.rgb, road.res, road.res);
                    const std::string zname = (style.first == "vec") ? "vector" : "satellite";
                    zip.addMem(zname + "_z" + std::to_string(rz) + ".png",
                               rpng.data(), rpng.size());
                } catch (const std::exception &e) {
                    std::cerr << "[zip] 底图(" << style.first << ") skip: " << e.what() << std::endl;
                }
            }

            /// 参数档案（复现用）
            const json meta = {
                {"format", fmtKey}, {"lng", p.lon}, {"lat", p.lat},
                {"sizeKm", p.sizeKm}, {"angle", p.angleDeg},
                {"baseLevel", p.baseLevel}, {"heightScalePct", p.heightScalePct},
                {"resolution", p.outRes}, {"interpolation",
                    p.interp == core::Interp::Bilinear ? "bilinear" : "bicubic"},
                {"elevation", {{"min", gen.minElev}, {"max", gen.maxElev},
                                {"clipped", gen.clippedCount}}},
                {"generatedAt", "" },
            };


            const auto mstr = meta.dump(2);
            zip.addMem("settings.json", mstr.data(), mstr.size());
            if (prog) prog(97, "打包 zip");
            const auto zbytes = zip.finish();

            out.headers.push_back({"X-Min-Elevation", std::to_string(gen.minElev)});
            out.headers.push_back({"X-Max-Elevation", std::to_string(gen.maxElev)});
            out.headers.push_back({"X-World-Min-Elevation", std::to_string(wgen.minElev)});
            out.headers.push_back({"X-World-Max-Elevation", std::to_string(wgen.maxElev)});
            out.headers.push_back({"X-Clipped-Pixels", std::to_string(gen.clippedCount + wgen.clippedCount)});
            out.headers.push_back({"Content-Disposition", "attachment; filename=heightmap_" + fmtKey + ".zip"});
            out.body = std::string(reinterpret_cast<const char *>(zbytes.data()), zbytes.size());
            out.contentType = "application/zip";
            return out;
        }

        const std::string exportType = body.value("export", "png");
        if(exportType == "png"){
            const auto png = imaging::encodeGray16(gen.pix16, gen.res, gen.res);
            out.headers.push_back({"X-Min-Elevation", std::to_string(gen.minElev)});
            out.headers.push_back({"X-Max-Elevation", std::to_string(gen.maxElev)});
            out.headers.push_back({"X-Clipped-Pixels", std::to_string(gen.clippedCount)});
            out.headers.push_back({"Content-Disposition", "attachment; filename=heightmap_" + fmtKey + ".png"});
            out.body = std::string(reinterpret_cast<const char *>(png.data()), png.size());
            out.contentType = "image/png";
        }else if(exportType == "raw"){
            /// 16-bit LE RAW（与 PNG 同数据，字节序小端）
            std::string raw;
            raw.resize(gen.pix16.size() * 2);
            for (size_t i = 0; i < gen.pix16.size(); ++i) {
                raw[i * 2] = (char)(gen.pix16[i] & 0xFF);
                raw[i * 2 + 1] = (char)(gen.pix16[i] >> 8);
            }
            out.headers.push_back({"Content-Disposition", "attachment; filename=heightmap_" + fmtKey + ".raw"});
            out.body = std::move(raw); out.contentType = "application/octet-stream";
        }else{
            throw std::runtime_error("export 须为 png 或 raw");
        }
        return out;
        } catch (const std::exception &e) {
            out.status = 500; out.error = e.what();
            out.body = json{{"error", e.what()}}.dump(); out.contentType = "application/json";
            return out;
        }
    };

    /// 同步版（脚本/兼容）：直接执行并返回
    svr.Post("/api/heightmap", guard([&](const httplib::Request &req, httplib::Response &res) {
        auto out = runHeightmapJob(json::parse(req.body), {});
        for (auto &[k, v] : out.headers) res.set_header(k, v);
        res.status = out.status;
        res.set_content(out.body, out.contentType.c_str());
    }));

    /// 异步版（前端）：立即返回 jobId，后台线程执行，可轮询进度
    svr.Post("/api/heightmap-async", guard([&](const httplib::Request &req, httplib::Response &res) {
        {
            std::lock_guard<std::mutex> lk(g_jobsMtx);
            int running = 0;
            for (auto &[_, j] : g_jobs) if (j->status == "running") ++running;
            if (running >= 2) { res.status = 429;
                res.set_content(R"({"error":"已有 2 个生成任务在进行，请稍候"})", "application/json"); return; }
        }
        auto job = std::make_shared<GenJob>();
        job->id = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const json body = json::parse(req.body);
        auto weak = std::weak_ptr<GenJob>(job);
        job->th = std::thread([weak, body, &runHeightmapJob]() {
            if (auto job = weak.lock()) {
                GenOutput out = runHeightmapJob(body, [&job](int p, const std::string &ph) {
                    job->progress = p;
                    std::lock_guard<std::mutex> l(job->m);
                    job->phase = ph;
                });
                std::lock_guard<std::mutex> l(job->m);
                job->out = std::move(out);
                job->status = job->out.error.empty() ? "done" : "error";
            }
        });
        job->th.detach();
        {
            std::lock_guard<std::mutex> lk(g_jobsMtx);
            g_jobs[job->id] = job;                       // 淘汰 15 分钟前的旧任务
            for (auto it = g_jobs.begin(); it != g_jobs.end();) {
                if (it->second->status != "running" &&
                    std::chrono::steady_clock::now() - it->second->t0 > std::chrono::minutes(15))
                        it = g_jobs.erase(it);
                else ++it;
            }
        }
        res.set_content(json{{"jobId", job->id}}.dump(), "application/json");
    }));

    /// 单点高程（前端光标探测）
    svr.Get("/api/elevation/point", guard([&](const httplib::Request &req, httplib::Response &res) {
        auto src = currentSource(st);
        if (!src) { res.status = 503; return; }
        const double lng = std::stod(req.get_param_value("lng"));
        const double lat = std::stod(req.get_param_value("lat"));
        const float e = src->elevationAt(lng, lat);
        const std::string body = "{\"elev\":" + std::string(std::isnan(e) ? "null" : std::to_string(e)) + "}";
        res.set_content(body, "application/json");
    }));

    /// 行政区边界 → 包络选区参数（阿里 DataV GeoAtlas 数据，已本地化到 D:/data/region/）
    svr.Get("/api/region", guard([&](const httplib::Request &req, httplib::Response &res) {
        const std::string q = req.get_param_value("q");
        if (q.empty()) { res.status = 400; return; }
        /// 名录：本地 all.json，进程内缓存
        static std::mutex allMtx;
        static json all = json::array();
        { std::lock_guard<std::mutex> lk(allMtx);
          if (all.empty()) {
            std::error_code ec;
            const auto p_all = st.dataRoot / "region" / "all.json";
            if (!std::filesystem::exists(p_all, ec))
                throw std::runtime_error("region/all.json 不存在（本地下载工具生成或手动放置）");
            std::ifstream f(p_all);
            all = json::parse(f);
          } }
        /// 命名匹配：全等 > 前缀 > 包含；省>市>县 优先
        json cands = json::array();
        for (int pass = 0; pass < 3 && cands.size() < 4; ++pass)
          for (const auto &it : all) {
            if (cands.size() >= 4) break;
            const std::string name = it.value("name", "");
            const bool hit = pass == 0 ? name == q : (pass == 1 ? name.rfind(q, 0) == 0 : name.find(q) != std::string::npos);
            if (!hit) continue;
            bool dup = false;
            for (const auto &c : cands) if (c["name"] == name && c["level"] == it.value("level", "")) dup = true;
            if (!dup) cands.push_back(it);
          }
        if (cands.empty()) { res.set_content("[]", "application/json"); return; }
        /// 逐候选取边界算 bbox（最多 4 个，防慢）
        json out = json::array();
        for (const auto &c : cands) {
            const auto p_bound = st.dataRoot / "region" / "bound"
                                / (std::to_string(c.value("adcode", 0)) + ".json");
            std::error_code ec;
            if (!std::filesystem::exists(p_bound, ec)) continue;   // 未下载/无独立边界
            std::ifstream bf(p_bound);
            std::string braw((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
            if (braw.size() < 100) continue;
            double w = 1e9, s_ = 1e9, e = -1e9, n = -1e9;
            std::function<void(const json &)> walk = [&](const json &node) {
                if (node.is_array() && !node.empty() && node[0].is_number()) {
                    w = std::min(w, node[0].get<double>()); e = std::max(e, node[0].get<double>());
                    s_ = std::min(s_, node[1].get<double>()); n = std::max(n, node[1].get<double>());
                } else if (node.is_array()) for (const auto &x : node) walk(x);
            };
            try { walk(json::parse(braw)["features"][0]["geometry"]["coordinates"]); } catch (...) { continue; }
            if (w > e) continue;
            const double latMid = (s_ + n) / 2;
            const double wKm = (e - w) * 111.32 * std::cos(latMid * geo::PI / 180.0);
            const double hKm = (n - s_) * 110.9;
            out.push_back({{"name", c.value("name", "")}, {"adcode", c.value("adcode", 0)},
                           {"level", c.value("level", "")},
                           {"bbox", {w, s_, e, n}},
                           {"spanKm", std::round(std::max(wKm, hKm) * 1.03)}});
            if (out.size() >= 2) break;   // 最多返回 2 个（市/县同名时给两个选择）
        }
        res.set_content(out.dump(), "application/json");
    }));


    svr.Get(R"(/api/job/([A-Za-z0-9]+))", guard([&](const httplib::Request &req, httplib::Response &res) {
        std::shared_ptr<GenJob> job;
        { std::lock_guard<std::mutex> lk(g_jobsMtx);
          auto it = g_jobs.find(req.matches[1].str());
          if (it == g_jobs.end()) { res.status = 404; return; }
          job = it->second; }
        std::lock_guard<std::mutex> l(job->m);
        json j{{"status", job->status},
               {"progress", job->progress.load()},
               {"phase", job->phase},
               {"elapsedMs", (long)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - job->t0).count()}};
        j["error"] = job->status == "error" ? json(job->out.error) : json(nullptr);
        res.set_content(j.dump(), "application/json");
    }));

    /// 建筑白膜 GeoJSON（异步：china.pbf 全库两遍扫描约 1-3 分钟）
    svr.Post("/api/buildings-async", guard([&](const httplib::Request &req, httplib::Response &res) {
        const json body = json::parse(req.body);
        auto job = std::make_shared<GenJob>();
        job->id = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const std::string pbf = (st.dataRoot / "osm" / "china-latest.osm.pbf").string();
        const double w = body.at("bbox")[0], s_ = body.at("bbox")[1],
                     e = body.at("bbox")[2], nn = body.at("bbox")[3];
        auto weak = std::weak_ptr<GenJob>(job);
        job->th = std::thread([weak, pbf, w, s_, e, nn]() {
            if (auto job = weak.lock()) {
                GenOutput out;
                try {
                    out.body = osm::buildingsGeoJson(pbf, w, s_, e, nn, 300000,
                        [&job](int p, const std::string &ph) {
                            job->progress = p;
                            std::lock_guard<std::mutex> l(job->m); job->phase = ph;
                        });
                    out.contentType = "application/geo+json";
                    out.headers.push_back({"Content-Disposition", "attachment; filename=buildings.geojson"});
                    /// 网格缓存（按 0.05° 量化，供地图层秒出）
                    { const auto cacheDir = std::filesystem::path("D:/data") / "cache" / "buildings";
                      std::error_code ec2;
                      std::filesystem::create_directories(cacheDir, ec2);
                      const double qw = std::floor(w * 20) / 20, qs = std::floor(s_ * 20) / 20;
                      std::ofstream cf(cacheDir / (std::to_string(qw) + "_" + std::to_string(qs) + ".json"));
                      cf << out.body; }
                } catch (const std::exception &ex) {
                    out.status = 500; out.error = ex.what();
                    out.body = json{{"error", ex.what()}}.dump(); out.contentType = "application/json";
                }
                std::lock_guard<std::mutex> l(job->m);
                job->out = std::move(out);
                job->status = job->out.error.empty() ? "done" : "error";
            }
        });
        job->th.detach();
        { std::lock_guard<std::mutex> lk(g_jobsMtx); g_jobs[job->id] = job; }
        res.set_content(json{{"jobId", job->id}}.dump(), "application/json");
    }));

    /// 建筑白膜地图层：按 0.05° 网格缓存，首次生成后秒出
    svr.Get("/api/buildings-cell", guard([&](const httplib::Request &req, httplib::Response &res) {
        const double w = std::stod(req.get_param_value("w"));
        const double s = std::stod(req.get_param_value("s"));
        /// 量化到 0.05° 网格
        const double qw = std::floor(w * 20) / 20, qs = std::floor(s * 20) / 20;
        const auto cacheDir = st.dataRoot / "cache" / "buildings";
        const auto cellFile = cacheDir / (std::to_string(qw) + "_" + std::to_string(qs) + ".json");
        std::error_code ec;
        if (std::filesystem::exists(cellFile, ec)) {
            std::ifstream f(cellFile);
            res.set_content(std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()),
                            "application/geo+json");
            return;
        }
        res.status = 404;   // 未缓存，前端调 buildings-async 生成
    }));

    /// OSM 路网导出（异步，Network Importer mod 用）
    svr.Post("/api/roads-async", guard([&](const httplib::Request &req, httplib::Response &res) {
        const json body = json::parse(req.body);
        auto job = std::make_shared<GenJob>();
        job->id = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const std::string pbf = (st.dataRoot / "osm" / "china-latest.osm.pbf").string();
        const double w = body.at("bbox")[0], s_ = body.at("bbox")[1],
                     e = body.at("bbox")[2], nn = body.at("bbox")[3];
        auto weak = std::weak_ptr<GenJob>(job);
        job->th = std::thread([weak, pbf, w, s_, e, nn]() {
            if (auto job = weak.lock()) {
                GenOutput out;
                try {
                    out.body = osm::roadsOsmXml(pbf, w, s_, e, nn, 100000,
                        [&job](int p, const std::string &ph) {
                            job->progress = p;
                            std::lock_guard<std::mutex> l(job->m); job->phase = ph;
                        });
                    out.contentType = "application/xml";
                    out.headers.push_back({"Content-Disposition", "attachment; filename=roads.osm"});
                } catch (const std::exception &ex) {
                    out.status = 500; out.error = ex.what();
                    out.body = json{{"error", ex.what()}}.dump(); out.contentType = "application/json";
                }
                std::lock_guard<std::mutex> l(job->m);
                job->out = std::move(out);
                job->status = job->out.error.empty() ? "done" : "error";
            }
        });
        job->th.detach();
        { std::lock_guard<std::mutex> lk(g_jobsMtx); g_jobs[job->id] = job; }
        res.set_content(json{{"jobId", job->id}}.dump(), "application/json");
    }));

    svr.Get(R"(/api/job/([A-Za-z0-9]+)/result)", guard([&](const httplib::Request &req, httplib::Response &res) {
        std::shared_ptr<GenJob> job;
        { std::lock_guard<std::mutex> lk(g_jobsMtx);
          auto it = g_jobs.find(req.matches[1].str());
          if (it == g_jobs.end()) { res.status = 404; return; }
          job = it->second; }
        std::lock_guard<std::mutex> l(job->m);
        if (job->status == "running") { res.status = 409;
            res.set_content(R"({"error":"仍在进行"})", "application/json"); return; }
        for (auto &[k, v] : job->out.headers) res.set_header(k, v);
        res.status = job->out.status;
        res.set_content(job->out.body, job->out.contentType.c_str());
    }));
    /// 地名搜索代理（天地图 geocoder 接口；config.tianditu_key 为空时 503）
    svr.Get("/api/geocode", guard([&](const httplib::Request &req, httplib::Response &res){
        std::string key;
        { std::lock_guard<std::mutex> lk(st.cfgMtx); key = st.cfg.value("tianditu_key", ""); }
        if (key.empty()) { res.status = 503;
            res.set_content(R"({"error":"未配置 tianditu_key"})", "application/json"); return; }
        const std::string q = req.get_param_value("q");
        if (q.empty()) { res.status = 400; res.set_content(R"({"error":"缺少 q"})", "application/json"); return; }
        const std::string ds = "{\"keyWord\":\"" + q + "\"}";
        char *esc = curl_easy_escape(nullptr, ds.c_str(), (int)ds.size());
        const std::string url = "https://api.tianditu.gov.cn/geocoder?ds=" + std::string(esc) + "&tk=" + key;
        curl_free(esc);
        const std::string raw = httpGet(url);
        json out = json::array();
        try {
            const auto j = json::parse(raw);
            if (j.value("status", "1") == "0" && j.contains("location")) {
                const auto &loc = j.at("location");
                out.push_back({{"name", j.at("location").value("keyWord", q)},
                               {"lng", std::stod(loc.value("lon", "0"))},
                               {"lat", std::stod(loc.value("lat", "0"))}});
            }
        } catch (...) {}
        res.set_content(out.dump(), "application/json");
    }));

    /// 预览：与 heightmap 同管线的着色渲染（分辨率截断 512，交互用）
    svr.Post("/api/preview", guard([&](const httplib::Request& req, httplib::Response& res){
        auto src = currentSource(st);
        if(!src) { res.status = 503; res.set_content(R"({"error":"DEM 未就绪"})", "application/json"); return; }
        const json body = json::parse(req.body);
        const std::string fmtKey = body.value("format", "cs1");
        const auto* fmt = core::findFormat(fmtKey);
        if (!fmt) throw std::runtime_error("未知格式: " + fmtKey);

        core::GenParams p;
        p.lon = body.at("lng"); p.lat = body.at("lat");
        p.sizeKm = body.value("sizeKm", fmt->defaultSizeKm);
        p.angleDeg = body.value("angle", 0.0);
        p.outRes = std::min(body.value("resolution", 512), 512);
        p.correction = fmt->correction;
        p.interp = core::Interp::Bicubic;

        const auto bbox = geo::coveringBbox(p.lon, p.lat, p.sizeKm, p.angleDeg);
        const int srcRes = std::clamp((int)std::lround((bbox.e - bbox.w) / 0.000277) + 2, 64, 8192);
        const auto grid = src->readWindow(bbox, srcRes, srcRes);
        const core::Grid cg{srcRes, srcRes, bbox.w, bbox.s, bbox.e, bbox.n, grid.values};
        auto gen = core::generateHeightmap(cg, p, fmt->elevationScaleM);

        core::PreviewStyle st;
        float mn = std::nanf(""), mx = std::nanf("");
        for (float v : gen.elevM) {
            if (std::isnan(v)) continue;
            if (std::isnan(mn) || v < mn) mn = v;
            if (std::isnan(mx) || v > mx) mx = v;
        }
        st.minElev = std::isnan(mn) ? 0 : mn;
        st.maxElev = std::isnan(mx) ? 500 : mx;
        const auto rgb = core::renderPreview(gen.elevM, gen.res, gen.res, st);
        const auto png = imaging::encodeRgb8(rgb, gen.res, gen.res);
        res.set_content(std::string(reinterpret_cast<const char*>(png.data()), png.size()), "image/png");
    }));

    /// 矢量/影像底图拼接：与 heightmap 同范围同中心的参考图（Image Overlay 工作流）
    svr.Post("/api/mapimage", guard([&](const httplib::Request& req, httplib::Response& res){
        const json body = json::parse(req.body);
        imaging::StitchParams p;
        p.lng = body.at("lng");  p.lat = body.at("lat");
        p.sizeKm = body.value("sizeKm", 57.344);
        p.zoom = body.value("zoom", 0);
        if (p.zoom <= 0)
            p.zoom = autoRoadZoom(p.lat, p.sizeKm, body.value("outRes", 4096), 20000);
        p.outRes = body.value("outRes", 4096);
        p.tileRoot = st.dataRoot;
        p.useTileCache = net::TileFetcher::instance().online();   // 在线模式读 tile_cache
        if (body.contains("layers"))
            for (const auto &l : body.at("layers")) p.layers.push_back(l.get<std::string>());
        if (p.layers.empty()) p.layers = {"vec", "cva"};

        /// 按需回源：拼接前把选区缺失的瓦片从天地图拉齐（需 key；可关）
        auto &tf = net::TileFetcher::instance();
        if (body.value("fetchMissing", true) && tf.enabled()) {
            const auto bb = geo::coveringBbox(p.lng, p.lat, p.sizeKm, 0.0);
            const auto tr = geo::tilesCovering(bb, p.zoom);
            const long need = (long)(tr.x1 - tr.x0 + 1) * (tr.y1 - tr.y0 + 1) * (long)p.layers.size();
            if (need > 6000)
                throw std::runtime_error("需补拉 " + std::to_string(need)
                    + " 张瓦片（上限 6000）：请缩小范围或降低 zoom");
            long fetched = 0, miss = 0;
            for (const auto &L : p.layers)
                for (int x = tr.x0; x <= tr.x1; ++x)
                    for (int y = tr.y0; y <= tr.y1; ++y) {
                        const auto f = tf.pathOf(L, p.zoom, x, y);
                        if (net::TileFetcher::usableFile(f)) continue;
                        if (tf.fetch(L, p.zoom, x, y)) ++fetched; else ++miss;
                    }
            std::cerr << "[mapimage] 回源补拉 " << fetched << " 张，失败 " << miss << "\n";
        }

        const auto r = imaging::stitchMap(p);
        const auto png = imaging::encodeRgb8(r.rgb, r.res, r.res);
        res.set_header("X-Cover-Meters", std::to_string((long)(p.sizeKm * 1000)));
        res.set_header("X-Missing-Tiles", std::to_string(r.missingTiles));
        res.set_header("X-Zoom", std::to_string(p.zoom));
        res.set_header("Content-Disposition", "attachment; filename=roadmap.png");
        res.set_content(std::string(reinterpret_cast<const char*>(png.data()), png.size()), "image/png");
    }));

    /// 静态前端：挂载 web/public（原生 HTML+JS 单页，无构建步骤）
    /// 注意：相对路径要求服务在项目根目录启动（WorkingDirectory 必须含 web/public）
    if (std::filesystem::exists("web/public"))
        svr.set_mount_point("/", "web/public");

    std::cout << "realitymap-server\n"
              << "  data_root: " << data_root.string() << "\n"
              << "  listening: " << host << ":" << port << "（127.0.0.1 = 仅本机，0.0.0.0 = 所有网卡）\n"
              << "  浏览器访问: http://127.0.0.1:" << port << " 或 http://localhost:" << port << "\n";

    if (!svr.listen(host, port)) {
        std::cerr << "[error] 监听失败: " << host << ":" << port << "\n";
        return 1;
    }


    return 0;
}

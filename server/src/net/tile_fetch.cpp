/// @file tile_fetch.cpp
/// @brief 天地图瓦片按需拉取实现：curl + 并发限流 + 礼貌间隔
#include "net/tile_fetch.hpp"

#include <curl/curl.h>
#include <cstdio>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace net {

TileFetcher &TileFetcher::instance() {
    static TileFetcher f;
    return f;
}

void TileFetcher::configure(const std::string &key, const std::string &dataRoot, bool online) {
    std::lock_guard<std::mutex> lk(mtx_);
    key_ = key;
    dataRoot_ = dataRoot;
    online_ = online;
}

bool TileFetcher::enabled() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return !key_.empty();
}

bool TileFetcher::online() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return online_;
}

/// 层名 → 天地图 T 参数 / 本地目录
static bool layerSpec(const std::string &layer, std::string &T, std::string &dir) {
    if (layer == "vec") { T = "vec_w"; dir = "vec_tiles"; return true; }
    if (layer == "cva") { T = "cva_w"; dir = "cva"; return true; }
    if (layer == "img") { T = "img_w"; dir = "img_tiles"; return true; }
    return false;
}

bool TileFetcher::usableFile(const std::filesystem::path &p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec) && std::filesystem::file_size(p, ec) > 0;
}

std::filesystem::path TileFetcher::pathOf(const std::string &layer, int z, int x, int y) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string T, dir;
    if (!layerSpec(layer, T, dir)) return {};
    /// 天地图在线模式：独立缓存目录，与本地预下载（vec_tiles/cva/img_tiles）完全隔离
    const std::string root = online_ ? ("tile_cache/" + layer) : dir;
    return std::filesystem::path(dataRoot_) / root / std::to_string(z)
           / std::to_string(x) / (std::to_string(y) + ".png");
}

bool TileFetcher::fetch(const std::string &layer, int z, int x, int y) {
    /// 快照 key / 路径（网络 I/O 期间不持锁，切换来源时可能略不一致但无害）
    std::string key;
    std::filesystem::path path;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (key_.empty()) return false;
        std::string T, dir;
        if (!layerSpec(layer, T, dir)) return false;
        const std::string root = online_ ? ("tile_cache/" + layer) : dir;
        path = std::filesystem::path(dataRoot_) / root / std::to_string(z)
               / std::to_string(x) / (std::to_string(y) + ".png");
        key = key_;
    }
    std::string T, dir;
    if (!layerSpec(layer, T, dir)) return false;
    if (usableFile(path)) return true;

    /// 4 路并发 + 每路 60ms 礼让（聚合 ≈ 60张/秒，与 QGIS 默认并发同级，不触限流）
    static std::mutex qmtx;
    static std::condition_variable cv;
    static int inflight = 0;
    static constexpr int MAXCONC = 4;
    {
        std::unique_lock<std::mutex> lk(qmtx);
        cv.wait(lk, [] { return inflight < MAXCONC; });
        ++inflight;
    }
    struct Guard_ {
        ~Guard_() {
            { std::lock_guard<std::mutex> l(qmtx); --inflight; }
            cv.notify_one();
        }
    } conc_guard;
    if (usableFile(path)) return true;   // 双检（并发下另一线程已拉/占位符已被覆盖）

    sub_ = (sub_ + 1) % 8;
    const std::string url = "https://t" + std::to_string(sub_)
        + ".tianditu.gov.cn/DataServer?T=" + T
        + "&x=" + std::to_string(x) + "&y=" + std::to_string(y)
        + "&l=" + std::to_string(z) + "&tk=" + key;

    std::string data;
    if (CURL *c = curl_easy_init()) {
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
            +[](char *p, size_t n, size_t m, void *u) -> size_t {
                static_cast<std::string *>(u)->append(p, n * m); return n * m; });
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &data);
        const CURLcode rc = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(c);
        /// 天地图：矢量/标注瓦片是 PNG，卫星瓦片是 JPEG —— 两种魔数都接受（存 .png 统一寻址）
        const bool pngMagic = data.size() > 8 && (unsigned char)data[0] == 0x89 && data.substr(1, 3) == "PNG";
        const bool jpgMagic = data.size() > 3 && (unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xD8;
        /// 仅拒绝「非图片 / 网络错误 / 非 200」：这些不落盘、下次重试；
        /// 200 的图片（含透明空白瓦片）一律缓存，与 QGIS 行为一致
        if (rc != CURLE_OK || code != 200 || (!pngMagic && !jpgMagic)) {
            std::fprintf(stderr, "[tile_fetch] %s rc=%d http=%ld size=%zu head=%02x%02x%02x%02x\n",
                         url.c_str(), (int)rc, code, data.size(),
                         data.size() > 3 ? (unsigned char)data[0] : 0,
                         data.size() > 3 ? (unsigned char)data[1] : 0,
                         data.size() > 3 ? (unsigned char)data[2] : 0,
                         data.size() > 3 ? (unsigned char)data[3] : 0);
            return false;                    // 无效响应不入库（下次可重试）
        }
    } else return false;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::FILE *f = fopen(path.string().c_str(), "wb");
    if (!f) return false;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));   // 每路礼让，聚合 ~60张/秒
    return true;
}

} // namespace net

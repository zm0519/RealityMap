#ifndef NET_TILE_FETCH_HPP
#define NET_TILE_FETCH_HPP

#include <filesystem>
#include <mutex>
#include <string>

namespace net {

/// 瓦片服务两种互斥来源（由 configure 的 online 决定缓存目录，两者零交集）：
///   online=false（本地模式）：读/写 dataRoot/{vec_tiles|cva|img_tiles}，不联网
///   online=true（天地图模式）：读/写 dataRoot/tile_cache/{vec|cva|img}，按需联网拉取
/// 需在 config 配置 tianditu_key。礼貌策略：互斥锁串行 + 请求间小延迟。
/// 运行期可通过 configure 重新切换（/api/tile-source），内部用互斥锁保证线程安全。
class TileFetcher {
public:
    static TileFetcher &instance();   // 进程级单例（持有锁与轮询状态）

    void configure(const std::string &key, const std::string &dataRoot, bool online);

    bool enabled() const;             // 是否配置了 key（可联网）
    bool online() const;              // 当前是否为天地图在线模式

    /// 拉取一张瓦片到缓存。已存在直接返回 true；失败返回 false。
    /// layer: "vec" | "cva" | "img"（映射到 vec_w/cva_w/img_w 与本地目录）
    bool fetch(const std::string &layer, int z, int x, int y);

    /// 当前模式下的瓦片路径（本地模式=预下载目录，天地图模式=独立缓存目录；不拉取）
    std::filesystem::path pathOf(const std::string &layer, int z, int x, int y) const;

    /// 文件"可用"判定：存在且非空（QGIS 风格——任何 200 响应都缓存，含天地图透明空白瓦片）
    static bool usableFile(const std::filesystem::path &p);

private:
    TileFetcher() = default;
    mutable std::mutex mtx_;          // 保护 key_/dataRoot_/online_（运行期可切换）
    std::string key_;
    std::string dataRoot_;
    bool online_ = false;             // true=天地图在线模式（独立缓存目录 tile_cache）
    int sub_ = 0;                     // t0-t7 轮询
};

} // namespace net
#endif // NET_TILE_FETCH_HPP

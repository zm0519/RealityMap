# RealityMap

真实高度图生成器。  
DEM 数据本地化（自备）；底图瓦片支持本地预下载或天地图在线两种模式（`tile_source` 切换）。

## 功能

- 地图框选区域 → 生成 CS1 / CS2（已测） / Unity / UE 格式 16-bit 高度图（PNG / RAW / ZIP）
- 影像图 / 矢量图导出（瓦片拼接）、OSM 建筑白膜 / 路网导出
- 部署形态：C++ 后端 + 静态前端

## 数据布局（`data_root`，Windows 默认 `D:\data`，Linux 请改成本机路径）

```
D:\data\
├── img_tiles\{z}\{x}\{y}.png      # 卫星影像（标准 XYZ，y 不翻转）
├── vec_tiles\{z}\{x}\{y}.png      # 矢量道路图
├── cva\{z}\{x}\{y}.png            # 地名标注（叠加层）
├── DEM\30\<区域>\ASTGTMV003_*_dem.tif   # ASTER GDEM v3 30m 分幅
├── osm\china-latest.osm.pbf       # OSM 路网
└── region\
    ├── all.json                   # 行政区名录（可选，「行政区」框选用）
    └── bound\{adcode}.json        # 各行政区边界 GeoJSON（可选）
```

瓦片用 QGIS 的 XYZ 下载工具下载（勿勾选 y 翻转）；DEM 从[地理空间数据云](https://www.gscloud.cn)下载。`region/` 是「行政区」框选的可选数据，缺失时该功能返回空、不影响手动框选。

## 底图数据源与格式

底图（地图瓦片）统一使用 **Web 墨卡托（EPSG:3857）XYZ 瓦片**，256×256 像素，层级 z 0–18（矢量/标注到 17，卫星到 18）。三层：

| 层 | 内容 | 编码 |
|---|---|---|
| `vec` | 矢量道路图 | PNG |
| `cva` | 地名标注（叠加） | PNG |
| `img` | 卫星影像 | JPEG（统一存为 `.png`） |

两种互斥来源（`config.json` 的 `tile_source`），两者目录物理隔离、零交集：

- `"local"`（离线）：只读本地预下载瓦片，目录在 `data_root` 下：
  ```
  D:\data\{vec_tiles,cva,img_tiles}\{z}\{x}\{y}.png
  ```
  不联网；瓦片缺失就留空（返回 404）。

- `"tianditu"`（在线）：按需访问**天地图**（tianditu.gov.cn）的 Web 墨卡托瓦片接口，拉到后缓存到独立目录 `tile_cache/`（与上面的本地预下载目录完全分开）：
  ```
  D:\data\tile_cache\{vec,cva,img}\{z}\{x}\{y}.png
  ```
  缓存是持久化的：同层级瓦片拉过一次后，重启服务也会复用，不再重复请求天地图。

密钥通过 `config.json` 的 `tianditu_key` 配置；也可用环境变量 `REALITYMAP_TIANDITU_KEY` 注入（优先于 `config.json`，生产部署推荐）。

## 构建与部署

### Windows（本机开发 / 运行）

前置：VS 2022（含「使用 C++ 的桌面开发」+ Windows SDK）、CMake ≥ 3.25、vcpkg。

```powershell
# 1) vcpkg（首次）
git clone https://github.com/microsoft/vcpkg D:\vcpkg
D:\vcpkg\bootstrap-vcpkg.bat

# 2) 配置 + 构建（首次编译 GDAL 等依赖 30-60 分钟）
$env:VCPKG_ROOT = "D:\vcpkg"   # 指向你的 vcpkg 位置
cmake --preset win-dev
cmake --build build/win-dev --config Release   # 开发调试用 Debug 也行

# 3) 运行（需在项目根目录启动，web/public 按相对路径挂载）
copy server\config.example.json server\config.json   # 改 data_root / tianditu_key / tile_source
.\build\win-dev\server\Release\realitymap-server.exe server\config.json
# 浏览器打开 http://127.0.0.1:8300
```

### Linux（服务器部署）

> 在 WSL Ubuntu-22.04 + gcc 11.4 实测通过。

**系统依赖**：

```bash
sudo apt-get update
sudo apt-get install -y zip unzip ninja-build pkg-config \
    bison flex autoconf automake libtool autoconf-archive

# cmake 需 ≥3.25（CMakePresets.json 用 version:6）
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null
echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ jammy main' | sudo tee /etc/apt/sources.list.d/kitware.list
sudo apt-get update && sudo apt-get install -y cmake
```

**vcpkg**：

```bash
git clone https://github.com/microsoft/vcpkg /opt/vcpkg && /opt/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=/opt/vcpkg
```

**配置 + 编译**：

```bash
cd realitymap
cmake --preset linux-release        # 首次触发 vcpkg 编译 gdal 等 74 个包，约 30-60 分钟
cmake --build build/linux-release --config Release
```

产物：`build/linux-release/server/realitymap-server`、`realitymap_tests`。

**运行配置**：`cp server/config.json` 并改 `data_root` 指向数据目录、`tianditu_key` 填入、`host 127.0.0.1`。

### systemd 服务（Linux）

`/etc/systemd/system/realitymap.service`：

```ini
[Unit]
Description=RealityMap server
After=network.target

[Service]
WorkingDirectory=/opt/realitymap
ExecStart=/opt/realitymap/build/linux-release/server/realitymap-server server/config.json
Restart=always
RestartSec=5
# 加固
NoNewPrivileges=true
ProtectSystem=strict
ReadWritePaths=/opt/realitymap

[Install]
WantedBy=multi-user.target
```

```bash
systemctl enable --now realitymap
```

注意 `WorkingDirectory` 必须含 `web/public`（静态前端按相对路径挂载）。

### 验证清单

```bash
curl -s http://127.0.0.1:8300/api/ping                 # {"status":"ok","dem_ready":true}
浏览器打开 http://127.0.0.1:8300/                       # 前端 + 地图 + 预览
systemctl status realitymap                              # active (running)
```

## 目录

- [server/](server/) C++ 后端（cpp-httplib + GDAL）
- [web/](web/) 前端（原生 HTML + JS + Leaflet）

## 许可

- 本项目代码：MIT（见 [LICENSE](LICENSE)）
- 算法移植自 [terraining-heightmap-generator](https://github.com/nonta1234/terraining-heightmap-generator)（MIT），见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
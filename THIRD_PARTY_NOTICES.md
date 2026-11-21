# 第三方组件声明

## 移植来源

本项目的核心算法（格式预设 / 重采样 / 预览渲染）移植自：

**terraining-heightmap-generator** — https://github.com/nonta1234/terraining-heightmap-generator

```
MIT License

Copyright (c) 2023 nonta1234

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## 前端依赖（vendored）

- **Leaflet 1.9.4**（`web/public/vendor/leaflet.js` / `leaflet.css`）：BSD-2-Clause，Copyright (c) 2010–2023 Volodymyr Agafonkin 等

## 数据与素材

- ASTER GDEM v3：METI / NASA 提供，用户自行从地理空间数据云下载
- 天地图瓦片：国家基础地理信息中心，用户自备
- OpenStreetMap 数据：ODbL 1.0，© OpenStreetMap contributors

# Unity Asset Store 上架配图

`png/` 是可直接上传的成品，`src/` 是 HTML 源（headless Chrome 渲染，改完跑
`./render.sh` 重新生成全部，2x 采样后缩到精确尺寸）。

| 文件 | 规格 | Asset Store 用途 |
|---|---|---|
| `png/icon-160.png` | 160×160 | Icon |
| `png/card-420x280.png` | 420×280 | Card image |
| `png/cover-1950x1300.png` | 1950×1300 | Cover image |
| `png/social-1200x630.png` | 1200×630 | Social media image |
| `png/screenshot-1-workflow.png` | 2400×1600 | 截图 1 — 三步工作流 + 真实 CLI 输出 |
| `png/screenshot-2-editor.png` | 2400×1600 | 截图 2 — 插件窗口（风格化示意，右下有标注）|
| `png/screenshot-3-sprites.png` | 2400×1600 | 截图 3 — starfall 真实烘焙贴图 |
| `png/screenshot-4-features.png` | 2400×1600 | 截图 4 — 特性总览 |

素材来源：品牌 mark / 配色来自 `figo/branding/`；starfall 大图是
`examples/html/starfall_menu.web.png`；`src/assets/tex_*.png` 是 figo2unity
转 starfall 样例的真实输出贴图（对应文案里 405 objects / 33 unique sprites）。

注意：截图 2 是按 `FigoPrefabImporter.cs` 实际字段绘制的品牌风格示意图，
不是 Unity Editor 实拍——等 mac/Win 端到端跑通后建议补 1-2 张真实编辑器截图
（转换前 Project 视图右键菜单 + 转换后 prefab 拖进场景）替换或追加。

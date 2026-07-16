# figo-convert — AI 工作指南

把 figo 设计（.fig / canvas.json / REST JSON）或 React/HTML 页面转换成游戏引擎
预制体：figo2godot（Godot 4）/ figo2cocos（Cocos Creator 3.x）/ figo2unity
（Unity UGUI）+ web2canvas（HTML→canvas.json，Node/Playwright）。

核心库来自**同级 `../figo` 仓库**（本工程 CMake 以 add_subdirectory 引入，
raylib/quickjs/examples 全部强制 OFF，只编 figo 静态库），ThorVG 静态库按
figo 的约定在 `../thorvg`（可用 `-DTHORVG_INCLUDE_DIR/-DTHORVG_LIBRARY` 覆盖），
.fig 直读需要 `../fig2json`（缺失则退回 CLI / 只收 canvas.json）。

## 构建与验证

构建目录 `build/`（CMake + Ninja）。**Windows（MSVC /MD）必须先加载 VS 环境**，
PowerShell 里用临时 cmd 脚本包一层：

```powershell
$bat = 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1' + "`r`n" +
       'cd /d <repo>\build' + "`r`n" + 'cmake --build . --config Release -j'
Set-Content build\bw.cmd $bat -Encoding ascii; cmd /c "<repo>\build\bw.cmd"; Remove-Item build\bw.cmd
```

首次 configure（同样要在 vcvars 环境下）：

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release [-DFIGO_ROOT=...] [-DTHORVG_LIBRARY=...]
```

验证（零依赖冒烟，2 帧 15 贴图的现成样例）：

```
build\figo2cocos.exe examples\html\starfall_menu.canvas.json out_cocos
build\figo2unity.exe examples\html\starfall_menu.canvas.json out_unity
build\figo2godot.exe examples\html\starfall_menu.canvas.json out_godot
```

产出目录里应有 .prefab/.tscn + textures/（PNG 去重 + .meta），转换器 stdout 会
打印帧数与贴图数。输出目录用完即删（.gitignore 已兜底 `*_out/`）。

## Unity 插件（Asset Store 包）

`unity-plugin/FigoPrefabImporter/` 是 Unity 编辑器包源码（薄 C# 胶水：
Tools → Figo Prefab Importer 窗口 + 右键菜单，调包内 `Editor/Bin/figo2unity.exe`，
Gamma/Linear 按 PlayerSettings 自动选 `--linear`）。
`powershell -File tools/pack_unity_plugin.ps1` 把源码 + exe + starfall 样例
staged 到 `build_unityplugin/`，拷进任意 Unity 工程 Assets/ 即可用/导出。
**注意 exe 必须是带 fig2json 静态库的自包含构建**（同级 `../fig2json` 先
`cargo build --release` 再 configure，否则 .fig 输入不可用）。
已在 Unity 2022.3 batchmode 端到端验证（编译 + 转换 starfall + prefab 断言）。

## 工作流技能

- 导 Cocos → `.claude/skills/figo2cocos/SKILL.md`
- 导 Unity → `.claude/skills/figo2unity/SKILL.md`（Linear 色彩空间工程加 `--linear`）
- HTML/React → Godot → `.claude/skills/web-to-godot/SKILL.md`

## 关键约定与坑

- 三个转换器只链 figo 核心库（无 raylib/quickjs），共享 `apps/exporter_png.h`
  （贴图烘焙，走 `Renderer::renderOverlay`，与运行时逐像素一致）和
  `apps/anim_tracks.h`；`main.cpp` 里以 `#include "../exporter_png.h"` 相对引用，
  改目录结构时注意。
- UUID/GUID/fileID 都是内容派生的——重跑输出稳定，diff 干净。
- Unity：Gamma 空间逐像素一致；客户工程是 Linear 时必须 `--linear` 重导
  （半透明 alpha 预补偿），交付前先问清 ProjectSettings 的 m_ActiveColorSpace。
- web2canvas 用系统已装 Edge/Chrome（playwright-core 不下载浏览器）；首次
  `cd tools/web2canvas && npm install`。Google Fonts 抓取时被屏蔽，用 `--fonts`
  指本地字体。只抓屏幕级状态（`window.__nav`），点击弹层/视口外滚动内容抓不到——
  抓不到的用 `--manual`（有头窗口 + 页面内工具条，人工摆好画面逐屏点采集；工具条
  可拖 ⠿ 换位置；「❄ 冻结」暂停 CSS/WAAPI 动画（CDP playbackRate 0）并把 setTimeout
  回调停车拦截——先冻结再触发 1-2 秒自动消失的 toast/弹层就能定住慢慢选；「🎯 拾取
  节点」点选后先绿框预览 + 「📦 导出此节点」确认，才只采该子树并**强制**导出为
  预制体——comp 名写入 design.pins.json，figo2X 经 `--prefab-pin` 豁免 ≥2 实例/
  占满全屏/≥3 后代三道闸）；`--append` 在 -o/--out 已有 canvas.json 上续帧
  （分工：CLI AI 自动批量，desktop 导出向导默认手动采集，漏采的屏/组件用
  "追加到已有采集"人工补全；GUI 导出目录自带 .web2canvas/ 采集数据 + pins，
  可直接作为追加基底）。
- .fig 输入靠 fig2json（`../fig2json`，Rust）转 canvas.json，缓存于
  `<file>.fig.export/`；没有 fig2json 时给转换器喂 canvas.json / REST JSON。
- 历史坑速查：`docs/agent-memory/`（构建目录缓存指向失效路径、React→Godot
  管线的完整修复日志）。

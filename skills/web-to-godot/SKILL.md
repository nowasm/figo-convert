---
description: Turn a React/HTML page into a ready-to-open Godot 4 project (pixel-faithful, fonts and component reuse included), or export any figo design to Godot .tscn scenes. 把 React/HTML 页面变成可在 Godot 4 打开的工程。当用户要"把这个 React 页面/网页/HTML 导成 Godot"、"web 转 Godot 工程"、"生成 .tscn 场景",或提到 web2canvas / figo2godot / html2godot 时使用。
---

# 把 React/HTML 页面变成 Godot 4 工程

一条链,已在真实页面(8 屏 HUD)端到端验证:

```
React/HTML ──web2canvas──► canvas.json + images/ ──figo2godot──► Godot .tscn + sprites
```

页面在**真实无头浏览器**里渲染再采集,所以 flex/grid 布局、`oklch` 颜色、
`clip-path` 切角、渐变、阴影、字体宽度都按浏览器算好的来。flat fill 表达不了的
(渐变/图片/`<svg>`/clip-path/旋转元素/虚线边框/外发光)会光栅化成 PNG `IMAGE`
fill——和"矢量→贴图"目标一致。

工具位置(没有就先跑 `figo:setup`):
- 采集器:`"${CLAUDE_PLUGIN_ROOT}/tools/web2canvas/"`(需 `npm install` 过;
  **插件更新后 node_modules 会丢,重跑 setup 第 3 步**)
- 导出器:`"${CLAUDE_PLUGIN_DATA}/bin/figo2godot"`(Windows 为 .exe)

> 只有 .fig / canvas.json、不涉及 HTML?直接跳到 figo2godot:
> `"${CLAUDE_PLUGIN_DATA}/bin/figo2godot" <input> <outDir> [--prefabs] [--fonts DIR]`,
> 输入格式与自验流程同 `figo:figo2cocos` 技能。

## 1. 想清楚输入(30 秒)

看一眼目标页面,定四件事:
- **入口**:本地 `file.html` 还是 `http://` URL。本地文件会被起一个临时 HTTP
  服务托管(Babel 的 XHR 加载 .jsx 需要 http://)。
- **采集根** `--root`:整页就 `body`(默认);有固定舞台就指它,如 `#stage`。
- **视口** `--viewport WxH`:**对准设计的舞台尺寸才有 1:1**。
- **多屏** `--states`:页面如果用 `window.__nav(state)` 切屏,列出所有屏名一次
  全采(每屏一个顶层 frame / 一个 .tscn)。非 `__nav` 用 `--nav-fn` 指定。

## 2. 一条命令跑

```
node "${CLAUDE_PLUGIN_ROOT}/tools/web2canvas/html2godot.js" <url|file.html> --out <godotDir> --prefabs \
     --figo2godot "${CLAUDE_PLUGIN_DATA}/bin/figo2godot" \
     [--states "a,b,c"] [--flows FILE] [--fonts DIR] [--root SEL] [--viewport WxH] \
     [--wait MS] [--ai-name] [--prefab-anon] [--browser msedge|chrome]
```

> **约定:`--prefabs` 默认启用**——跑这条命令时总是带上 `--prefabs`,
> 除非用户明确说不要 prefab。`--figo2godot` 总是显式给(指向 BIN 下的导出器)。

`<godotDir>/` 即一个可直接 Godot 4 打开的工程:每屏一个 `.tscn`、去重 sprites、
打包字体、`manifest.json`、`project.godot`。中间产物在 `<godotDir>/.web2canvas/`。

> **节点命名**:容器/图节点默认用 React 组件名或 class(PlayerCard/BigButton…);
> **文本节点按语义角色命名**(不是用文本内容当名)——按内容+上下文判
> `username`/`playerId`/`status`/`levelText`/`count`/`icon`/`heading`/`buttonLabel`/
> `hintText`/`labelText` 等,内容无关、稳定。
>
> **`--ai-name`(图形识别命名)**:规则名只和源码 markup 一样好——压缩类名 /
> Babel-in-browser 应用常给出 `div_0`/`css-1abc`。加 `--ai-name` 后,对每个值得命名
> 的容器**逐个截活图**,结构去重后拼成带编号的 montage,调 `claude` CLI 看图推断
> `PascalCase` 名(CreateRoomButton / HealthBar…),写回 `.tscn` 节点名 + sprite
> 文件名 + 组件场景名。**混合式**:只有候选被 AI 命名,其余仍走规则名。需 `claude`
> 在 PATH;调用失败则该批静默回退规则名,不中断。成本≈每 24 个唯一组件一次调用。
> montage + 候选图落在 `<godotDir>/.ai-name/` 可目检。

关键参数:
- `--fonts DIR`:**几乎必加**。指向页面用的字体根(含 `fonts.css`)。无网/沙箱环境
  拿不到 Google Fonts → 不给字体则浏览器用 fallback 测宽、Godot 用默认字体渲,
  文字会重叠/错位。给了会注入 fonts.css + 等 `document.fonts.ready` 再测量,
  figo2godot 把字体绑进 `theme_override_fonts`。
- `--prefabs`:把重复组件(卡片/按钮/行)抽成 `components/*.tscn`(PackedScene),
  每处实例化 + 按实例覆盖文本——真正的 prefab 复用。同一 React 组件的**不同状态
  变体合并成一个预制体**(缺的子节点隐藏、独有的追加),对齐太差的变体自动内联保真。
  只被别处嵌套使用、从不在帧里单独实例化的组件不生成 .tscn。
- `--prefab-anon`:在 `--prefabs` 基础上,**任何重复>1 的匿名容器**也按结构抽成
  预制体。可选、默认关(会产出较多 `Group_N` 兜底名),**和 `--ai-name` 一起用**最好。
- `--wait MS`:等页面 JS 渲染完(Babel-in-browser 的 React 给 2500~5000ms)。
- `--flows FILE`:采**点击触发的弹窗/overlay**(`--states` 只能到 screen 级)。
  FILE 是 JSON 数组,每个 capture 可选 `nav`(先导航),再跑 `do`/`steps` 交互
  步骤,然后截一帧 → 一个 `.tscn`。步骤格式 `verb:arg`:`click:<sel>` /
  `hover:<sel>`(Playwright 选择器)/ `nav:<state>` / `wait:<ms>`,裸串默认 click。
  多步可串(先开面板再点里面)。capture 间自动用哨兵 state 重挂载隔离,上一个
  弹窗不会漏到下一个。弹窗**带身后暗显的屏一起采**。示例:
  ```json
  [
    { "name": "game_settings", "nav": "game", "do": ["click:[title=\"游戏设置\"]"] },
    { "name": "game_death",    "nav": "game", "do": ["click:[title^=\"调试\"]", "click:text=模拟被击杀"] }
  ]
  ```

完整示例(8 屏 HUD → 8 个 Godot 场景):
```
node "${CLAUDE_PLUGIN_ROOT}/tools/web2canvas/html2godot.js" "<...>/HUD.html" --out hud_app \
  --prefabs --figo2godot "${CLAUDE_PLUGIN_DATA}/bin/figo2godot" \
  --root "#stage" --viewport 1280x720 --wait 2500 \
  --fonts "<...>/fonts" \
  --states "lobby,search,room,role,game,meeting,victory,aftermath"
```

> 只要 canvas.json 不要 Godot:
> `node "${CLAUDE_PLUGIN_ROOT}/tools/web2canvas/index.js" <input> -o out.canvas.json [同上参数]`。
> 该 canvas.json 还能喂 figo2cocos / figo2unity / figoplay / figoedit。

## 3. 自验(必做)

按可用性从高到低:
- **导入自检**:`<godot> --headless --editor --quit --path <godotDir>` —— 干净导入
  无报错即结构 OK。**这步还是后续截图的前置**:不先导入,raw `.png/.ttf` 没生成
  `.import`,运行时会刷屏 `No loader found for resource ...`,贴图/字体全丢。
  **每次重新生成工程后都要再导入一次。**
- **目检渲染**:打开 Godot(或用 godot MCP 的 `run_project`/`game_screenshot`)截图,
  Read 出来对着原页面看:布局/字体/颜色/发光/切角对不对。弹窗工程逐 `.tscn` 截。
- **像素级地真值**:要分辨"解析 bug vs Godot 专有 bug(z 序/裁剪/文本)",拿 figo
  自己的渲染器当 oracle——`"${CLAUDE_PLUGIN_DATA}/bin/figoplay"` 渲同一 canvas.json
  截图对比。和 Godot 一致 = 链路对;不一致 = Godot 侧问题。

发现不对 → 多半落在下面的"已知 limits",回到第 2 步调参或记问题。

## 4. 已知 limits(先告诉用户,别让他以为全覆盖)

- **弹窗/点击态已覆盖**:用 `--flows` 的 click-sim 采点击打开的 popup/overlay。
  前提是元素可被 Playwright 选择器点到;纯 hover 态、依赖真实指针拖拽的交互不采。
- **滚动区外没覆盖**:只截可视视口,每屏滚动超出部分丢失。
- **保真长尾**:letter-spacing(Godot Label 无原生支持)、每角独立圆角的
  NinePatch(现用最大角)。
- 多屏/弹窗都依赖程序化 nav 钩子(`window.__nav` 或 `--nav-fn`)+ 可点击的
  触发元素;没有 nav 钩子就只能单屏。

---
**闭环**:装环境 → 一条命令 `html2godot.js` → Godot 导入 + 截图目检 → 调参/记 limit
→ 再跑。每步都有"眼睛"(Godot 截图 / figoplay 真值),不要盲交付。

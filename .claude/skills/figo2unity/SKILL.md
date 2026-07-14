---
name: figo2unity
description: 把一个 figo 设计（.fig / canvas.json / 设计 JSON）导成 Unity 可直接打开的 UGUI 预制体（.prefab YAML + 贴图 + .meta）。当用户要"把这个设计/页面导成 Unity"、"figo 转 Unity 预制体/prefab"、"生成 Unity .prefab"，或提到 figo2unity 时使用。覆盖：构建 figo2unity → 一条命令转 → 拖进 Unity Assets/ 打开自验 → 看图迭代。
---

# 把 figo 设计变成 Unity UGUI 预制体

一条链，复用 figo2godot/figo2cocos 的渲染+烘焙机制，序列化换成 Unity YAML：

```
.fig / canvas.json / 设计 JSON ──figo2unity──▶ <name>.prefab(+.meta) + textures/*.png(+.meta)
```

节点树用 figo 自己的 **ThorVG 光栅化**烘焙复杂图形，所以贴图和 figo 运行时**逐像素一致**。
`.prefab` 是 Unity 的多文档 YAML（`--- !u!<classId> &<fileID>`：GameObject →
RectTransform / CanvasRenderer / MonoBehaviour(UI.Image / UI.Text)），拖进任意
带 Canvas 的场景即可。**Unity 2021+ 与 Unity 6 都能读**（TextureImporter 会自动升级）。

## 节点映射

| figo 节点 | Unity 产物 |
|---|---|
| TEXT（纯色） | GameObject + `UI.Text`（字符串/字号/颜色/对齐/换行，内置字体） |
| TEXT（渐变/图片填充） | GameObject + `UI.Image`（烘焙 PNG——染色 quad 复现不了，直接烤像素） |
| 纯色矩形 / 容器底色 | GameObject + `UI.Image`（**不挂 sprite** = 引擎自带的染色 quad，零贴图） |
| 椭圆/矢量/渐变/图片/描边/外发光/圆角面板 | GameObject + `UI.Image`（烘焙 PNG；够大且可拉伸的走 9 宫格 `m_Type: 1` + meta `spriteBorder`） |
| 容器 | GameObject，挂底色 Image + 子节点 |
| 空容器 | GameObject（透传） |

**坐标翻转**：每个 RectTransform 的 anchor **和 pivot 都钉 `(0,1)`**（左上角），
`anchoredPosition = (relX, -relY)`，`sizeDelta` 用 figo 的盒子尺寸——Figma(Y 向下) →
UGUI(Y 向上) 逐层正确复合。半透明容器挂 `CanvasGroup`，不可见节点 `m_IsActive: 0`。

## 0. 先确认（一次性）

- **figo2unity 在吗**：在 `build/` 下编出来（macOS 见 CLAUDE.md，秒级增量）：
  ```bash
  cd build && cmake . && cmake --build . --target figo2unity -j
  ```
  Windows 必须在 vcvars64 下用 CLAUDE.md 的 PowerShell cmd 包装跑。
- **输入是什么**：`figo2unity` 吃 `FigmaUI::fromFile` 认的所有格式——`.fig`（自动
  转 canvas.json 缓存）、fig2json 的 `canvas.json`、Figma REST JSON、或 figo app
  工程的 `design.json`。
- **输入要完整**：HTML 设计稿走 web2canvas 时，只给 `--states` 抓不到点击弹窗/
  多阶段界面，要配 `--flows`（GOGO KILL 完整 32 帧捕获现成在
  `hud_full/.web2canvas/design.canvas.json`，可直接复用）。

## 1. 一条命令转

```bash
build/figo2unity <input> [outDir] [--frame NAME] [--fonts DIR] [--scale N]
                 [--prefabs] [--prefab-anon] [--no-prefab T1,T2] [--tmp]
```

- `outDir` 默认 `unity_out`。每个顶层 frame 出一个 `<Frame>.prefab`，贴图共享
  `<outDir>/textures/`。
- `--frame NAME` 只导一个 frame；不给则全导。
- `--fonts DIR` **强烈建议给**（设计有配套字体时）：递归收集目录下的 .ttf/.otf，
  拷进 `<outDir>/fonts/` 并生成 TrueTypeFontImporter .meta；每个 `UI.Text` 按
  (family, weight, italic) 匹配到具体字体文件（找不到的 family 回退到 Noto Sans
  保证 CJK 覆盖），行距按字体 hhea/head 表的真实 em 系数换算。不给则全部落
  Unity 内置字体——字形和度量都和设计不一致，观感明显打折。
- `--scale N` sprite 超采样倍率（默认 2，更清晰；9 宫格内部按 1x 烘焙保证切边锐利）。
- **主题 token**：设计带变量表（colorVar）时自动落 `figo_tokens.json`——modes +
  token→各 mode 色值 + "帧/节点路径/通道(fill|text)→token 名"绑定表。prefab 里的
  颜色是解析后的字面量，引擎侧换肤脚本按这份绑定表回写颜色即可（只记实心填充
  与文本色；烘进贴图的渐变/效果不记）。
- `--prefabs` 把重复组件（卡片/按钮/行）抽成 `components/*.prefab`，各处用 Unity
  嵌套预制体（`PrefabInstance` + `m_Modifications`）实例化：文本/颜色/换图/位移/
  隐藏子节点都是逐实例 override，新增子节点走 `m_AddedGameObjects`（需 Unity
  2022.2+）。识别/聚类逻辑与 figo2godot 同源（compType 分组 → 最富实例当超集母版
  → poorFit 门槛把差太多的变体拆成独立 prefab 或内联）。`--prefab-anon` 连匿名
  重复容器也抽；`--no-prefab HPanel,HRow` 屏蔽指定 compType。
- `--tmp` 文本出 **TextMeshProUGUI**（SDF 级清晰度）而非 legacy UI.Text。所有
  文本引用默认 `LiberationSans SDF` 字体资产（SDF 图集没法离线生成）——**目标
  工程必须已导入 TMP Essential Resources**，自定义/CJK 字体在 Unity 里把字体
  资产换掉即可（fixed 多行框升级成真省略号 Ellipsis；emoji/CJK 在默认资产下
  是豆腐块；字体度量与设计字体不同时紧排文本可能顶到邻居——换字体资产后消失）。
  batchmode 验证工程要装 essentials 得用 Unity CLI 的 `-importPackage
  "<Library/PackageCache/com.unity.ugui@*/Package Resources/TMP Essential
  Resources.unitypackage>"` 单独跑一趟（Editor 脚本里 AssetDatabase.
  ImportPackage 在 batchmode 是异步的，Exit 前不落地）。

例：
```bash
build/figo2unity examples/html/starfall_menu.canvas.json unity_out
```

## 2. 拖进 Unity 工程

把 `outDir` 整个目录（`*.prefab` + `*.prefab.meta` + `textures/`）拷进目标 Unity
工程的 `Assets/` 下某个子目录。**`.meta` 必须一起拷**——里面是贴图的 GUID，prefab 靠
`{fileID: 21300000, guid: …}` 引用它们；少了 .meta，Unity 会重新分配 GUID，引用就断了。

打开 Unity 等它导入资源，把 `<Frame>.prefab` 拖进一个有 Canvas 的场景里，即可看到
还原的界面。

## 3. 自验

### 结构自检（无 Unity 时的快筛）

```bash
python3 - <<'PY' unity_out
import re,glob,os,sys
os.chdir(sys.argv[1] if len(sys.argv)>1 else "unity_out")
tex={re.search(r"^guid: ([0-9a-f]{32})$",open(m).read(),re.M).group(1) for m in glob.glob("textures/*.png.meta")}
for pf in glob.glob("*.prefab"):
    txt=open(pf,encoding="utf8").read()
    docs=re.findall(r"^--- !u!(\d+) &(\d+)\n(.*?)(?=^--- |\Z)",txt,re.M|re.S)
    ids={int(i) for _,i,_ in docs}; bad=[]
    for cls,fid,body in docs:
        for rid,guid in re.findall(r"\{fileID: (-?\d+)(?:, guid: ([0-9a-f]{32}))?",body):
            rid=int(rid)
            if guid:
                if rid==21300000 and guid not in tex: bad.append(f"sprite guid {guid}")
            elif rid and rid not in ids: bad.append(f"dangling {rid} in &{fid}")
    roots=[f for c,f,b in docs if c=="224" and "m_Father: {fileID: 0}" in b]
    print(pf,"OK" if not bad and len(roots)==1 else f"BAD {bad[:3]} roots={len(roots)}")
PY
```

全 `OK` 表示对象图自洽、单根、所有 sprite 引用都能落到一张有 .meta 的贴图上。

### 真机自验：Unity batchmode（已端到端验证）

有 Unity（Windows 机上装了 `C:\Program Files\Unity 6000.0.75f1`）就跑真引擎：
建个空工程目录（只要 `Assets/`；**Unity 6 自动生成的 manifest 不含 UGUI**——首跑一次
生成 `Packages/manifest.json` 后往 dependencies 补 `"com.unity.ugui": "2.0.0"`，且该
文件必须**无 BOM**：PowerShell 5.1 `-Encoding utf8` 带 BOM 会让 Package Manager 报
"not valid JSON"，用 `[IO.File]::WriteAllText(..., UTF8Encoding($false))` 写），把
输出拷成 `Assets/figo/`，放一个 Editor 脚本逐 prefab 实例化到 world-space Canvas +
正交相机离屏截图（脚本见 `.claude/skills/figo2unity/` 同目录参考或临时手写，核心：
`AssetDatabase.LoadAssetAtPath<GameObject>` → 检查 missing script 数（能抓出
MonoBehaviour guid 写错——ScrollRect/RectMask2D 等 UGUI 组件引用靠它验）→
`PrefabUtility.InstantiatePrefab` 挂 Canvas → RenderTexture + `ReadPixels` 存 PNG）：

```powershell
& "C:\Program Files\Unity 6000.0.75f1\Editor\Unity.exe" -batchmode -projectPath <proj> `
  -executeMethod FigoValidate.Run -quit -logFile <proj>\unity.log
```

**不要加 `-nographics`**（RenderTexture 截图需要 GPU）。首次导入 ~1-3 分钟。
截图用 Read 工具目检：层级顺序、文字位置、9 宫格面板拉伸是否走样。

## 关键约定与坑

- **每帧一个自包含 prefab**，节点全部内联——v1 **不做嵌套 prefab 提取**（Unity
  PrefabInstance 的 m_Modifications 序列化重得多）。重复卡片想复用，在 Unity 里手动抽。
- **Linear 色彩空间下文字发淡（已修，两层）**：Unity 工程用 Linear（URP 默认）时
  UGUI 在线性空间混合，文字两处发淡——(1) 半透明文字/opacity 淡字：导出器把颜色与
  其下方真实像素**预合成**后发不透明色（祖先纯色链精确合成，兜底"隐藏全部文字整帧
  渲染"采样），节点 opacity 折进颜色不再挂 CanvasGroup；(2) 不透明文字的字形 AA
  覆盖像素（小字号"黑字不够黑"的主因）：导出物自带 `FigoUIText.shader` +
  `FigoText.mat`（所有 UI.Text 引用），Linear 下按亮度自适应重映射覆盖 alpha
  （暗字 `1-(1-a)^2.2`、亮字 `a^2.2`），Gamma 下 `UNITY_COLORSPACE_GAMMA` 自动停用，
  也处理了 `Canvas.vertexColorAlwaysGammaSpace` 的顶点色转换。实测 Linear 工程里
  字芯/p10 像素与浏览器基准对齐。删掉这两个文件则回退内置 UI/Default（Gamma 工程
  无影响）。
- **字体**：优先走 `--fonts`（真字体打包，见上）。匹配到的文件权重/斜体已烤在字体
  里，不再合成 bold/italic（避免双重加粗）。没匹配上才落内置字体
  `{fileID: 10102, guid: 0000000000000000e000000000000000}`（2022.2 前是 Arial.ttf，
  之后是 LegacyRuntime.ttf，引用不变），此时行距按 1.15em 近似。要 SDF 级精度在
  Unity 里换 TMP。
  overflow 按 figo `autoResize` 映射：`WIDTH_AND_HEIGHT`→双向 Overflow（框贴合文字）、
  `HEIGHT`→Wrap+纵向 Overflow（定宽向下长）、固定框多行→Wrap+**Truncate**（裁掉、
  不压到下面的邻居）、固定框单行→不换行+Overflow（保持单行向右溢出，接近 figo 的
  截断视觉；这里若用 Truncate，Unity 字体比设计框高一丝就会整行消失）。
- **纯色 quad 不用贴图**：`UI.Image` 不挂 sprite 本来就渲白色 quad，靠 `m_Color` 染色。
- **轴对齐**：静态旋转不导出。旋转元素按未旋转的盒子摆放（烘焙类 sprite 的旋转已烤进像素）。
- **CSS 动画重放（2026-07-07 起）**：web2canvas 抓到的 `anim`（opacity/scale/pos/rot 四类
  关键帧）导出为 legacy AnimationClip（`anims/<帧>_<节点>_<n>.anim`）+ 节点上的 Animation
  组件（PlayAutomatically）。透明度走 CanvasGroup.m_Alpha、位移走 anchoredPosition 浮点
  曲线（y 取反）、旋转烘成**四元数 m_RotationCurves**（legacy 不认 EulerCurves；按 ≤45°
  细分防 slerp 走短路）、缩放/旋转的 transform-origin 落到 RectTransform pivot（anchoredPosition
  已补偿）。缓动在导出侧预采样成线性段。无限循环 wrap=Loop、有限 wrap=ClampForever 定住尾帧。
  坑：动画数值一律用 a2s 格式化——c2s 是颜色通道格式化器会把值钳到 [0,1]（负角度/长时长全毁）。
- **GUID/fileID 确定性**：贴图 GUID 由内容哈希派生、fileID 由每 prefab 名字做种子的
  splitmix64 派生，**重跑输出稳定**、同图自动去重。若已在 Unity 里改过这些 .meta 的
  GUID，重跑会覆盖。
  **fileID 必须像随机数（已踩）**：曾用 1000000 起的小连续整数当 fileID，配合
  `--prefabs` 后 Unity 导入报 `Assertion failed: 'fidA != fidB'`——PrefabImporter
  实例化嵌套预制体时把源 fileID 和实例 fileID **混合**成导入产物的 id，宿主和组件里
  同一批小整数必然撞车。63 位伪随机 id 后消失。
- 烘焙/去重/9 宫格逻辑与 `apps/figo2cocos/main.cpp` 逐行同源；像素问题先对照那边。

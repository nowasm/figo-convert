---
description: Export a figo design (.fig / canvas.json / Figma REST JSON) to Cocos Creator 3.x prefabs (.prefab + textures + .meta). 把设计稿导成 Cocos Creator 3.x 可直接打开的预制体。当用户要"把这个设计/页面导成 Cocos"、"转 Cocos Creator 预制体/prefab"、"生成 .prefab",或提到 figo2cocos 时使用。
---

# 把 figo 设计变成 Cocos Creator 3.x 预制体

工具:`"${CLAUDE_PLUGIN_DATA}/bin/figo2cocos"`(Windows 为 `figo2cocos.exe`,
PowerShell/cmd 会自动补后缀;**不存在就先跑 `figo:setup` 技能装工具链**)。

```
.fig / canvas.json / 设计 JSON ──figo2cocos──► <name>.prefab + textures/*.png(+.meta)
```

节点树用 figo 自己的 **ThorVG 光栅化**烘焙复杂图形,贴图和 figo 运行时**逐像素一致**。
`.prefab` 是一串 `__id__` 互引的引擎对象数组(`cc.Prefab` → `cc.Node` →
`cc.UITransform`/`cc.Sprite`/`cc.Label`…),格式对齐 **Cocos Creator 3.4.2+**。

## 节点映射

| figo 节点 | Cocos 产物 |
|---|---|
| TEXT | `cc.Node` + `cc.Label`(字符串/字号/颜色/对齐/换行,系统字体) |
| 纯色矩形 / 容器底色 | `cc.Node` + `cc.Sprite`(共享 `white.png`,用 `_color` 染色——免分辨率) |
| 椭圆/矢量/渐变/图片/描边/外发光/圆角面板 | `cc.Node` + `cc.Sprite`(烘焙 PNG;够大且可拉伸的走 9 宫格 `_type=1`) |
| 容器 | `cc.Node`,挂底色 sprite + 子节点 |
| 空容器 | `cc.Node`(透传) |

**坐标翻转**:每个节点 anchor 取 `(0,1)`(左上角),`_lpos = (relX, -relY)`,
`cc.UITransform._contentSize` 用 figo 的盒子尺寸——Figma(Y 向下) → Cocos(Y 向上)
逐层正确复合。

## 0. 先确认(一次性)

- **工具在吗**:`ls "${CLAUDE_PLUGIN_DATA}/bin"` 里有 figo2cocos。没有 → `figo:setup`。
- **输入是什么**:figo2cocos 吃所有这些格式——`.fig`(自动转 canvas.json 缓存;
  Windows 预编译包内置解析,macOS/Linux 见 setup 的 fig2json 说明)、
  fig2json 的 `canvas.json`、Figma REST JSON、figo app 工程的 `design.json`。
- **输入要完整**:HTML 设计稿走 web2canvas 时,只给 `--states` 抓不到点击弹窗/
  多阶段界面(弹窗、设置面板、死亡页……),要配 `--flows` 捕获文件(格式见
  `figo:web-to-godot` 技能第 2 步)——否则"导出不完整"是捕获缺帧,不是导出器丢内容。

## 1. 一条命令转

```bash
"${CLAUDE_PLUGIN_DATA}/bin/figo2cocos" <input> [outDir] [--frame NAME] [--fonts DIR] [--scale N]
                                       [--prefabs] [--prefab-anon] [--no-prefab T1,T2]
```

- `outDir` 默认 `cocos_out`。每个顶层 frame 出一个 `<Frame>.prefab`,贴图共享
  `<outDir>/textures/`。
- `--frame NAME` 只导一个 frame;不给则全导。
- `--fonts DIR` **设计有配套字体时强烈建议给**:递归收集 .ttf/.otf 拷进
  `<outDir>/fonts/`(`ttf-font` .meta),每个 Label 按 (family, weight, italic)
  匹配到 `cc.TTFFont` 资源(找不到的 family 回退 Noto Sans 保 CJK),匹配到的
  权重/斜体不再叠加合成 `_isBold`/`_isItalic`。不给则全落系统字体。
- `--scale N` sprite 超采样倍率(默认 2,更清晰;9 宫格内部按 1x 烘焙保证切角锐利)。
- **主题 token**:设计带变量表(colorVar)时自动落 `figo_tokens.json`——modes +
  token→各 mode 色值 + "帧/节点路径/通道(fill|text)→token 名"绑定表。prefab 里的
  颜色是解析后的字面量,引擎侧换肤脚本按这份绑定表回写颜色即可(只记实心填充
  与文本色;烘进贴图的渐变/效果不记)。
- `--prefabs` 把重复组件抽成 `components/*.prefab`,各处用**真嵌套预制体**引用
  (`cc.PrefabInstance` + `CCPropertyOverrideInfo`:`_name`/`_lpos`/`_contentSize`/
  `_string`/`_color`/`_spriteFrame`/`_active` 逐实例 override,新增子节点走
  `mountedChildren`)。`--prefab-anon` 连匿名重复容器也抽;`--no-prefab HPanel,HRow`
  屏蔽指定 compType。
  **fileId 坑(已踩)**:宿主里每个实例节点的 `PrefabInfo.fileId` 必须**文件内唯一**。
  override 定位用的 `cc.TargetInfo.localID` 是**源内** fileId(引擎实例化后根节点保留
  源 `prefab.fileId`),与宿主实例 fileId 无关。
  **nestedPrefabInstanceRoots 坑(已踩,"N 个实例只显示 1 个"的真正根因)**:宿主
  **根节点的 PrefabInfo** 必须带 `nestedPrefabInstanceRoots: [{__id__: 各实例 stub}]`
  ——引擎只展开这个清单里的嵌套实例,漏了它们就永远是空 stub。figo2cocos 已自动生成;
  下方自检脚本会验证。

例(插件自带样例):
```bash
"${CLAUDE_PLUGIN_DATA}/bin/figo2cocos" "${CLAUDE_PLUGIN_ROOT}/examples/html/starfall_menu.canvas.json" cocos_out
```

## 2. 拖进 Cocos 工程

把 `outDir` 整个目录(`*.prefab` + `*.prefab.meta` + `textures/`)拷进目标
Cocos Creator 3.x 工程的 `assets/` 下某个子目录。**`.meta` 必须一起拷**——里面是
贴图/sprite-frame 的 UUID,prefab 靠 `uuid@f9941` 引用它们;少了 .meta,Cocos 会
重新分配 UUID,引用就断了。

打开 Cocos Creator,等它导入资源,把 `<Frame>.prefab` 拖进一个有 Canvas 的场景里,
即可看到还原的界面。

## 3. 自验

没有 Creator 时,先做**结构自检**(再人工开 Cocos 目检):

```bash
python3 - <<'PY'
import json,glob,os
os.chdir("cocos_out")
tex={json.load(open(m))["uuid"] for m in glob.glob("textures/*.png.meta")}
for pf in glob.glob("*.prefab"):
    a=json.load(open(pf)); n=len(a); bad=[]
    def walk(o,w):
        if isinstance(o,dict):
            if "__id__" in o and not(0<=o["__id__"]<n): bad.append(w)
            for k,v in o.items(): walk(v,f"{w}.{k}")
        elif isinstance(o,list):
            [walk(v,f"{w}[{i}]") for i,v in enumerate(o)]
    [walk(o,f"[{i}]") for i,o in enumerate(a)]
    assert a[0]["__type__"]=="cc.Prefab" and a[0]["data"]["__id__"]==1
    for o in a:  # 每个 sprite frame 的贴图 uuid 都得有 meta
        if o.get("__type__")=="cc.Sprite":
            assert o["_spriteFrame"]["__uuid__"].split("@")[0] in tex
    print(pf,"OK" if not bad else f"DANGLING {bad}")
print("done")
PY
```

全 `OK` 表示对象图自洽、所有 sprite-frame 引用都能落到一张有 .meta 的贴图上。
人工目检重点:层级顺序、文字位置(Label 锚点左上 + 顶对齐时短文本可能偏上)、
9 宫格面板拉伸是否走样。

**装了 Creator 时的无头导入验证**(比结构自检强:真引擎反序列化):
建个最小工程(`package.json` 含 `{"creator":{"version":"3.8.8"}}` + 空 `assets/`),
把输出拷成 `assets/figo/`,跑
`CocosCreator --project <proj> --build "platform=web-mobile"`
——build 本身会因"没有启动场景"失败(exit 34,预期),但**资源导入先完成**:
`library/` 里出现每个 prefab/字体/贴图 uuid 的编译产物 = 全部反序列化成功;
`temp/logs/project.log` 里除 start-scene 外不应有 error。

**真渲染验证(最强)**:无头导入只验反序列化,不验画面。要看真像素:在验证工程
`assets/` 放一个手写 `test.scene`(Canvas 1280×720 + 正交 Camera + 目标 prefab 的
实例 stub;场景 PrefabInfo 记得带 `nestedPrefabInstanceRoots: [{__id__: stub}]`,
否则实例不展开),`--build "platform=web-mobile;startScene=<场景uuid>"` 构建后起
HTTP 服务,用无头浏览器截图 + 页面里 `cc.director.getScene()` 数节点。这条链当场
抓出过"实例不展开"的 bug,结构自检和无头导入都放行了它。

### 进阶:cocos-cli MCP 真机自验

官方 cocos-cli 带 `start-mcp-server`,能用真实引擎加载工程并通过 MCP 程序化校验
(prefab 反序列化成活节点、层级/组件/属性全可查)。figo 插件已带其 MCP 连接配置
(`http://localhost:9527/mcp`),起了服务后 `/mcp` 重连即可:

```bash
node <cocos-cli>/dist/cli.js start-mcp-server --project <cocosProj> --port 9527
```

关键工具链:`assets-refresh {dir}` 导入 → `assets-query-asset-info` 确认
`type:"cc.Prefab", invalid:false` → `scene-open` 打开 prefab 看活节点树 →
`scene-query-component` 读 Label/UITransform 真实属性。
怪癖:(1) 同一节点 `scene-query-node` 用场景实例路径,`scene-query-component`
在 prefab 实例里常 404/500——位置用 query-node 验,属性值优先直接读 .prefab JSON;
(2) 工具返回双层包裹(`content[0].text` 里又一层 `{"result":{"data":...}}`),解析再剥一层。

> 文本位置/对齐:Label 按 figo `autoResize` 映射 `_overflow`:
> `WIDTH_AND_HEIGHT`→NONE(贴合文字)、`HEIGHT`→RESIZE_HEIGHT(定宽换行增高)、
> `NONE`/`TRUNCATE`→CLAMP(锁住文本框、框内对齐、超出裁剪)。

## 关键约定与坑

- **只产 v3.4.2+ 格式**。Cocos 2.4.x 不支持。
- **嵌套 prefab 走 `--prefabs`**;默认每帧一个自包含 prefab、节点全部内联。
- **字体走 `--fonts`**;不给时 Label 走 `_isSystemFontUsed`,要自定义字体
  得在 Cocos 里手动给 Label 指 Font 资源。
- **轴对齐**:静态旋转不导出。绝大多数 UI 不旋转;个别旋转元素按未旋转的盒子摆放
  (烘焙类 sprite 的旋转已烤进像素)。
- **CSS 动画重放**:web2canvas 抓到的 `anim` 导出为 `scripts/FigoAnim.ts` 运行时组件
  (固定 UUID,仅在用到时生成)+ 预制体里的组件实例(关键帧扁平数组,缓动已预采样成
  线性段)。脚本在 update 里重放:opacity→UIOpacity、scale/rot 的 transform-origin
  枢轴用位置补偿模拟(不动 anchorPoint)、CSS 顺时针角度→cocos `angle` 取负、y 轴取反。
  **交付时 `scripts/` 目录必须随包**(丢了 .ts.meta 组件引用就断)。
- **UUID 确定性**:贴图 UUID 由内容哈希派生、prefab/节点 fileId 由帧名+计数派生,
  **重跑输出稳定**、同图自动去重。但若你已在 Cocos 里改过这些 .meta 的 UUID,
  重跑会覆盖回派生值。
- **导出物按 Creator 3.8.8 的字节习惯生成**(当前 importer 版本号 + `_name`=文件名 +
  整数不带小数点 + 自带目录 meta),导入后零文件被编辑器改写;升级 Creator 大版本后
  若出现自动改写,属版本迁移,无害。

# Unity 插件 macOS 支持 — mac 侧待办清单

状态(2026-07-18,mac 侧已完成):`unity-plugin/prebuilt/macos/figo2unity` 已提交
(**universal arm64+x86_64**,minos 11.0,~10 MB,自包含 fig2json 静态库,
两个架构分别用 `arch -arm64/-x86_64` + `FIGO_FIG2JSON=nonexistent` 转
wallet.fig 验证 `RESULT: OK`)。mac 侧实际踩到的坑:

- figo 的 `FIGO_FIG2JSON_LIB` 链接原来被 `AND WIN32` 限死,已改成平台无关
  (默认路径 win=`fig2json.lib` / 其余=`libfig2json.a`),非 Windows 平台补链
  系统 `bz2`(fig2json 的 bzip2 crate 依赖)。
- **最低系统版本**:三个静态件(thorvg / fig2json / figo+converters)都要用
  `MACOSX_DEPLOYMENT_TARGET=11.0`(cmake 用 `-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0`)
  重建,否则产物 minos 跟宿主 SDK 走(实测 26.0)。`vtool -show-build` 验证。
- 本机 thorvg 在 `~/work/ai/thorvg`(非 `../thorvg` 约定位),macOS 11 版构建在
  `build_static_mac11/`(meson 配置对齐 build_static:static+cpu engine+
  svg,lottie,ttf,png,jpg,webp loaders,threads/simd off,**`-Dextra=` 置空**——
  默认的 openmp 会引入 `___kmpc_*` 未定义符号),configure 时用
  `-DTHORVG_INCLUDE_DIR/-DTHORVG_LIBRARY` 指过去。
- **通用二进制的实际做法**(与第 4 节略有出入):
  - Rust:Homebrew rust 不带 x86_64 std,`brew install rustup` +
    `rustup toolchain install stable --profile minimal --target x86_64-apple-darwin`;
    构建时 **rustup 的 bin 必须排在 PATH 前面**(`PATH=/opt/homebrew/opt/rustup/bin:$PATH`),
    否则 cargo 调到 Homebrew 的 rustc 报 `can't find crate for core`。
    产物在 `target/x86_64-apple-darwin/release/libfig2json.a`。
  - ThorVG:meson cross 文件即可在 arm64 机上出 x86_64(clang 通用工具链),
    构建目录 `build_static_mac11_x64/`。cross 文件关键内容:
    `[binaries] c/cpp=clang/clang++`;`[built-in options]` 四项 args 全部
    `['-arch','x86_64']`;`[host_machine] system=darwin, cpu_family=x86_64`。
  - figo-convert:另开 `build_x64/`,加 `-DCMAKE_OSX_ARCHITECTURES=x86_64` 并把
    `-DTHORVG_LIBRARY/-DFIGO_FIG2JSON_LIB` 指到 x86_64 的两个 .a,最后
    `lipo -create build/figo2unity build_x64/figo2unity -output ...`。
    x86_64 侧冒烟直接跑(Rosetta),两 slice 用 `arch -arm64/-x86_64` 各验一遍。
- 本机无 pwsh,staging 用 bash 手工镜像了 ps1 逻辑(注意:ps1 会优先取 build/
  的新鲜构建,但 build/figo2unity 是 lipo 前的单架构产物——**打包 mac 二进制
  一律取 prebuilt/macos 的 universal 版**)。
- 第 6/7 步已在 mac 完成(2026-07-18,Unity 2022.3.62f3 batchmode):staged 包
  → `AssetDatabase.ExportPackage` 出 `.unitypackage`(编译零错误)→ 导入全新
  工程(此时 figo2unity 确实丢了可执行位)→ 反射驱动 `Convert(false)` 转
  starfall 样例 → `RESULT: OK, 2 prefab(s), 33 unique sprite(s)`,chmod 修复
  路径实测生效。e2e 脚手架:scratchpad 的 FigoE2E.cs(反射设 inputPath/
  outputFolder 后调私有 Convert)。

原始待办(2026-07-17,Windows 侧记录):

- C# 已做平台分发:`FigoPrefabImporter.cs` 在 OSXEditor 下找 `Editor/Bin/figo2unity`
  (无后缀 mach-o),启动前自动 `chmod +x`(.unitypackage 不保留可执行位)。
- 打包脚本 `tools/pack_unity_plugin.ps1` 双平台取用:宿主平台优先 `build/` 新鲜
  构建,另一平台用 `unity-plugin/prebuilt/<platform>/` 的已提交副本;mac 二进制
  缺失时只警告(打出 Windows-only 包)。
- `unity-plugin/prebuilt/win-x64/figo2unity.exe` 已提交(自包含构建,内嵌
  fig2json 静态库)。

## mac 上要做的事

1. **依赖**:同级仓库 `../fig2json` 执行 `cargo build --release`(产出
   `target/release/libfig2json.a`);ThorVG 静态库按 figo 约定在 `../thorvg`
   (转换器不需要 GL,静态 SW 引擎即可),路径不对就用
   `-DTHORVG_INCLUDE_DIR/-DTHORVG_LIBRARY` 覆盖。
2. **构建**(clang++ + Ninja,figo 在 mac 实测可用):

   ```sh
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j
   ```

   configure 输出里确认 fig2json 静态库被找到(`FIGO_FIG2JSON_LIB` 非空)——
   否则产出的是 CLI 回退版,.fig 输入在用户机器上不可用。
3. **自包含验证**(关键,照 Windows 侧的验法):拷一份无 `.fig.export/` 缓存的
   .fig,`FIGO_FIG2JSON=nonexistent build/figo2unity <fig> /tmp/out` 应
   `RESULT: OK`。
4. **通用二进制(推荐)**:Asset Store 用户有 Intel mac。rustup 补
   `x86_64-apple-darwin` target,fig2json 两个 target 各编一份,ThorVG/figo 用
   `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` 各自构建后 `lipo -create` 合并;
   嫌麻烦可先只出 arm64,包描述里注明 Apple Silicon only。
   `file build/figo2unity` / `lipo -archs` 验证架构。
5. **入库**:

   ```sh
   cp build/figo2unity unity-plugin/prebuilt/macos/figo2unity
   git add unity-plugin/prebuilt/macos/figo2unity && git commit
   ```

6. **重新打包**:回任一平台跑 `tools/pack_unity_plugin.ps1`(mac 上用 pwsh),
   确认输出里 `macos converter:` 不再是 `(none)`,且 Editor/Bin 下两个二进制都在。
7. **端到端**:mac Unity 工程 Assets/ 放入包,Tools → Figo Prefab Importer 转
   `Samples/starfall_menu.canvas.json` 与一个 .fig,均应产出 prefab;首次运行
   验证 chmod 路径生效(删掉可执行位再导一次)。

## 注意

- prebuilt 二进制每次重建都提交会膨胀 git 历史——**只在发布节点刷新提交**,
  平时本地 build/ 里的新鲜产物会被打包脚本优先取用。
- exe/mach-o 各约 3 MB,正常提交即可,无需 LFS。

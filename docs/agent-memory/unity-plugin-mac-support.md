# Unity 插件 macOS 支持 — mac 侧待办清单

状态(2026-07-17,Windows 侧已完成):

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

# Android ASTC 数据包（不支持 BC 的设备）

面向 Mali-G720 这类**没有 BC 贴图支持、但有 ASTC**的设备。做法和 iOS 的[老机型补充包](LEGACY_DEVICES.zh-CN.md)相同：在电脑上把游戏里绝大多数 BC3 贴图离线转成 ASTC 4×4，再把这批替换打进 Android 数据包的 RomFS。

**它不是运行时回退**：转换结果在数据层面把贴图格式码换成了 ASTC，运行时只是照格式上传；设备不支持 ASTC 会直接报错。支持 BC 的 Adreno 等设备用正常的 `make_package.py` 包即可，不需要也不建议用这份 ASTC 包。

这个流程只在仓库里放工具和文档；转码产物、补充数据档和最终 zip 都由玩家用自己的游戏文件在本机生成，**不要提交进仓库**。

## 先确认三件事

- 已有解出的 `RomFS/`、`ExeFS/`，以及正常流程打出的 `game/`、`shaders/spirv/` 和 `derived.tsv`（见 [Android 数据包格式](ANDROID_PACKAGE.zh-CN.md) 与 [Android 构建](ANDROID_BUILD.zh-CN.md)）。
- Mali 的着色器分组档位是 **16**，不是文档里 Adreno 常用的 64。`shaders/spirv/host-subgroup-size.txt` 必须是 16，且 SPIR-V 就是用 `--host-subgroup-size 16` 翻译的。
- Python 环境里有 `numpy`、`Pillow`（`astc_pack.py` 解码 BC3 与改缩略图要用），并准备好 **astcenc 5.2.0**。Windows 可直接用 ARM 上游发布页的可执行文件；`ASTCENC` 环境变量或 `--astcenc` 指向它。

以下命令在仓库根目录执行；`R` 是原版 RomFS，`D` 是输出目录，`REUSE` 是正常打包留下的复用目录（含 `game/`、`shaders/spirv/`、`derived.tsv`）。示例是 Git Bash；PowerShell 把路径换成 `C:\...`，并把 `export PYTHONUTF8=1` 换成 `$env:PYTHONUTF8=1`。

```bash
export PYTHONUTF8=1          # Windows 控制台中文输出/路径不乱码
R=/d/dm-work/data/switch/3.3.94012/Update/RomFS
D=/d/dm-work/astc
REUSE=/d/dm-work/pack-build/reuse
```

## 第 1 步：BC3 → ASTC 4×4

```bash
python dm/tools/astc_pack.py "$R" "$D/work" \
  --astcenc /c/tools/astcenc/astcenc-avx2.exe --preset thorough
```

产物在 `$D/work`：

| 文件/目录 | 内容 |
| --- | --- |
| `files/data/...` | 转好的 `.texture`（格式码 0x3d/0x3e → 0x5f/0x51） |
| `files/data/hd/texture_desc_cache.json` | 同步改过格式码与缩略图的贴图描述缓存 |
| `repl.tsv` | 27,216 条替换：27,215 张贴图 + 上面的缓存文件 |
| `quality.tsv` | 每张贴图相对 BC3 解码真值的 PSNR |
| `summary.txt` | 转码统计 |

3.3.94012 + `thorough` 的实测结果是 31,092 张里转 27,215 张、跳过非 BC3 3,877 张（BC1/BC4/BC5/BC6H 不转，留给运行时在不支持 BC 的设备上解压），墙钟约 20 分钟（本机 Windows）。换游戏版本时数字会变，不要拿旧版本的 `repl.tsv` 混用。

## 第 2 步：写成独立的补充数据档

```bash
python dm/android/tools/casc_overlay_win.py pack "$R" "$D/pack" "$D/work/repl.tsv" 0-8
```

- `0-8` 是补充数据档的块号/档号，必须保证原 RomFS 里没有 `data/00-00000008.data`；工具会检查，已存在就换一个空闲档号。
- 产物是 `$D/pack/00-00000008.data`（3.3.94012 实测 1,017,670,670 字节）和 `$D/pack/pack_index.tsv`（27,216 条 + 表头）。
- 这一步按每条 TVFS 条目的原编码规格压缩，2.7 万条互不依赖，会并行跑；`pack_index.tsv` 与数据档必须来自同一次运行。

## 第 3 步：生成覆盖层，并把补充档带进去

```bash
python dm/android/tools/casc_overlay_win.py "$R" "$D/romfs" "$D/work/repl.tsv" \
  --prebuilt "$D/pack/pack_index.tsv"
cp "$D/pack/00-00000008.data" "$D/romfs/data/"
```

- 输出目录必须不存在。工具会复制/链接原 RomFS 的其余文件，并新写一个数据档（3.3.94012 是 `00-00000007.data`，约 3.8 MiB），里面是新的 `.build.config`、vfs-1 和 vfs-root。
- `--prebuilt` 让 27,216 条 TVFS 条目直接指向补充档，**不会把内容再复制进覆盖层的新档**；所以第 3 步末尾的 `cp` 不能漏。漏了的表现是导入正常但贴图缺失或启动报数据档错误。
- 新数据档的编号是按源 RomFS 里第一个空闲位置自动选的，不一定是 07；以命令输出的 `data=` 为准。复制进去的补充档固定叫 `00-00000008.data`（或你第 2 步选的名字）。
- Windows 默认用硬链接，硬链接跨卷或文件系统不支持时会自动退回复制；输出放在与 RomFS 同一个盘最省空间和时间。未开开发者模式时不要显式指定 `--link-mode symlink`；需要强制复制可用 `--link-mode copy`。
- macOS / Linux 仍默认符号链接，iOS 出包脚本靠 `islink` 区分新旧数据档，行为不变。

检查覆盖层：

```bash
ls "$D/romfs/data" | grep -E '00-0000000[78]\.data|\.build\.config'
```

## 第 4 步：按正常 Android 包结构重新汇编

`build_package.py assemble` 直接接收覆盖层 RomFS，不需要先把整个 RomFS 复制成另一种结构：

```bash
python dm/android/tools/build_package.py assemble "$D/d2r-package-astc.zip" \
  --game "$REUSE/game" \
  --romfs "$D/romfs" \
  --shaders "$REUSE/shaders/spirv" \
  --derived "$REUSE/derived.tsv" \
  --note "ASTC 4x4 offline pack + subgroup16 for Mali"
```

- `--shaders` 目录里的 `host-subgroup-size.txt` 必须已经是 16。没有 16 档着色器时，先按 [Android 构建](ANDROID_BUILD.zh-CN.md) 的 `build_shaders.py` 命令加 `--host-subgroup-size 16` 重翻。
- `--derived` 用正常流程生成的 `derived.tsv` 即可；换贴图不影响这三张表。若手头没有，先用正常 `make_package.py` 走一遍生成 `game/`、`shaders/spirv/` 和 `derived.tsv`。
- 这条命令只读覆盖层和复用目录，不碰原 RomFS；输出约 9.8 GiB（3.3.94012 实测 romfs 48 个档案、shaders 20,795 个、derived 756 行）。

检查包元信息：

```bash
python dm/android/tools/build_package.py inspect "$D/d2r-package-astc.zip"
```

确认 `"format": 1`、`"subgroup_size": 16`，RomFS 档案数比普通包多 2（覆盖层新档 + 补充档）。

## 和设备上的表现对照

- Mali（BC=0、ASTC=1）启动日志应有 `compressed texture support BC=0 ASTC=1`、`shader subgroup profile=16 matches`；BC3 贴图走 ASTC 直传，剩余 BC1/4/5/6H 在运行时解成非压缩格式；ASTC→RGBA8 的纹理复制会打印 `astc_to_rgba_copy`。
- Adreno 等支持 BC 的设备请用正常包。这份 ASTC 包的格式转换是**强制**的：即使设备同时支持 BC 和 ASTC，这 2.7 万张贴图也会按 ASTC 上传；能用，但没必要，而且白背了 1 GB 补充档。
- 数据包和 APK 的分组档位必须一致；16 档的包配到选中 64 的设备会在启动检查处直接拒绝。

## 注意

- 不要对 Android 流程执行 `astc_pack.py finalize`：那是 iOS「Legacy Device Pack」的 `pack.json`/手机文件夹步骤，Android 包的清单由 `build_package.py` 生成。
- 不要在仓库里保存转码产物、补充档、密钥或最终 zip；`astcenc` 也只放在本机。
- 3.3.94012 的纹理路径、数量和格式码是按当前游戏文件实测的；换版本后以 `summary.txt` 和 `quality.tsv` 为准，并重新生成 `repl.tsv`。

# Android 构建与部署

English: [README.md](ANDROID_BUILD.md)。实际状态见[源码冻结说明](ANDROID_RELEASE_SNAPSHOT.zh-CN.md)。

## 环境

构建脚本当前面向 Linux x86_64，使用 Android NDK r28c、API35平台/构建工具和 JDK，无需 Gradle。工具默认安装至 `~/dm-work/tools/android`，可用 `DM_ANDROID_TOOLS` 指定。

```bash
python3 dm/android/tools/bootstrap.py
python3 -m venv ~/dm-work/tools/android-venv
source ~/dm-work/tools/android-venv/bin/activate
python -m pip install lz4 capstone unicorn numpy pillow zstandard nsz
```

工具首次下载需要网络。游戏内容、提取工具及 .NET 着色器翻译环境需自行准备，仓库不是开箱即玩的游戏发行包。已有测试/诊断代码仍随源码保留；此次整理不要求执行测试。

## 准备游戏模块和着色器

先按[原项目资源说明](GAME_FILES.zh-CN.md)准备自己的合法游戏文件。已有提取工具支持本体与更新合并：

```bash
python dm/android/tools/extract_packages.py BASE.nsp UPDATE.nsp PROD.keys   ~/dm-work/data/switch/3.3.94012/Update --hactool ~/dm-work/tools/hactool/hactool
```

`BASE.nsp`、`UPDATE.nsp`、`PROD.keys` 是本地路径占位符，不随仓库提供。随后对 main、sdk、subsdk0 分别按同一规则改写：

```bash
for module in main sdk subsdk0; do
  DM_TARGET=android DM_RULESET=touch-run-v4 python dm/tools/patch_nso.py     "$HOME/dm-work/data/switch/3.3.94012/Update/ExeFS/$module"     "$HOME/dm-work/build/android/game" "$module"
done
```

仅当输入确实已经应用同一登记补丁时才使用 `DM_ALLOW_PREPATCHED=1`。着色器转换需要 .NET 10 SDK（默认 `~/dm-work/tools/dotnet/dotnet`，可用官方 dotnet-install 脚本装到该路径；首次 `dotnet restore` 需要网络）：

```bash
python dm/android/tools/build_shaders.py   ~/dm-work/data/switch/3.3.94012/Update/RomFS   ~/dm-work/data/switch/3.3.94012/Update/ExeFS/main   ~/dm-work/build/android/shaders --host-subgroup-size 64
```

这里的64对应本次已验证设备配置，其他GPU必须核对实际能力。转换结果在输出目录的 `spirv/`。工具也支持 `--dotnet` / `--translator` 指定环境；详细参数由 `--help` 查看。

## 构建与安装

首次在干净源码目录构建前，需要从本地 RomFS 生成仓库、经验和场景光照表。激活上述 Python 虚拟环境后运行（`ROMFS` 指向自己的资源目录）：

```bash
make -C dm/host dm_vault_tab.h dm_hud_exp.h dm_bright_levels.inc \
  ROMFS="$HOME/dm-work/data/switch/3.3.94012/Update/RomFS"
```

这些生成文件被 Git 忽略，不随源码发布。更换游戏资源后需删除这三个生成文件，再运行上面的命令重新生成。

```bash
python dm/android/tools/build.py --game-run   --game-source ~/dm-work/build/android/game   --shader-cache ~/dm-work/build/android/shaders/spirv
```

输出 `dm/android/build/D2R-Android-port.apk`，包名 `org.d2r.android.port`。真实游戏APK包含本机派生的游戏模块，不随源码发布。

```bash
export ANDROID_SERIAL='你的设备序列号'
adb -s "$ANDROID_SERIAL" install -r dm/android/build/D2R-Android-port.apk
adb -s "$ANDROID_SERIAL" shell mkdir -p /sdcard/Android/data/org.d2r.android.port/files/romfs
adb -s "$ANDROID_SERIAL" push ~/dm-work/data/switch/3.3.94012/Update/RomFS/. /sdcard/Android/data/org.d2r.android.port/files/romfs/
adb -s "$ANDROID_SERIAL" push ~/dm-work/build/android/shaders/spirv /sdcard/Android/data/org.d2r.android.port/files/shaders
adb -s "$ANDROID_SERIAL" shell am start -n org.d2r.android.port/.D2RActivity
```

RomFS和着色器仅首次或资源版本变化时部署；更新APK保留应用数据。首次无存档会进入建角界面。存档位于应用私有 `files/save/`，不要用其他设备存档覆盖现有角色。驱动与启动配置见[驱动说明](ANDROID_GPU_DRIVER.zh-CN.md)和[启动设置](ANDROID_LAUNCH_SETTINGS.zh-CN.md)。

## 输入与日志

支持虚拟摇杆、按键、系统键盘命名、键盘和手柄。画面按16:9等比适配，手机两侧可有留白。调试浮层显示FPS、CPU/GPU时间和等待，GPU时间是批次时间跨度，不能与CPU简单相加。

```bash
adb -s "$ANDROID_SERIAL" shell run-as org.d2r.android.port tail -100 files/game.log
```

管线缓存位于应用私有 `files/pipeline-cache/`，与存档分离；当前仅验证首次保存，第二次启动加载与收益待验证。

不带 `--game-run` / `--game-source` 的 `build.py` 构建运行时诊断APK，不能以该诊断画面当作真实游戏运行证据。

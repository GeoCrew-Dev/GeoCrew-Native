# 同勘 GeoCrew 原生组件

同勘 GeoCrew 用到的第三方原生库（GDAL、PROJ、GEOS 等）在各平台上的编译与发布。安卓客户端和服务端都从这里取编好的库，保证两端版本一致、计算结果一致。

> 状态：实验。安卓 arm64-v8a / x86_64 的 GDAL、PROJ、GEOS 已能编出并打成 AAR，只在模拟器上验证过，还没上真机。

## 做法

本仓库**不存放第三方源码**，只放：

- **构建工作流**（`.github/workflows/`）：按平台矩阵编译，产出各端的二进制。编译结果做缓存，没改动的库不重复编译。
- **依赖清单**：用 [vcpkg](https://github.com/microsoft/vcpkg) 清单模式固定每个库的版本。源码在构建时按固定版本下载，并校验哈希。
- **补丁**：某个平台需要改源码时，用 diff 补丁（vcpkg 的 overlay port）。每个补丁都要写明原因和对应的上游议题；能提交给上游的尽量提交，升级版本时逐个检查还需不需要。
- **我们自己的封装**：安卓的 JNI 接口与 Kotlin 封装。这里只有我们自己写的代码。服务端的 GDAL 辅助程序放在主仓库的 `server/` 里，链接这里编出的静态库；同步核心保持纯 Go。

## 约束

- GDAL 锁定 3.12 及以上，因为算法注册表的 C 接口从 3.12 起才有。
- vcpkg 的特性清单排除 GPL 依赖（如 poppler），CI 检查许可证。
- 安卓上 GEOS 和 GNU libiconv（都是 LGPL）编成单独的 .so，满足 LGPL 允许用户替换重新链接的要求；CI 会检查。
- 安卓自带的 iconv 只能转 UTF-8 / UTF-16，读不了 GBK 编码的 Shapefile，所以要带上 GNU libiconv。
- 最低 API 31（安卓 12）；库按 16 KB 页对齐，安卓 15 起的 16 KB 页设备才能加载。

## 目标平台

| 用途 | 平台 |
| --- | --- |
| 安卓客户端 | arm64-v8a，以及给模拟器用的 x86_64 |
| 服务端 | Linux x86_64 / arm64、Windows x86_64、macOS arm64 / x86_64；静态链接，单文件部署 |

不支持任何 32 位平台。

## 组件

| 组件 | 用途 |
| --- | --- |
| GDAL | 数据格式读写、矢量与栅格处理算法 |
| PROJ | 坐标系与坐标转换 |
| GEOS | 几何运算 |
| SQLite | GeoPackage 存储 |
| GNU libiconv | Shapefile 等格式的字符编码转换（GBK 等） |
| QuickJS | 插件引擎（待技术验证） |
| llama.cpp / MNN | 手机端模型 GeoCrew Nugget（同勘灵宝），以后再加 |

## 目录

| 路径 | 内容 |
| --- | --- |
| `vcpkg.json`、`vcpkg-configuration.json` | 依赖清单；`builtin-baseline` 固定所有库的版本 |
| `triplets/` | 安卓的 vcpkg 三元组（API 31、只编 Release、16 KB 对齐、LGPL 库编成 .so） |
| `src/` | JNI 封装，编成 `libgeocrew_native.so` |
| `bench/` | 在设备上跑的检查与测速程序，见 [bench/README.md](bench/README.md) |
| `android/` | Gradle 工程：把编好的库打成 AAR（`geocrew-native`），以及量 APK 体积用的示例应用（`sample`） |
| `scripts/` | 整理产物、检查许可证和 16 KB 对齐、在设备上跑测速 |

## 本地构建

需要：

- vcpkg，检出到 `vcpkg.json` 里 `builtin-baseline` 的那个提交，并设好 `VCPKG_ROOT`；
- Android NDK r27（27.3.13750724），设好 `ANDROID_NDK_HOME`；
- CMake ≥ 3.25 和 Ninja；打 AAR 还要 JDK 17 及以上。

```sh
cmake --preset arm64-android          # 第一次会编所有依赖，较慢
cmake --build --preset arm64-android
cmake --preset x64-android && cmake --build --preset x64-android
scripts/stage-android.sh              # 剥离符号，整理到 build/android/
scripts/check-android-libs.sh         # 16 KB 对齐检查和体积表
cd android && ./gradlew :geocrew-native:assembleRelease :sample:assembleRelease
```

在 Windows 上也能编，GNU libiconv 由 vcpkg 自动下载 MSYS2 来编，configure 一步比较慢。

## 发布

按版本号发布，每个版本对应一组固定的库版本（例如 1.0.0 = GDAL 3.x + PROJ 9.x + GEOS 3.x）。安卓客户端和服务端各自锁定要用的版本。每次发布附带：

- 安卓库（AAR）；
- 服务端各平台的静态库和头文件；
- 版本清单（每个库的版本和补丁列表）与 `SHA256SUMS`；
- 全部第三方许可证说明。GEOS 等 LGPL 组件按许可证要求提供源码的获取方式。

## 许可

本仓库的构建脚本、补丁和封装代码采用 Apache-2.0，第三方组件各自遵循其许可证。

# GeoCrew-Native：给 AI 代理的工作规则

本仓库编译同勘 GeoCrew 用到的第三方原生库（GDAL、PROJ、GEOS 等），产出安卓 AAR，以后还有服务端静态库。总的规则以主仓库 [GeoCrew/CLAUDE.md](https://github.com/GeoCrew-Dev/GeoCrew/blob/main/CLAUDE.md) 为准，相关决定见主仓库的 ADR 0003（GIS 引擎）和 [许可证清单](https://github.com/GeoCrew-Dev/GeoCrew/blob/main/docs/licenses.md)。

## 规则

- **不放第三方源码**。要改源码就在 `ports/` 下做 overlay port，用 diff 补丁，并写明原因和对应的上游议题。
- **版本只由 `vcpkg.json` 的 `builtin-baseline` 固定**，不要再加 `version>=` 之类的约束：它会让 vcpkg 去取旧版本的 port，本机的 vcpkg 是部分克隆，会因为取不到而失败。升级时同时改 `builtin-baseline` 和 `.github/workflows/android.yml` 里的 `VCPKG_COMMIT`。
- **改三元组（`triplets/`）会让所有库全部重编**，因为三元组文件的内容算在每个包的哈希里。
- **许可证**：不能引入 GPL 依赖；LGPL 的库（GEOS、GNU libiconv）必须编成单独的 .so。`scripts/check-licenses.py` 在 CI 里检查。新增依赖前先更新主仓库的许可证清单。
- **安卓的库必须按 16 KB 页对齐**，`scripts/check-android-libs.sh` 检查。
- **README 只写验证过的事**，只在模拟器上跑过的标"实验"。
- **流程**：开 PR，PR 由 AI 评审，CI 通过后合并。提交信息用英文 Conventional Commits，文档用中文。
  - main 受保护：必须走 PR，`native (arm64-android)`、`native (x64-android)`、`aar` 三个检查必须通过。
  - 评审改完后用 `gh pr merge <N> --auto --rebase` 打开自动合并：CI 一通过就按 rebase 合并，保留每个提交。
- **不提交**密钥和本机特有的设置（代理地址、`local.properties` 等）。

## 怎么构建和测试

见 [README.md](README.md) 的"本地构建"。在模拟器或手机上跑检查与测速：

```sh
cmake --build --preset x64-android
scripts/stage-android.sh x64-android
scripts/run-bench.sh x64-android
```

AAR 的仪器测试（要连着设备）：先 `scripts/stage-android.sh`，再在 `android/` 下运行 `./gradlew :geocrew-native:connectedAndroidTest`。

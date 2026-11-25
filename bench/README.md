# geocrew_bench

在手机或模拟器上检查并测速 GDAL、PROJ、GEOS：

- GeoPackage 写入、读取 10 万个点，按范围查询（走空间索引），SQLite 数学函数；
- PROJ 把 10 万个点从 CGCS2000 经纬度（EPSG:4490）转到 3 度带高斯-克吕格（EPSG:4547）；
- GEOS 缓冲区、合并；
- 用 GBK 编码写 Shapefile 再读回，检查磁盘上的字节确实是 GBK；
- GeoTIFF 写入，重投影到 Web 墨卡托；
- GDAL 3.12 的算法注册表（`gdal vector info`）。

每项检查失败都会打印 `FAIL`，退出码是失败的项数。

## 运行

先按仓库 README 编好对应平台，再连上设备（`adb devices` 能看到），然后：

```sh
cmake --build --preset x64-android      # 模拟器；真机用 arm64-android
scripts/stage-android.sh x64-android
scripts/run-bench.sh x64-android
```

`run-bench.sh` 把剥离过符号的程序、整理好的动态库推到设备的 `/data/local/tmp/geocrew-bench/` 下，`proj.db` 和 GDAL 的数据文件放在其中的 `data/`，再运行。

模拟器跑在电脑的 CPU 上，测出的时间只能看相对快慢；手机上的真实速度要用 arm64 真机测。

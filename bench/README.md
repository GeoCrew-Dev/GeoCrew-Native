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

CI 的 `emulator (API 31)` 任务在 API 31 的 x86_64 模拟器上跑测速程序、存储测试（私有目录和 /sdcard）和 AAR 的仪器测试，见 `scripts/ci-device-tests.sh`。

# geocrew_storage

测 GeoPackage 放在不同位置时的速度和崩溃恢复，用来决定工程文件放在哪里：/sdcard（经过 FUSE）、应用私有目录、外置存储卡。每个目录分别用 SQLite 的 DELETE 和 WAL 两种日志模式测：

- 一个事务写 10 万个点（新建文件，GDAL 对新建文件默认不做 fsync）；超过 2 分钟就停止插入并报告速率（只限插入，不含提交和建空间索引）；
- 重新打开后逐条提交 1000 个点，像野外每定一个点就保存一次（SQLite 默认每次提交都 fsync）；
- 读出全部要素；
- 崩溃恢复：子进程不停地每 100 个点提交一次，在随机时刻被 `kill -9`，共 20 轮。每轮之后重新打开文件，检查 `PRAGMA integrity_check`、空间索引的条数与数据一致，并且已提交的都在、没提交的都不在。

杀进程只能模拟应用崩溃，模拟不了断电。

```sh
scripts/run-bench.sh x64-android geocrew_storage /data/local/tmp/geocrew-bench/private /sdcard/GeoCrew-spike
```

参数是设备上的目录，测完会删掉测试文件，但不删目录。

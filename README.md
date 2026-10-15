# 稀疏文件断点续传演示程序

## 1. 作业说明

本项目在 Linux 上用 C 语言实现“文件 I/O 断点续传”核心演示：把一个本地源文件逐块写入目标文件。程序先用 `ftruncate()` 设置目标文件的逻辑长度，因此未写入区间是稀疏空洞；每次写入都定位到保存的偏移，使用 `lseek() + write()`；写完一块并调用 `fdatasync()` 后才原子更新断点。按 `Ctrl-C` 时，程序完成当前数据块并保存偏移，下一次运行同一命令即可续传。

本程序以本地源文件模拟“下载到本机”的数据来源，重点演示文件偏移、稀疏空间和断点一致性；网络请求和服务器交互不在本次演示范围内。

### 作业功能对应

| 要求 | 程序中的实现 | 可查看的结果 |
| --- | --- | --- |
| `create`：创建至少 128 KiB 的空洞文件 | 检查源文件大小，并用 `ftruncate()` 设置目标逻辑长度 | 中断时逻辑大小 1 MiB、实际占块 64 KiB |
| `write`：按偏移写入 | 每块调用 `lseek()`、`write()`，处理短写 | 中断时剩余区域仍为空洞；续传后 `cmp` 一致 |
| 中断后继续 | `SIGINT` 设置标志，主循环同步数据并保存 `.state` 偏移 | 从 65,536 B 恢复 |
| `status`：显示占用情况 | 在中断和完成时打印逻辑大小、实际占块、空洞比例 | `evidence/transfer.log` 与 `resume.log` |

在板端快速复现：

```bash
make clean && make
./tests/run_demo.sh
```

脚本运行结束时应显示 `ALL DEMO CHECKS PASSED`；完整实测记录位于 `evidence/`。

## 2. 实验环境与边界

- 实际测试板：Radxa ROCK 5C，RK3588 平台，aarch64，Debian GNU/Linux 12 (bookworm)
- 编译环境：GCC 12.2、GNU Make 4.3
- AI 协作：由 Codex 辅助分析、实现和代码核查；五轮需求与判断、三处重点审查见 `AI_COLLABORATION.md`

## 3. 文件清单

- `sparse_downloader.c`：主程序，包含重要步骤的中文注释
- `Makefile`：编译与清理规则
- `tests/run_demo.sh`：在板端生成测试数据、执行中断/续传/拒绝覆盖测试并校验结果
- `AI_COLLABORATION.md`：五轮协作要点、代码审查与板端结果判断
- `evidence/`：板端实际运行的终端记录、环境信息和 `board-test-screenshot.png` 截图

## 4. 编译

```bash
make
```

清理可执行文件：

```bash
make clean
```

## 5. 运行

先准备一个至少 128 KiB 的普通源文件。示例创建 1 MiB 测试数据：

```bash
python3 - <<'PY'
from pathlib import Path
size = 1024 * 1024
Path("source.bin").write_bytes(bytes((i * 37 + 11) % 256 for i in range(size)))
PY
```

开始传输：

```bash
./sparse_downloader source.bin downloaded.bin
```

程序创建 `downloaded.bin` 和 `downloaded.bin.state`。目标已存在且断点有效时会从断点继续；已有目标但没有有效断点时程序会拒绝覆盖。不要手工删除 `.state` 后期待自动接续；如果要重新开始，应同时确认备份后删除目标文件和断点文件。

### 中断与续传

运行较慢的演示，便于按 `Ctrl-C`：

```bash
./sparse_downloader source.bin downloaded.bin --chunk-size 4096 --delay-ms 30
```

按 `Ctrl-C` 后会输出已完成偏移、逻辑大小、实际占块和空洞比例。再次执行同一命令，即从保存的偏移继续。`--delay-ms` 只用于演示，正常运行可省略。

## 6. 查看稀疏文件分配

程序内的占用值来自 `stat.st_blocks × 512`，空洞比例按 `1 - 实际占块 / 逻辑大小` 估算。文件系统会按块分配空间，数值可能有小幅舍入差异。

也可自行核对：

```bash
stat -c 'logical=%s bytes; allocated=%b blocks (512-byte units)' downloaded.bin
du -h downloaded.bin
du -h --apparent-size downloaded.bin
```

在传输未完成时，目标文件的逻辑大小已经是源文件大小，而未写入的尾部仍是空洞；传输完成后，目标文件内容完整，空洞比例可能接近 0%，这是正常现象。初次创建时文件系统也可能采用延迟分配，因此需以板端 `stat` 实测为准。

## 7. 断点保存顺序

1. 通过源文件偏移读取一个数据块。
2. 对目标文件 `lseek(fd, offset, SEEK_SET)`，然后循环 `write()` 直到整块写完。
3. 调用 `fdatasync()` 确保该块数据先于断点落盘。
4. 把新偏移写入临时状态文件并 `fsync()`，再用 `rename()` 原子替换正式状态文件。
5. 下次启动时检查源文件身份/大小/修改时间，再从记录的偏移续传。

这样即使程序在保存断点过程中退出，最多重写最后一个尚未登记完成的块，不会把断点推进到尚未同步的数据之后。信号处理函数只设置标志位，不在异步信号上下文中执行文件操作。

## 8. 当前限制

- 本程序演示文件 I/O，不连接网络或 URL，也不模拟服务器响应。
- 状态文件以源文件设备号、inode、长度和纳秒修改时间识别源文件；源文件变化后会停止，需重新开始。
- 没有实现 SHA-256 等端到端内容校验；演示脚本使用 `cmp` 对照源文件与结果。
- 文件系统的稀疏分配策略不同，实际占块数以本板实测为准。
- 截图来自解锁后的 TigerVNC 桌面中实际展示板端测试结果的终端窗口。


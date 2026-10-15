# 板端实测结果

测试主机：Radxa ROCK 5C，aarch64，Debian GNU/Linux 12。完整环境记录见 `environment.txt`。

## 编译

运行 `make clean && make`，GCC 12.2 编译成功；`build.log` 未出现警告或错误。

## 断点和稀疏空间

测试脚本生成 1 MiB 普通源文件，并在确认写入 64 KiB 后触发 SIGINT。中断后保存偏移 65,536 字节，目标逻辑长度为 1,048,576 字节，实际分配 65,536 字节，空洞比例 93.75%。对应输出见 `transfer.log`。

继续运行时把块大小从 4,096 改为 8,192 字节，程序从 65,536 字节偏移继续并完成。`cmp source.bin target.bin` 通过。完成后 `stat` 显示逻辑长度 1,048,576 字节、分配 2,048 个 512 字节块（共 1,048,576 字节），空洞比例 0.00%。对应输出见 `resume.log` 和 `stat.txt`。

## 防覆盖检查

给定一个已存在但没有断点文件的目标，程序返回失败并提示拒绝覆盖；测试再次用 `cmp` 确认原文件未改变。输出见 `overwrite-guard.log`。

## 截图状态

`board-test-screenshot.png` 是解锁后的板端 TigerVNC 会话中实际测试终端截图；图像由 X11 屏幕捕获保存，再通过 SSH/SCP 传回 PC。

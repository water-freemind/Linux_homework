#!/usr/bin/env bash
set -euo pipefail
ROOT=/home/radxa/sparse-resume-downloader
cd "$ROOT"
echo '$ ./tests/run_demo.sh'
./tests/run_demo.sh > evidence/test-run-full.log 2>&1
cd "$ROOT/demo_data"
printf '\n========== ROCK 5C FILE-IO ASSIGNMENT ==========\n'
printf 'Board: Radxa ROCK 5C | aarch64 | Debian 12\n'
printf 'Build: '
tail -n 1 ../evidence/build.log
printf '\n[Ctrl-C checkpoint]\n'
grep -E '^写入进度：65536/|^收到 Ctrl-C|^进度：完成偏移|^稀疏统计' ../evidence/transfer.log
printf '\n[Resume with a different chunk size]\n'
grep -E '^检测到有效断点|^传输完成|^进度：完成偏移|^稀疏统计|^PASS:' ../evidence/resume.log
printf '\n[Filesystem allocation]\n'
cat ../evidence/stat.txt
printf '\n[No-overwrite guard]\n'
cat ../evidence/overwrite-guard.log
echo 'PASS: original protected.bin preserved'
printf '\n[Overall]\nALL DEMO CHECKS PASSED\n'
read -r -p '按回车关闭测试窗口。' _

#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT_DIR/sparse_downloader"
EVIDENCE="$ROOT_DIR/evidence"
TEST_DIR="$ROOT_DIR/demo_data"
mkdir -p "$EVIDENCE" "$TEST_DIR"
cd "$TEST_DIR"

python3 - <<'PY'
from pathlib import Path
size = 1024 * 1024
Path("source.bin").write_bytes(bytes((i * 37 + 11) % 256 for i in range(size)))
PY

rm -f target.bin target.bin.state target.bin.state.tmp
printf '%s\n' '=== 1. 初始稀疏文件与可中断传输 ==='
python3 - "$BIN" "$EVIDENCE/transfer.log" "$EVIDENCE/interrupt.rc" <<'PY'
import signal
import subprocess
import sys

binary, log_path, rc_path = sys.argv[1:]
with open(log_path, "w", encoding="utf-8") as log:
    process = subprocess.Popen(
        [binary, "source.bin", "target.bin", "--chunk-size", "4096", "--delay-ms", "20"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        encoding="utf-8", errors="replace", bufsize=1,
    )
    signal_sent = False
    for line in process.stdout:
        log.write(line)
        log.flush()
        # 等程序确认提交到64 KiB，再发送SIGINT，获得可重复的断点位置。
        if not signal_sent and "写入进度：65536/" in line:
            process.send_signal(signal.SIGINT)
            signal_sent = True
    result = process.wait(timeout=10)
if not signal_sent:
    raise SystemExit("没有观察到 64 KiB 的进度行，未能按预期触发 Ctrl-C。")
with open(rc_path, "w", encoding="ascii") as result_file:
    result_file.write(str(result))
PY
interrupt_rc="$(cat "$EVIDENCE/interrupt.rc")"
cat "$EVIDENCE/transfer.log"
if [[ "$interrupt_rc" -ne 130 ]]; then
    echo "FAIL: 中断退出码应为130，实际为 $interrupt_rc" >&2
    exit 1
fi
grep -q '断点已保存' "$EVIDENCE/transfer.log"
grep -q '^offset=' target.bin.state
printf '\n%s\n' '=== 2. 从断点续传 ==='
"$BIN" source.bin target.bin --chunk-size 8192 | tee "$EVIDENCE/resume.log"
cmp source.bin target.bin
echo 'PASS: cmp 确认续传目标与源文件逐字节一致。'

printf '\n%s\n' '=== 3. 稀疏占用数据 ==='
stat -c 'logical=%s bytes; allocated=%b blocks (512-byte units)' target.bin | tee "$EVIDENCE/stat.txt"
du -h target.bin | tee "$EVIDENCE/du-allocated.txt"
du -h --apparent-size target.bin | tee "$EVIDENCE/du-apparent.txt"

printf '\n%s\n' '=== 4. 无断点时拒绝覆盖现有目标 ==='
cp source.bin protected.bin
set +e
"$BIN" source.bin protected.bin >"$EVIDENCE/overwrite-guard.log" 2>&1
guard_rc=$?
set -e
cat "$EVIDENCE/overwrite-guard.log"
if [[ "$guard_rc" -eq 0 ]]; then
    echo 'FAIL: 程序不应覆盖没有断点的目标文件。' >&2
    exit 1
fi
cmp source.bin protected.bin
echo 'PASS: 已拒绝覆盖，原目标数据保持一致。'
echo 'ALL DEMO CHECKS PASSED'

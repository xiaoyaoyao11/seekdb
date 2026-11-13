#!/usr/bin/env bash
# 赛题一：全文索引调优 —— 基准测试执行脚本
#
# 用法:
#   bash run_fts_tuning_bench.sh <host> <port> <user> [password]
#
# 环境变量:
#   BENCH_ROWS          语料行数，默认 50000
#   BENCH_REPEAT        查询重复轮数，默认 1
#   BENCH_OUT           输出文件，默认 fts_bench_$(date +%Y%m%d_%H%M%S).log
#
# 在「基线版本」与「优化版本」上分别执行本脚本，diff 两份日志即可得到
# 索引构建 / 分词写入 / 查询三个维度的耗时对比。

set -euo pipefail

HOST=${1:-127.0.0.1}
PORT=${2:-2881}
USER=${3:-root}
PASSWORD=${4:-}
ROWS=${BENCH_ROWS:-50000}
REPEAT=${BENCH_REPEAT:-1}
OUT=${BENCH_OUT:-fts_bench_$(date +%Y%m%d_%H%M%S).log}

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SQL_FILE=${HERE}/fts_tuning_bench.sql

if [ ! -f "${SQL_FILE}" ]; then
  echo "bench sql not found: ${SQL_FILE}" >&2
  exit 1
fi

# 按 BENCH_ROWS 调整语料行数
TMP_SQL=$(mktemp)
trap 'rm -f "${TMP_SQL}"' EXIT
sed "s/where n < 50000/where n < ${ROWS}/" "${SQL_FILE}" > "${TMP_SQL}"

MYSQL_ARGS=(--host="${HOST}" --port="${PORT}" --user="${USER}" --default-character-set=utf8mb4)
if [ -n "${PASSWORD}" ]; then
  MYSQL_ARGS+=(--password="${PASSWORD}")
fi

echo "== 全文索引基准测试 =="
echo "host=${HOST} port=${PORT} user=${USER} rows=${ROWS} repeat=${REPEAT}"
echo "输出文件: ${OUT}"
echo

for i in $(seq 1 "${REPEAT}"); do
  echo "---- 第 ${i}/${REPEAT} 轮 ----" | tee -a "${OUT}"
  # -t 打印每条语句耗时，便于逐项对比
  mysql "${MYSQL_ARGS[@]}" < "${TMP_SQL}" 2>&1 | tee -a "${OUT}"
  echo "" | tee -a "${OUT}"
done

echo "完成，结果见 ${OUT}"

#!/usr/bin/env bash
# 把本地改动提交并推送到 fork（fengs2021/xiaozhi-esp32）
# 用法: ./sync-fork.sh "提交说明"
set -e
cd "$(dirname "$0")"
MSG="${1:-chore: 同步本地改动}"

export http_proxy=http://127.0.0.1:7890
export https_proxy=http://127.0.0.1:7890

git add -A main/ scripts/ docs/
git diff --cached --quiet && { echo "没有需要提交的改动"; exit 0; }

git commit -m "$MSG"
git push fork main
echo "已推送到 https://github.com/fengs2021/xiaozhi-esp32"

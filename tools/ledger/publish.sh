#!/usr/bin/env bash
# ledger와 검증 스크립트를 기록 전용 공개 저장소에 올린다. 자동으로 실행되지 않는다.
#
#   LEDGER_REPO=~/holdem-ledger tools/ledger/publish.sh [--dry-run]
#
#   LEDGER_REPO  기록 전용 저장소를 clone한 폴더 (필수)
#   LEDGER_FILE  올릴 ledger (기본값: 지금 폴더의 data/stats-ledger.jsonl)
#
# 올리는 파일은 stats-ledger.jsonl, verify.py, README.md 세 개뿐이다.
# 토큰은 이 스크립트에 적지 않는다 (git의 SSH 키나 credential 설정을 쓴다).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
source_file="${LEDGER_FILE:-data/stats-ledger.jsonl}"
repo="${LEDGER_REPO:?LEDGER_REPO에 기록 전용 저장소 폴더를 지정하세요}"
dry_run=0
case "${1:-}" in
  "") ;;
  --dry-run) dry_run=1 ;;
  *) echo "알 수 없는 옵션: $1" >&2; exit 2 ;;
esac

[ -s "$source_file" ] || { echo "아직 올릴 기록이 없습니다: $source_file"; exit 0; }
[ -d "$repo/.git" ] || { echo "git 저장소가 아닙니다: $repo" >&2; exit 1; }
# 코드 저장소에 잘못 올리지 않게
[ ! -e "$repo/CMakeLists.txt" ] || { echo "코드 저장소가 아니라 기록 전용 저장소를 지정하세요." >&2; exit 1; }

# 서버가 쓰는 동안 바뀌지 않게 사본을 만들어 검사하고, 그 사본을 올린다
copy="$(mktemp)"
trap 'rm -f "$copy"' EXIT
cp "$source_file" "$copy"
python3 "$here/verify.py" "$copy"

if [ "$dry_run" = 0 ] && git -C "$repo" rev-parse --verify -q '@{upstream}' >/dev/null; then
  git -C "$repo" pull --ff-only --quiet
fi

# 이미 공개한 줄은 그대로여야 한다 (새 파일의 앞부분이 공개본과 같아야 함)
published="$repo/stats-ledger.jsonl"
if [ -f "$published" ]; then
  size="$(wc -c < "$published" | tr -d ' ')"
  if ! head -c "$size" "$copy" | cmp -s - "$published"; then
    echo "중단: 이미 공개한 기록과 다릅니다. 아무것도 올리지 않았습니다." >&2
    exit 1
  fi
fi

if [ "$dry_run" = 1 ]; then
  before=0
  if [ -f "$published" ]; then before="$(wc -l < "$published" | tr -d ' ')"; fi
  echo "[dry-run] 새로 올라갈 줄:"
  tail -n +"$((before + 1))" "$copy"
  exit 0
fi

cp "$copy" "$published"
chmod 644 "$published"
cp "$here/verify.py" "$here/README.md" "$repo/"
files=(stats-ledger.jsonl verify.py README.md)
git -C "$repo" add -- "${files[@]}"
if ! git -C "$repo" diff --cached --quiet -- "${files[@]}"; then
  # 서버 계정의 이름이나 이메일이 공개되지 않게 고정된 이름으로 남긴다
  GIT_AUTHOR_NAME=holdem-ledger GIT_AUTHOR_EMAIL=ledger@localhost \
  GIT_COMMITTER_NAME=holdem-ledger GIT_COMMITTER_EMAIL=ledger@localhost \
    git -C "$repo" commit --quiet -m "ledger $(date -u +%Y-%m-%d)" -- "${files[@]}"
fi
git -C "$repo" push --quiet -u origin HEAD  # 지난번에 못 올린 커밋도 함께 올라간다
echo "올렸습니다."

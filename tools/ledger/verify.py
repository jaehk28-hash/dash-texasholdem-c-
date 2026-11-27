#!/usr/bin/env python3
"""stats-ledger.jsonl의 해시 체인과 형식을 검증한다.

사용법: python3 verify.py [파일]   (기본값: stats-ledger.jsonl)
맞으면 종료 코드 0, 어긋나면 1. 표준 라이브러리만 쓴다.
"""
import datetime
import hashlib
import json
import re
import sys

NUMBER = r"(?:0|[1-9][0-9]*)"
SOURCES = r"(?:instagram|everytime|kakao|discord|friend|poster|qr|github|other)"
# 이 모양과 한 글자라도 다르면 실패: 날짜, 정해 둔 이름의 숫자, 해시 말고는 아무것도 담을 수 없다.
LINE = re.compile(
    r'\{"date":"[0-9]{4}-[0-9]{2}-[0-9]{2}","counts":\{"visits":N,"unique_visitors":N,"solo_starts":N,'
    r'"room_starts":N,"hands_finished":N,"src":\{(?:"S":N(?:,"S":N)*)?\}\},'
    r'"prev":"(?:genesis|[0-9a-f]{64})","hash":"[0-9a-f]{64}"\}'.replace("N", NUMBER).replace('"S"', '"' + SOURCES + '"')
)


def verify(text):
    """(줄 수, 마지막 hash)를 돌려준다. 어긋나면 ValueError."""
    if text and not text.endswith("\n"):
        raise ValueError("마지막 줄이 잘렸습니다")
    prev, last_date = "genesis", None
    lines = text.split("\n")[:-1]
    for number, line in enumerate(lines, 1):
        if not LINE.fullmatch(line):
            raise ValueError(f"{number}번째 줄: 형식이 다릅니다")
        entry = json.loads(line)
        body = line[: line.rindex(',"hash":"')] + "}"
        if hashlib.sha256(body.encode()).hexdigest() != entry["hash"]:
            raise ValueError(f"{number}번째 줄: 내용과 hash가 다릅니다")
        if entry["prev"] != prev:
            raise ValueError(f"{number}번째 줄: 앞줄과 이어지지 않습니다")
        date = datetime.date.fromisoformat(entry["date"])
        if last_date and date != last_date + datetime.timedelta(days=1):
            raise ValueError(f"{number}번째 줄: 날짜가 하루씩 이어지지 않습니다")
        prev, last_date = entry["hash"], date
    return len(lines), prev


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "stats-ledger.jsonl"
    try:
        with open(path, "rb") as file:
            count, head = verify(file.read().decode("ascii"))
    except (OSError, UnicodeDecodeError, ValueError) as error:
        print(f"실패: {error}")
        return 1
    print(f"확인: {count}줄, 마지막 hash {head}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

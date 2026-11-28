<p align="center"><img src="web/logo.svg" alt="Coupe Holdem 로고" width="180"></p>

<h1 align="center">Coupe Holdem · 쿠페 홀덤</h1>

<p align="center">홀덤 연습방</p>

<p align="center"><b>잔 한 모금처럼 가볍게 시작하는 카드게임</b><br>도박장 분위기 없이, 부담 없이 배우는 텍사스 홀덤. 일단 해보면서 배우는 홀덤!</p>

<p align="center">라이브 데모: <a href="https://holdem.apps.khu-return.com">https://holdem.apps.khu-return.com</a></p>

> **연습용 앱입니다.** 칩은 모두 연습용이고 실제 돈은 오가지 않습니다. 결제, 환전, 현금 베팅 기능은 없습니다.

## 주요 기능

- **혼자 봇과 하기:** 성격이 다른 봇 1~5명. 승률 힌트를 켜고 끌 수 있습니다.
- **친구와 하기:** 6자리 방 코드로 온라인에서 최대 6명이 함께 합니다.
- **처음 하는 사람을 위한 안내:** 그림 규칙, 족보 패널, 용어 "?" 풀이, 지금 상황 설명.
- **내 속도로:** 한 명씩 행동하고, 다음 단계는 직접 눌러 넘깁니다.
- **판 결과와 리뷰:** 순위, 최종 5장, 왜 이겼는지 설명.
- **1인칭 테이블:** 인원수에 맞춰 나뉘는 펠트, 칩 더미와 이동 연출.
- **콘솔 모드:** 터미널에서도 같은 규칙으로 연습할 수 있습니다.

## 스크린샷

<!-- 아래 칸에 그림을 넣으세요. 예: ![시작 화면](docs/screenshots/home.png) -->

| 시작 화면 | 게임 테이블 | 판 결과 |
|---|---|---|
|  |  |  |

## 실행 방법

```bash
git clone https://github.com/jaehk28-hash/dash-texasholdem-c-.git holdem-practice
cd holdem-practice
cmake -S . -B build && cmake --build build -j2
./build/holdem --web        # http://localhost:8080
./build/holdem              # 콘솔 모드
```

옵션, CMake 없이 빌드하는 방법, 서버 배포는 아래에 있습니다.

## 만든 이유

홀덤은 재미있는 카드게임인데, 이름만 들으면 도박장 같은 분위기부터 떠올라 처음 시작하기가 어렵게 느껴집니다.
그래서 부담 없이 혼자서도, 친구들과도 온라인으로 즐길 수 있는 연습방 쿠페 홀덤을 만들었습니다.
로고의 쿠페 잔과 스페이드에는 무거운 판돈 대신 한 잔의 여유처럼 즐기자는 뜻을 담았습니다.
틀려도 괜찮습니다. 일단 해보면서 배우는 홀덤입니다.

## 그림 파일

쿠페 잔 그림은 `web/logo.svg`의 `<symbol id="coupe">` 하나뿐이고(직접 그린 것), 화면은 이 symbol을 가져다 씁니다.
`favicon.svg`, `favicon.ico`, `apple-touch-icon.png`, `og-image.png`(링크 미리보기)는 `node tools/make-icons.mjs`로 만듭니다. 로고를 고치면 다시 실행해 주세요.
영문 로고 글꼴 `web/fonts/coupe-brand.woff2`는 [Bodoni Moda](https://github.com/indestructible-type/Bodoni)(SIL Open Font License 1.1, `web/fonts/OFL.txt`)를 굵기 700으로 고정하고 영문, 숫자, 기호만 남긴 것입니다.
링크 미리보기의 주소(`og:url`, `og:image`)는 `web/index.html`의 `<head>`에 적혀 있으니, 도메인이 바뀌면 함께 고쳐 주세요.

## 빌드

CMake가 있을 때 (권장):

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure   # 테스트 실행
```

CMake가 없을 때는 컴파일러로 바로 빌드할 수 있습니다 (macOS/Linux, zsh/bash 모두 동작):

```bash
mkdir -p build
CORE=(src/card.cpp src/evaluator.cpp src/pot.cpp src/review.cpp src/game.cpp src/equity.cpp src/bots.cpp src/human_player.cpp)
WEB=(src/web_json.cpp src/web_session.cpp src/web_server.cpp src/stats.cpp src/ledger.cpp src/sha256.cpp src/chat.cpp src/bot_talk.cpp)
WEBFLAGS=(-isystem third_party -pthread -DHOLDEM_WEB_DIR="\"$PWD/web\"")
c++ -std=c++17 -O2 -Wall -Wextra -Isrc "${WEBFLAGS[@]}" "${CORE[@]}" "${WEB[@]}" src/main.cpp -o build/holdem
c++ -std=c++17 -O2 -Wall -Wextra -Isrc "${CORE[@]}" src/bench.cpp -o build/holdem_bench
for t in evaluator pot game; do
  c++ -std=c++17 -O2 -Wall -Wextra -Isrc "${CORE[@]}" tests/test_$t.cpp -o build/test_$t && ./build/test_$t
done
c++ -std=c++17 -O2 -Wall -Wextra -Isrc "${WEBFLAGS[@]}" "${CORE[@]}" "${WEB[@]}" tests/test_web.cpp -o build/test_web && ./build/test_web
c++ -std=c++17 -O2 -Wall -Wextra -Isrc "${WEBFLAGS[@]}" "${CORE[@]}" "${WEB[@]}" tests/test_review.cpp -o build/test_review && ./build/test_review
c++ -std=c++17 -O2 -Wall -Wextra -Isrc "${WEBFLAGS[@]}" "${CORE[@]}" "${WEB[@]}" tests/test_server.cpp -o build/test_server && ./build/test_server
```

모든 빌드는 `-Wall -Wextra`로 경고 없이 컴파일됩니다.

## 실행

```bash
./build/holdem                       # 무작위 시드로 시작
./build/holdem --seed 42             # 같은 시드면 같은 카드와 같은 봇 행동 (재현용)
./build/holdem --no-hint             # 승률/팟 오즈 힌트 끄기 (승률 계산도 하지 않음)
./build/holdem --chips 5000 --sb 25 --bb 50
```

| 옵션 | 설명 | 기본값 |
|---|---|---|
| `--seed N` | 난수 시드 | 무작위 |
| `--no-hint` | 힌트 끄기 | 켜짐 |
| `--hint-trials N` | 힌트 승률 계산 시행 횟수 | 10000 |
| `--chips N` | 시작 칩 | 10000 |
| `--sb N`, `--bb N` | 스몰/빅 블라인드 | 50 / 100 |
| `--web` | 웹 모드로 실행 | 꺼짐 (콘솔) |
| `--port N` | 웹 모드 포트 (환경변수 `PORT`로도 지정, `--port`가 우선) | 8080 |

### 내 차례에 입력하는 법

| 입력 | 의미 |
|---|---|
| `f` | 폴드 (콜 비용이 없을 때는 체크하라고 안내) |
| `c` | 체크 또는 콜 (상황에 맞게 자동 선택) |
| `r 300` | 이번 라운드에 **총 300까지** 건다 (벳/레이즈). `r pot`, `r half`도 가능 |
| `a` | 올인 |
| `q` | 종료 |

숫자가 아닌 입력, 음수, 최소 레이즈보다 작은 금액, 가진 칩보다 큰 금액은 안내 문구와 함께 **다시 물어봅니다.**
한 판이 끝나면 `Enter`로 다음 판, `h`로 **핸드 히스토리**(모든 행동과 전원의 홀 카드), `q`로 종료합니다.

내 차례에 나오는 힌트 예:

```
[힌트] 포지션 BB  |  팟 1631  |  콜 비용 377  |  팟 오즈 18.8%
       현재 승률 16.0% (상대 4명이 무작위 카드를 가졌다고 가정, 10000회 시뮬레이션)
       승률(16.0%) < 팟 오즈(18.8%) -> 콜은 손해입니다 ...
```

승률은 "상대가 무작위 카드를 가졌다"는 가정의 값이라, 상대가 베팅을 했다면 실제 상대 핸드는 평균보다 강하다는 점을 감안하세요.

## 웹 모드

```bash
./build/holdem --web                 # http://localhost:8080
./build/holdem --web --port 9000
PORT=9000 ./build/holdem --web
./build/holdem --web --seed 42       # 재현용 (공개 서버에서는 쓰지 마세요)
```

접속하거나 새로고침하면 항상 **시작 화면**이 나옵니다 (친구방에 들어가 있으면 그 방으로 다시 들어갑니다). 규칙, 족보, 이야기는 오른쪽 위 버튼으로 엽니다.
게임 화면의 용어 옆 "?"를 누르면 뜻이 나옵니다.

| 메뉴 | 내용 |
|---|---|
| 혼자 봇과 하기 | 상대 인원(1~5, 기본 3)과 승률 힌트만 고르고 시작. 칩, 블라인드, 속도는 "상세 설정"에 있습니다 |
| 방 만들기 | 위 설정 + 닉네임, 행동 제한 시간, 연장 횟수, 공개 설정, 봇 수(0~5). **6자리 방 코드**가 나옵니다 |
| 코드로 들어가기 | 친구에게 받은 코드와 닉네임을 입력 |

### 친구와 하는 법

1. 모두 **같은 서버 주소**로 접속합니다. 같은 와이파이라면 `http://서버를_켠_컴퓨터의_IP:8080`, 멀리 있는 친구라면 아래 "서버 배포"가 필요합니다.
2. 한 사람이 방을 만들고 코드를 알려 주면, 친구들이 코드를 입력해 들어옵니다 (최대 6명).
3. 방장이 **게임 시작**을 누릅니다. 빈 자리에는 설정한 수만큼(남는 자리 한도) 봇이 앉습니다.

- **행동 제한 시간**은 결정 한 번에 주어지는 시간입니다: 15, 30, 45(기본), 60초, 제한 없음. 시간은 서버가 재고 1초의 여유를 둡니다.
- 시간이 다 되면 **연장**(+30초)이 저절로 한 번 쓰입니다. 연장은 0~3회(기본 2회)이고, 판마다가 아니라 게임을 새로 시작할 때만 다시 찹니다.
- 연장이 없으면 낼 칩이 있을 때는 폴드, 없을 때는 체크합니다.
- 연속 3번 자동 처리되면 **자리 비움**이 되어 그 사람의 차례는 바로 처리됩니다. "자리로 돌아오기"를 누르면 풀립니다.
- 제한 시간이 있는 방은 방장이 "다음"을 누르지 않아도 제한 시간이 지나면 넘어갑니다 (방장이 자리 비움이면 3초).
- 친구 방에는 승률 힌트가 없습니다.

#### 공개 방

- 방을 만들 때 비공개(기본)와 공개 중에 고르고, 방장은 게임 시작 전까지 바꿀 수 있습니다. 공개 방도 코드로 들어올 수 있습니다.
- **JOIN ROOM** 화면의 목록에는 공개 방만 보이고(5초마다 갱신), 방 코드는 나오지 않습니다. 게임 중인 방에는 중간 입장이 없습니다.
- 방장은 대기실에서 참가자를 내보낼 수 있고, 그 브라우저는 그 방에 다시 들어올 수 없습니다. (로그인이 없어, 저장 공간을 지우거나 시크릿 창을 쓰면 막지 못합니다.)
- 접속한 사람이 없는 공개 방은 5분 뒤에 닫습니다. 방 제한은 공개 방에도 같습니다.
- 브라우저마다 1분에 목록 30번, 입장 시도 20번까지입니다. 틀린 코드를 연속 5번 넣으면 1분 동안 막습니다 (IP별로는 1분에 10번).
- 닉네임은 링크·전화번호·이메일 모양과 간단한 욕설 목록을 거릅니다. 채팅이나 방 이름 입력은 없습니다.
- 게임 중에 나가면 그 자리는 자동으로 체크/폴드하며, 다시 들어올 수 없습니다. 새로고침이나 탭을 다시 여는 것은 괜찮습니다.
- 시작한 방에는 새로 들어올 수 없습니다. 게임이 끝나면 방장이 **다시 하기**를 누를 수 있습니다.
- 방장이 나가면 다음으로 들어온 사람이 방장이 됩니다.
- 브라우저 하나가 한 사람입니다(쿠키). 한 컴퓨터에서 두 명을 시험하려면 시크릿 창이나 다른 브라우저를 쓰세요.

### 채팅

- 게임 화면에 채팅 패널이 있습니다. 화면 폭이 1180px 이상이면 오른쪽 패널, 좁으면 아래에서 올라오는 시트입니다.
- **이모지 6개와 정해진 한마디**는 모든 방에서 쓸 수 있고, 3초에 한 번입니다(둘이 함께).
- **자유 입력**은 비공개 친구 방에서만 됩니다. 1~40자, 3초에 한 번이며 10초에 5번 넘게 시도하면 30초 잠깁니다.
- 링크, 전화번호, 이메일 모양은 거절하고, 작은 내장 목록의 욕설은 `***`로 바꿉니다.
- 방장은 자유 입력을 켜고 끄거나, 한 사람을 "채팅 금지"할 수 있습니다. 누구든 이름표를 눌러 한 사람을 자기 화면에서 숨길 수 있습니다.
- 봇이 있는 방에서는 봇이 가끔 반응합니다: 큰 팟을 이기거나 졌을 때, 블러프가 통했을 때, 올인을 마주했을 때, 사람의 이모지·한마디에 답할 때(1~3초 뒤). 봇 한 명은 한 판에 한 번, 봇 전체는 8초에 한 번까지입니다. "효과 줄이기"를 켠 사람에게는 보내지 않습니다.
- 최근 50개만 서버 메모리에 두고 방과 함께 사라집니다. 파일이나 로그에는 남기지 않고, 통계에는 사용한 방 수와 보낸 횟수만 셉니다.

### 공통

- `--chips`, `--sb`, `--bb`는 설정 화면의 기본값이 되고, `--no-hint`는 혼자 하기의 힌트를 끕니다.
- 방은 개수와 인원에 제한이 있고, 오래 쓰지 않으면 정리됩니다 (아래 "방 제한"). 서버를 끄면 모두 사라집니다.
- 봇은 한 명씩 "생각 중" → 행동 공개 순서로 움직입니다. 진행 속도는 느리게(5초, 기본), 보통(3초), 빠르게(1초) 중에서 고르며 게임 중에도 바꿀 수 있습니다.
  - 속도 값은 `src/web_session.h`의 `AI_ACTION_MS` 하나로 바꿉니다 (보통은 3/5, 빠르게는 1/5).
- 플랍, 턴, 리버, 쇼다운, 다음 판은 **방장이 "다음" 버튼(스페이스/Enter)을 눌러야** 넘어갑니다.
- 모두 폴드했거나 올인한 뒤에는 "남은 진행 빠르게 보기"로 이번 판의 남은 단계를 자동 진행할 수 있습니다.
- 판이 끝나면 이번 판 순위, 현재 칩 순위(변동 표시), 핸드 리뷰를 보여 줍니다.
- 남의 홀 카드는 쇼다운에서 폴드하지 않은 사람 것만 서버가 보냅니다.
- `--seed N`을 주면 만든 방 순서대로 N, N+1, N+2 … 시드를 씁니다. 시드를 아는 사람은 카드를 미리 알 수 있습니다.
- 화면 파일은 `web/index.html`입니다. 서버를 시작할 때 한 번 읽으므로, 고친 뒤에는 다시 실행하세요.

### API

| 요청 | 본문 (JSON) |
|---|---|
| `POST /api/new` | 혼자 하기. 모두 선택: `chips, sb, bb, bots, hint` |
| `POST /api/room/create` | `name` 필수, 선택: `chips, sb, bb, bots, turnSeconds`(0/15/30/45/60), `extensions`(0~3), `public`(true/false) |
| `GET /api/rooms/public` | 공개 방 목록 `{rooms:[{id,name,players,max,playing,turnSeconds,chips,bots}]}`. 방 코드는 없음 |
| `POST /api/room/visibility` | 방장만, 시작 전. `public` |
| `POST /api/room/kick` | 방장만, 대기실에서. `name` |
| `POST /api/back` | 자리 비움에서 돌아오기 |
| `POST /api/room/join` | `name`과 `code` 또는 `id`(공개 방). 없는 코드를 1분에 10번 넣으면 429 |
| `POST /api/room/start` | 방장만. 시작 또는 다시 하기 |
| `POST /api/room/leave` | 방 나가기 |
| `POST /api/continue` | 방장만. 기다리는 단계 넘기기. 선택: `gateId` |
| `POST /api/skip` | 방장만. 더 행동할 사람이 없을 때 이번 판 빠르게 진행 |
| `POST /api/chat/react` | `{"emoji":0~5}` 또는 `{"phrase":0~14}` (번호만) |
| `POST /api/chat/say` | `{"text":"..."}` 비공개 친구 방에서만 |
| `POST /api/chat/enable`, `/api/chat/mute` | 방장만. `{"on":bool}`, `{"name":"...","on":bool}` |
| `POST /api/chat/quiet` | 봇 반응 받지 않기 `{"quiet":bool}` |
| `POST /api/beat` | 이용 시간 신호 `{play,stay,game,final,touch,id}` (통계용) |
| `POST /api/pace` | 방장만. `pace`: `slow`, `normal`, `fast` (`/api/new`, `/api/room/create`에서도 선택) |
| `GET /api/state` | 내 시점의 상태. 방이 없으면 `{"hasGame":false, ...}` |
| `POST /api/action` | `{"action":"fold\|check\|call\|raise\|allin","amount":N,"turnId":T}`. `amount`는 `raise`의 이번 라운드 **총액**, `turnId`는 선택 |

`GET /api/state`의 안내/리뷰용 필드:

| 필드 | 내용 |
|---|---|
| `actingSeat` | 지금 결정 중인 좌석 (봇 포함, 없으면 -1) |
| `actionPhase`, `lastAction` | `thinking`/`revealed`, 방금 공개된 행동 `{seq,seat,text}` |
| `waiting` | `null` 또는 `{next: flop\|turn\|river\|showdown\|nextHand, gateId, canContinue}` |
| `canSkip`, `skipping`, `pace` | 건너뛰기 가능/진행 중, 현재 속도 |
| `myHand` | 플랍 이후 내 족보 `{name,code,level(1~6),boardOnly}` |
| `review.ranking[]` | 이번 판 순위: `seat, place, folded, tie, handName, cards, delta, best5, hand, label, groups` (폴드는 카드 없음) |
| `ranking[].best5[]` | 최종 5장: `card`, `group`(`A`, `B`, 족보에 안 들어가면 `null`), `hole`(손패인지) |
| `ranking[].hand, label, groups` | `FULL_HOUSE` 같은 코드, `풀 하우스 (2 트리플 + 8 페어)`, 묶음 이름 `[{id,label}]` |
| `review.standings[]` | 현재 칩 순위: `seat, rank, tie, chips, change, out` |
| `review` | 마지막으로 끝난 판의 리뷰. 아직 없으면 `null` (화면은 판이 끝났을 때만 보여 줌) |
| `review.handNumber, showdown, board, names` | 판 번호, 쇼다운 여부, 공용 카드, 좌석별 이름 |
| `review.entries[]` | 쇼다운 참여자(강한 순): `seat, place, hole, best, key, category, handName, nextSeat, versusNext` |
| `review.pots[]` | `amount, winners[{seat,amount}], eligible, cappedBy, loserSeat, oddChip, compare` |
| `review.folded` | 폴드한 좌석 (카드는 보내지 않음) |
| `review.me` | 내가 쇼다운에 참여했을 때: `won, streets[{street,status,equity}]` |

`compare`/`versusNext`는 `type`(category, rank, tie), `role`, `winRank`, `loseRank`, `shared`, `boardPlays`입니다.
안내와 리뷰의 문구는 `web/index.html`의 `TEXTS` 객체에 모여 있습니다.

잘못된 요청은 400, 방 한도를 넘으면 429와 `{"error":"ROOMS_FULL"|"CLIENT_ROOMS_FULL","message":"..."}`로 응답합니다. 접속자는 쿠키 `sid`로 구분합니다.
`POST /api/visit`은 방문 1회를 알리며, 브라우저 식별값은 헤더 `X-Client-Id`(랜덤 ID의 해시)로 보냅니다. 방이 정리되면 `/api/state`에 `closed` 안내 문구가 담깁니다.

## 방 제한

서버를 켤 때 환경변수로 바꿀 수 있습니다. 기본값은 `src/web_session.h`에 있습니다.

| 환경변수 | 기본 | 뜻 |
|---|---|---|
| `MAX_ROOMS` | 20 | 동시에 열려 있는 방 (혼자 봇과 하기 포함) |
| `MAX_PLAYERS_PER_ROOM` | 6 | 방당 최대 인원, 봇 포함 (2~6) |
| `ROOM_IDLE_MINUTES` | 30 | 이 시간 동안 입력(시작, 입장, 행동, 다음 누르기)이 없으면 삭제. 화면의 자동 조회는 세지 않음 |
| `MAX_ROOMS_PER_CLIENT` | 2 | 한 브라우저가 동시에 만들 수 있는 방 |

브라우저는 IP가 아니라 저장해 둔 랜덤 ID로 구분하므로, 같은 와이파이를 쓰는 사람들이 한 명으로 묶이지 않습니다.
ID는 누구나 바꿔 보낼 수 있으므로 `MAX_ROOMS_PER_CLIENT`는 실수를 막는 정도의 제한이고, 서버를 지키는 것은 `MAX_ROOMS`입니다.

## 사용 통계

집계 숫자, 날짜, 랜덤 ID의 해시를 `실행한 폴더/data/stats.json`(권한 600)에 쌓습니다. **닉네임, IP, 카드·채팅 내용은 저장하지 않습니다.**
방문자는 브라우저가 만든 랜덤 ID의 해시로만 구분하며, 같은 ID는 하루에 한 번만 셉니다.

통계 화면을 보려면 `STATS_TOKEN`을 정해서 서버를 켭니다.

```bash
export STATS_TOKEN="$(openssl rand -hex 16)"   # 긴 임의의 값
echo "$STATS_TOKEN"                            # 따로 적어 두세요
./build/holdem --web                           # 날짜는 늘 한국 시간으로 센다
```

그다음 `http://서버주소:8080/stats?token=위의값` 으로 접속합니다. 10초마다 저절로 새로 고칩니다.

| 항목 | 뜻 |
|---|---|
| 지금 접속 중 | 최근 60초 안에 신호를 보낸 브라우저 (게임 화면이 보이는 동안 30초마다). 메모리에서만 셈 |
| 진행 중인 게임 / 열린 방 | 지금 서버에 있는 방 (혼자 하기 포함) |
| 동시 접속 최고치 | 하루 중 가장 많았던 때와 그 시각(한국 시간) |
| 활성 플레이어 | 그날 한 판 이상 끝까지 함께한 브라우저 수 |
| 다시 한 사람 | 서로 다른 날 이틀 이상 한 판을 끝낸 브라우저 수 |
| 봇 접속 | 링크 미리보기 봇과 크롤러가 첫 화면을 가져간 횟수. 방문에는 세지 않음 |
| 플레이 시간 | 게임 화면이 보이고 최근 2분 안에 입력(터치·클릭·키)이 있었던 시간 |
| 체류 시간 | 시작·설정·대기실·규칙·족보·이야기 화면의 시간 (같은 조건) |
| 세션 | 30분 넘게 신호가 없으면 새 세션 (새로고침은 같은 세션). 길이 = 플레이 + 체류 |
| 브라우저 종류별 | User-Agent에서 종류만 추림(원문은 저장하지 않음). 첫 판 완료율 = 한 판 이상 완료한 브라우저 / 처음 본 브라우저 |
| 방문자 1~20 | 플레이 시간이 긴 브라우저. 식별값은 화면과 JSON에 내보내지 않음 |

신호는 30초마다이며, 서버는 15초보다 잦은 신호를 버리고 직전 신호 이후 실제로 흐른 시간까지만 인정합니다. 90초 넘게 벌어진 구간은 세지 않습니다.
`/stats?token=...&format=json`으로 같은 집계를 JSON으로 받을 수 있습니다. 브라우저별 값은 랜덤 ID의 해시와 함께 `data/stats.json`에 저장합니다.

**내 접속 빼기**: `주소/?owner=1`로 한 번 들어가면 그 브라우저는 모든 집계에서 빠집니다 (`?owner=0`으로 해제). 브라우저·기기마다 따로 해야 하고, 이미 센 숫자는 줄지 않습니다.

- `STATS_TOKEN`이 없으면 `/stats`는 404입니다. 토큰이 틀려도 404입니다.
- 토큰을 코드나 git에 적지 마세요. 이 서버는 HTTPS가 없어 주소의 토큰이 암호화되지 않으므로, 다른 곳에서 쓰는 비밀번호를 재사용하지 마세요.
- "총 방문 수"는 (브라우저, 날짜)의 합, "게임당 평균 판 수"는 완료한 판 수를 게임 시작 횟수로 나눈 값입니다.

### 날짜별 기록 (ledger)

한국 시간 자정이 지나면 전날 집계를 `data/stats-ledger.jsonl`에 한 줄 덧붙입니다(꺼져 있었다면 다음에 켤 때).
줄마다 앞줄의 해시를 담아, 지난 줄을 고치면 검증에서 드러납니다. 서버를 켤 때 전체를 검증하며, 어긋나면 `/stats`에 "ledger 불일치"가 표시되고 더 덧붙이지 않습니다.

- **`data/` 폴더는 다시 배포해도 남아 있어야 합니다.** 지워지면 기록이 처음부터 다시 시작됩니다.
- 유입 경로는 `https://주소/?src=instagram`처럼 붙입니다. 세는 이름: `instagram`, `everytime`, `kakao`, `discord`, `friend`, `poster`, `qr`, `github` (그 밖은 `other`).
- 항목 설명과 검증 방법: [tools/ledger/README.md](tools/ledger/README.md)

공개 저장소에 올리기 (자동 실행은 설정돼 있지 않습니다):

```bash
# 처음 한 번: GitHub에 기록 전용 공개 저장소를 만들고 받아 둔다
git clone git@github.com:계정/holdem-ledger.git ~/holdem-ledger

# 서버를 실행하는 폴더에서
LEDGER_REPO=~/holdem-ledger tools/ledger/publish.sh --dry-run   # 무엇이 올라갈지만 확인
LEDGER_REPO=~/holdem-ledger tools/ledger/publish.sh             # 실제로 올리기
```

## 서버 배포 (Ubuntu 22.04)

```bash
sudo apt update
sudo apt install -y g++ cmake make git

git clone https://github.com/jaehk28-hash/dash-texasholdem-c-.git holdem-practice
cd holdem-practice
cmake -S . -B build
cmake --build build -j2              # 메모리가 작은 서버라면 -j 없이
ctest --test-dir build --output-on-failure

./build/holdem --web --port 8080
```

포트 열기:

```bash
sudo ufw allow 8080/tcp              # ufw를 쓰는 경우
```

클라우드 서버(AWS, GCP, Oracle 등)는 서버 안의 설정과 별개로, 콘솔의 **보안 그룹/방화벽 규칙**에서도 TCP 8080 인바운드를 열어야 합니다.
그다음 브라우저에서 `http://서버IP:8080`으로 접속합니다.

SSH를 끊어도 계속 실행하려면:

```bash
nohup ./build/holdem --web --port 8080 > holdem.log 2>&1 &
pkill -f "holdem --web"              # 끌 때
```

실행 파일에는 빌드할 때의 `web/` 폴더 경로가 기록되므로, 폴더를 옮겼다면 다시 빌드하세요.

**주의**: 연습용 서버라 로그인과 HTTPS가 없습니다. 주소를 아는 사람은 누구나 접속할 수 있고, 방 만들기 횟수 제한이 없어 한 사람이 방 20개를 모두 차지할 수도 있습니다.
가능하면 방화벽에서 나와 친구들의 IP만 허용하세요. (코드 대입 제한은 접속 IP 기준이라, 프록시 뒤에 두면 모두가 한 IP로 계산됩니다.)

## 규칙 요약

- 52장 덱을 매 판 새로 섞고, 딜러 버튼은 매 판 시계 방향으로 한 칸씩 이동 (탈락자는 건너뜀).
- SB는 버튼 다음, BB는 그 다음. 홀 카드는 SB부터 한 장씩 두 바퀴. 플랍 3장, 턴 1장, 리버 1장이고 각 카드 전에 번 카드를 버림.
- 프리플랍은 BB 다음(UTG)부터, 이후 라운드는 버튼 다음 첫 번째로 남은 사람(SB)부터 행동.
- **헤즈업(2명)**: 버튼이 SB. 프리플랍은 버튼이 먼저, 이후 라운드는 BB가 먼저.
- 행동: 폴드, 체크, 콜, 벳/레이즈, 올인.
- **최소 레이즈**: 레이즈 증가분은 직전 벳/레이즈 증가분 이상(포스트플랍 최소 벳은 BB). 풀 레이즈보다 작은 올인은 이미 행동한 사람에게 레이즈 권리를 다시 열지 않음. 단, 작은 올인이 여러 번 쌓여 풀 레이즈 폭 이상이 되면 다시 열림.
- 콜 받지 못한 베팅(상대가 모두 폴드하거나 더 적게 올인한 경우)은 주인에게 돌려줌.
- **사이드 팟**: 올인 금액별로 메인 팟/사이드 팟을 나누고, 각 팟은 그 팟에 참여할 자격이 있는 사람 중 최고 핸드가 가져감.
- 쇼다운: 7장 중 최고 5장. A-2-3-4-5(휠)에서 에이스는 1. 동점이면 팟을 나누고, 남는 칩은 팟마다 버튼 왼쪽 첫 승자부터 한 칩씩.
- 모두 폴드해 한 명만 남으면 쇼다운 없이 팟 획득.
- 칩이 0이 되면 탈락. 내가 탈락하면 게임 종료, 봇만 남으면 내 우승.
- 칩 총합은 라운드마다/판마다 검사하며, 어긋나면 즉시 프로그램이 멈춥니다 (릴리스 빌드에서도 동작).

## 봇 5종

- 봇은 **공용 카드, 베팅, 팟, 스택, 포지션, 자기 카드만** 보고 판단합니다. 판단 함수의 입력(`GameState`)에는 남의 홀 카드가 없습니다.
- 성격은 참여 비율(VPIP), 레이즈 비율(PFR), 공격성, 블러프, 압박에 접는 정도, 슬로우플레이, 생각 속도의 값입니다 (`src/bots.h`).
- 베팅 크기는 프리플랍 2.2~3.5BB, 이후 팟의 1/3, 1/2, 2/3, 1배, 가끔 오버벳입니다. 큰 팟(20BB 이상)을 잃으면 3판 동안 성격에 따라 더 세지거나 위축됩니다.
- 생각 시간은 쉬운 폴드 1~2초, 보통 1.5~3.5초, 큰 베팅·올인 3.5~7초에 성격 속도와 ±20%를 곱합니다. 카드 강도는 쓰지 않습니다. 진행 속도에 비례해 줄고 그 속도의 값(5, 3, 1초)을 넘지 않습니다.
- 혼자 하기의 난이도(쉬움/보통/어려움)는 실수 확률과 블러프 빈도만 바꿉니다.

| 봇 | 성격 |
|---|---|
| 타이트 | 좋은 핸드(약 17%)만 플레이. 들어오면 강하게 베팅, 큰 레이즈 앞에서는 잘 접음 |
| 루즈 | 거의 모든 핸드에 참여하고 콜을 많이 함. 가끔 레이즈 |
| 공격형 | 자주 벳/레이즈하고 블러프도 함 (큰 베팅 앞에서는 신중) |
| 콜링스테이션 | 거의 항상 콜만 함. 아주 강할 때만 가끔 레이즈 |
| 확률계산 | 몬테카를로로 승률을 구해 팟 오즈와 비교해 행동 (승률 > 팟 오즈 + 3%p면 콜, 높으면 밸류 벳) |

새 봇 추가: `Player`를 상속해 `name()`과 `decide(const GameState&)`만 구현하면 됩니다. 봇은 `src/bots.h/.cpp`, 사람은 `src/human_player.h/.cpp`가 좋은 예입니다.

## 폴더 구조

```
src/
  card.*          카드, 덱                    evaluator.*   7장 중 최고 5장 족보 판정
  pot.*           팟/사이드 팟 계산과 분배     equity.*      몬테카를로 승률
  player.h        Player 추상 클래스, GameState, 행동
  game.*          한 판 진행, 베팅 라운드      bots.*        봇 5종
  human_player.*  콘솔 입력/출력, 힌트         main.cpp      실행 파일
  web_json.*      작은 JSON 도구               web_session.* 게임 방과 게임 스레드
  web_server.*    HTTP 서버와 API
  review.*        판이 끝난 뒤의 핸드 리뷰 데이터
  stats.*         익명 사용 통계와 /stats 화면
  bench.cpp       성능/정확도 측정 프로그램 (docs/bench.md 생성)
web/index.html    웹 화면 (HTML+CSS+JS 한 파일)
third_party/      httplib.h (cpp-httplib v0.67.0, MIT 라이선스)
tests/            test_evaluator / test_pot / test_game / test_web / test_review / test_server (CHECK 방식)
docs/bench.md     측정 결과 전체
```

## 테스트가 확인하는 것

- 족보: 모든 족보, 휠, 키커, 동점, 7장 선택, 5장 조합 2,598,960개 전체의 족보별 개수가 알려진 값과 일치, 원 페어 시뮬레이션 비율(약 42.3%), 빠른 판정 방식과 전수 검사 방식의 일치(수십만 건 무작위 대조).
- 팟: 올인이 여러 명인 사이드 팟, 폴드한 사람의 칩, 동점 분배, 홀수 칩, 무작위 입력에서 칩 보존.
- 진행: 버튼/블라인드 이동, 행동 순서, 헤즈업, 딜 순서, 최소 레이즈, 숏 올인/풀 올인의 레이즈 권리, 콜 못 받은 베팅 반환, 사이드 팟 쇼다운, 탈락, 무작위로 행동하는 플레이어 수천 판에서 칩 보존.
- 승률: AA/AKs/72o의 알려진 승률과 비교, 봇 성격이 통계로 드러나는지, 사람 입력 파싱과 재질문, 힌트 출력.

- 웹: 방 정리(30분)와 20개 제한, 입장/나가기/방장 승계, 설정과 닉네임 검사, 잘못된 요청의 400 거절, 코드 대입 제한, 제한 시간, 쇼다운 전 남의 카드 비노출, JSON 해석.

## 측정 결과 요약 (자세한 내용은 [docs/bench.md](docs/bench.md))

**봇끼리 10만 판** (5인 테이블, 매 판 100BB로 초기화, 약 18초):

| 봇 | 평균 칩 변화/판 | 이긴 판 비율 |
|---|---:|---:|
| 공격형 | +518.5 | 21.4% |
| 확률계산 | +326.5 | 9.3% |
| 타이트 | +315.3 | 7.5% |
| 루즈 | -109.7 | 28.4% |
| 콜링스테이션 | -1050.6 | 36.9% |

**승률 시뮬레이션 시행 횟수별 오차** (AA vs 1명, 기준 85.19%): 1천 회 ±0.87%p / 0.12ms, 1만 회 ±0.28%p / 1.2ms, 10만 회 ±0.08%p / 11.7ms. 힌트의 기본값 1만 회면 오차가 대체로 1%p 이내입니다.

족보 판정은 7장 기준 초당 약 2,500만 회로, 전수 검사 방식보다 약 23배 빠릅니다.

## 알아 둘 점

- 연습용 단순화: 탈락자가 생겨도 버튼은 "칩이 있는 다음 사람"으로 옮기며(데드 버튼 규칙 없음), 쇼다운에서 카드를 숨기는(머킹) 선택은 없고 모두 공개합니다.
- 체크할 수 있는 상황에서 폴드를 입력하면 막고 안내합니다 (봇이 그런 폴드를 내면 체크로 바뀜).
- 승률 힌트는 상대 카드를 무작위로 가정합니다. 실제 상대의 베팅 범위는 반영하지 않습니다.
- 봇 대결 측정은 봇 5종이 한 명씩인 5인 테이블에서 했고, 결과는 이 5종의 조합에서의 상대적 성적입니다.

# 홀덤 연습 시뮬레이터 (holdem-practice)

콘솔에서 **나 1명 + 봇 5명**이 한 테이블에서 노리밋 텍사스 홀덤을 연습하는 C++17 프로그램입니다.
홀덤 펍 규칙을 그대로 따르고, 칩만 쓰므로 실제 돈은 오가지 않습니다.
내 차례에는 승률과 팟 오즈 같은 **힌트**를 보여 주고, 판이 끝나면 **핸드 히스토리**를 볼 수 있습니다.
브라우저에서 플레이하는 [웹 모드](#웹-모드)도 있습니다.

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
WEB=(src/web_json.cpp src/web_session.cpp src/web_server.cpp)
WEBFLAGS=(-isystem third_party -pthread -DHOLDEM_WEB_DIR="\"$PWD/web\"")
c++ -std=c++17 -O2 -Wall -Wextra -Isrc "${WEBFLAGS[@]}" "${CORE[@]}" "${WEB[@]}" src/main.cpp -o build/holdem
c++ -std=c++17 -O2 -Wall -Wextra -Isrc "${CORE[@]}" src/bench.cpp -o build/holdem_bench
for t in evaluator pot game; do
  c++ -std=c++17 -O2 -Wall -Wextra -Isrc "${CORE[@]}" tests/test_$t.cpp -o build/test_$t && ./build/test_$t
done
c++ -std=c++17 -O2 -Wall -Wextra -Isrc "${WEBFLAGS[@]}" "${CORE[@]}" "${WEB[@]}" tests/test_web.cpp -o build/test_web && ./build/test_web
c++ -std=c++17 -O2 -Wall -Wextra -Isrc "${WEBFLAGS[@]}" "${CORE[@]}" "${WEB[@]}" tests/test_review.cpp -o build/test_review && ./build/test_review
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

처음 접속하면 **규칙 설명**이 나오고, 확인하면 메뉴가 보입니다 (규칙은 오른쪽 위 버튼으로 언제든 다시 볼 수 있습니다).

| 메뉴 | 내용 |
|---|---|
| 혼자 봇과 하기 | 상대 인원(1~5, 기본 3)과 승률 힌트만 고르고 시작. 칩, 블라인드, 속도는 "상세 설정"에 있습니다 |
| 방 만들기 | 위 설정 + 닉네임, 제한 시간(30초/60초/2분), 봇 수(0~5). **6자리 방 코드**가 나옵니다 |
| 코드로 들어가기 | 친구에게 받은 코드와 닉네임을 입력 |

### 친구와 하는 법

1. 모두 **같은 서버 주소**로 접속합니다. 같은 와이파이라면 `http://서버를_켠_컴퓨터의_IP:8080`, 멀리 있는 친구라면 아래 "서버 배포"가 필요합니다.
2. 한 사람이 방을 만들고 코드를 알려 주면, 친구들이 코드를 입력해 들어옵니다 (최대 6명).
3. 방장이 **게임 시작**을 누릅니다. 빈 자리에는 설정한 수만큼(남는 자리 한도) 봇이 앉습니다.

- 제한 시간 안에 행동하지 않으면 자동으로 체크, 낼 칩이 있으면 폴드합니다.
- 게임 중에 나가면 그 자리는 자동으로 체크/폴드하며, 다시 들어올 수 없습니다. 새로고침이나 탭을 다시 여는 것은 괜찮습니다.
- 시작한 방에는 새로 들어올 수 없습니다. 게임이 끝나면 방장이 **다시 하기**를 누를 수 있습니다.
- 방장이 나가면 다음으로 들어온 사람이 방장이 됩니다.
- 브라우저 하나가 한 사람입니다(쿠키). 한 컴퓨터에서 두 명을 시험하려면 시크릿 창이나 다른 브라우저를 쓰세요.

### 공통

- `--chips`, `--sb`, `--bb`는 설정 화면의 기본값이 되고, `--no-hint`는 모든 방의 힌트를 끕니다.
- 방은 동시에 최대 20개이고(혼자 하기 포함), 30분 동안 입력(시작, 입장, 행동)이 없으면 정리됩니다. 서버를 끄면 모두 사라집니다.
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
| `POST /api/room/create` | `name` 필수, 선택: `chips, sb, bb, bots, hint, turnSeconds` |
| `POST /api/room/join` | `code, name`. 없는 코드를 1분에 10번 넣으면 429 |
| `POST /api/room/start` | 방장만. 시작 또는 다시 하기 |
| `POST /api/room/leave` | 방 나가기 |
| `POST /api/continue` | 방장만. 기다리는 단계 넘기기. 선택: `gateId` |
| `POST /api/skip` | 방장만. 더 행동할 사람이 없을 때 이번 판 빠르게 진행 |
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

잘못된 요청은 400, 방 20개가 모두 차면 503으로 응답합니다. 접속자는 쿠키 `sid`로 구분합니다.

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
  bench.cpp       성능/정확도 측정 프로그램 (docs/bench.md 생성)
web/index.html    웹 화면 (HTML+CSS+JS 한 파일)
third_party/      httplib.h (cpp-httplib v0.67.0, MIT 라이선스)
tests/            test_evaluator / test_pot / test_game / test_web / test_review (CHECK 방식)
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

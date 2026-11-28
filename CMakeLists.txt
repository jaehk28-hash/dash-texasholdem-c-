cmake_minimum_required(VERSION 3.10)
project(holdem_practice CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

# 게임 로직 라이브러리 (단계가 진행되면서 파일이 추가된다)
add_library(holdem_core
  src/card.cpp
  src/evaluator.cpp
  src/pot.cpp
  src/review.cpp
  src/game.cpp
  src/equity.cpp
  src/bots.cpp
  src/human_player.cpp
)
target_include_directories(holdem_core PUBLIC src)

# 웹 모드: 세션 관리 + HTTP 서버 (third_party/httplib.h 사용, pthread 필요)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
add_library(holdem_web
  src/web_json.cpp
  src/web_session.cpp
  src/web_server.cpp
  src/stats.cpp
  src/ledger.cpp
  src/sha256.cpp
  src/chat.cpp
)
# SYSTEM: 외부 헤더에서 나는 경고는 숨긴다
target_include_directories(holdem_web SYSTEM PUBLIC third_party)
# web/index.html 위치를 실행 파일에 알려 준다
target_compile_definitions(holdem_web PRIVATE HOLDEM_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web")
target_link_libraries(holdem_web PUBLIC holdem_core Threads::Threads)

# 실행 파일
add_executable(holdem src/main.cpp)
target_link_libraries(holdem holdem_web)

# 성능 측정 프로그램 (docs/bench.md 생성)
add_executable(holdem_bench src/bench.cpp)
target_link_libraries(holdem_bench holdem_core)

# 테스트: 외부 라이브러리 없이 단순 assert 방식
enable_testing()
function(add_holdem_test name)
  add_executable(${name} tests/${name}.cpp)
  target_link_libraries(${name} holdem_core)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_holdem_test(test_evaluator)
add_holdem_test(test_pot)
add_holdem_test(test_game)

# 웹 테스트는 holdem_web(holdem_core 포함)에 연결한다
add_executable(test_web tests/test_web.cpp)
target_link_libraries(test_web holdem_web)
add_test(NAME test_web COMMAND test_web)

# 리뷰 테스트는 JSON까지 확인하므로 holdem_web에 연결한다
add_executable(test_review tests/test_review.cpp)
target_link_libraries(test_review holdem_web)
add_test(NAME test_review COMMAND test_review)

add_executable(test_server tests/test_server.cpp)
target_link_libraries(test_server holdem_web)
target_compile_definitions(test_server PRIVATE HOLDEM_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web")
add_test(NAME test_server COMMAND test_server)

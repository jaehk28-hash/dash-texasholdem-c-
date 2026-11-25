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
  src/game.cpp
  src/equity.cpp
  src/bots.cpp
  src/human_player.cpp
)
target_include_directories(holdem_core PUBLIC src)

# 실행 파일
add_executable(holdem src/main.cpp)
target_link_libraries(holdem holdem_core)

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

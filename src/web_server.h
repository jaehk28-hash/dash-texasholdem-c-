// web_server.h - HTTP 서버 (cpp-httplib)
#pragma once

#include <string>

#include "web_session.h"

namespace httplib {
class Server;
}

// 서버에 주소(라우트)와 안전 설정을 등록한다. 테스트에서도 같은 함수를 쓴다.
// indexHtml은 "/"에서 보여 줄 화면 내용.
void setupRoutes(httplib::Server& server, RoomManager& rooms, const std::string& indexHtml);

// 웹 모드 실행: 0.0.0.0:port에서 요청을 받는다. 서버가 끝날 때까지 돌아오지 않는다.
// 반환값은 프로그램 종료 코드.
int runWebServer(int port, const WebOptions& options);

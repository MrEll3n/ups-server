#pragma once

#include <string>
#include <vector>

inline constexpr const char* PROTOCOL_MAGIC = "MRLLN";

enum class RequestType {
    LOGIN,
    LOGOUT,
    CREATE_LOBBY,
    JOIN_LOBBY,
    LEAVE_LOBBY,
    MOVE,
    REMATCH,
    STATE,
    // Heartbeat initiated by server: server sends RES_PING|nonce|, client answers REQ_PONG|nonce|
    PONG,
    INVALID
};

enum class SessionPhase {
    NotLoggedIn,
    LoggedInNoLobby,
    InLobby,
    InGame,
    AFTER_GAME,   // PŘIDANÝ STAV
    INVALID       // Ponecháme zde pro obsluhu v switch blocích
};

struct Request {
    RequestType type{RequestType::INVALID};
    std::vector<std::string> params;
    bool valid_magic{true};
};

Request parse_request_line(const std::string& line);

namespace Responses {

    // ---- Standard OK responses ----
    std::string login_ok(int userId);
    std::string login_fail();

    std::string logout_ok();

    std::string lobby_created(int lobbyId);
    std::string lobby_joined(const std::string& lobbyName);
    std::string lobby_left();

    std::string game_started();

    std::string move_accepted(const std::string& moveStr);

    std::string round_result(int winnerUserId,
                             const std::string& p1Move,
                             const std::string& p2Move,
                             int p1Wins,
                             int p2Wins);

    std::string match_result(int winnerUserId,
                             int p1Wins,
                             int p2Wins);

    std::string rematch_ready();

    std::string game_cannot_continue(const std::string& reason);

    std::string state(const std::string& debug);

    // Server heartbeat (client must reply with REQ_PONG|same_nonce|)
    std::string ping(const std::string& nonce);

    // "RES_OPPONENT_DISCONNECTED|15"
    std::string opponent_disconnected(int seconds);

    // "RES_GAME_RESUMED"
    std::string game_resumed();

    // ---- Error responses ----
    std::string error_unexpected_state();
    std::string error_invalid_magic();
    std::string error_invalid_move();
    std::string error_not_in_lobby();
    std::string error_lobby_full();
    std::string error_lobby_not_found();
    std::string error_unknown_request();
    std::string error_not_in_game();
    std::string error_rematch_not_allowed();
    std::string error_malformed_request();
    std::string error(const std::string& msg); // generic

}

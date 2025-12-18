#include "Protocol.hpp"

#include <sstream>
#include <string>
#include <vector>

// ------------------------------------
// Helpers
// ------------------------------------
static std::vector<std::string> split_pipe_keep_empty(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '|') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\n' && c != '\r') {
            cur.push_back(c);
        }
    }
    // If the message has a trailing '|', split will already have pushed the last empty element.
    // If it doesn't, pushing the final token keeps behavior consistent.
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static std::string prefix(const std::string& payload) {
    // IMPORTANT: your Server::send_line() already appends "\n".
    // However, your project historically used "Responses::*" WITHOUT newline.
    // So we return WITHOUT newline here.
    return std::string(PROTOCOL_MAGIC) + "|" + payload + "|";
}

// ------------------------------------
// Request parsing (USED by Server.cpp)
// ------------------------------------
Request parse_request_line(const std::string& line) {
    Request req;
    req.valid_magic = true;
    req.type = RequestType::INVALID;
    req.params.clear();

    const auto parts = split_pipe_keep_empty(line);
    if (parts.size() < 2) {
        req.type = RequestType::INVALID;
        return req;
    }

    // parts[0] should be magic
    if (parts[0] != PROTOCOL_MAGIC) {
        req.valid_magic = false;
        req.type = RequestType::INVALID;
        return req;
    }

    const std::string& type_desc = parts[1];

    // Params are parts[2..n-2] typically (the last part is empty due to trailing '|')
    for (size_t i = 2; i < parts.size(); i++) {
        if (!parts[i].empty()) req.params.push_back(parts[i]);
    }

    if (type_desc == "REQ_LOGIN") req.type = RequestType::LOGIN;
    else if (type_desc == "REQ_LOGOUT") req.type = RequestType::LOGOUT;
    else if (type_desc == "REQ_CREATE_LOBBY") req.type = RequestType::CREATE_LOBBY;
    else if (type_desc == "REQ_JOIN_LOBBY") req.type = RequestType::JOIN_LOBBY;
    else if (type_desc == "REQ_LEAVE_LOBBY") req.type = RequestType::LEAVE_LOBBY;
    else if (type_desc == "REQ_MOVE") req.type = RequestType::MOVE;
    else if (type_desc == "REQ_REMATCH") req.type = RequestType::REMATCH;
    else if (type_desc == "REQ_STATE") req.type = RequestType::STATE;
    else if (type_desc == "REQ_PONG") req.type = RequestType::PONG;
    else {
        // Unknown request -> keep INVALID, Server.cpp will respond error_unknown_request()
        req.type = RequestType::INVALID;
    }

    return req;
}

// ------------------------------------
// Responses (USED by Server.cpp)
// ------------------------------------
namespace Responses {

std::string login_ok(int userId) {
    return prefix("RES_LOGIN_OK|" + std::to_string(userId));
}
std::string login_fail() {
    return prefix("RES_LOGIN_FAIL");
}
std::string logout_ok() {
    return prefix("RES_LOGOUT_OK");
}

std::string lobby_created(int lobbyId) {
    return prefix("RES_LOBBY_CREATED|" + std::to_string(lobbyId));
}
std::string lobby_joined(const std::string& lobbyName) {
    return prefix("RES_LOBBY_JOINED|" + lobbyName);
}
std::string lobby_left() {
    return prefix("RES_LOBBY_LEFT");
}

std::string game_started() {
    return prefix("RES_GAME_STARTED");
}

std::string move_accepted(const std::string& moveStr) {
    return prefix("RES_MOVE|" + moveStr);
}

// Backward compatible version (3 params) – keep if some code still calls it
std::string round_result(int winnerUserId,
                         const std::string& p1Move,
                         const std::string& p2Move) {
    return prefix("RES_ROUND_RESULT|" + std::to_string(winnerUserId) + "|" + p1Move + "|" + p2Move);
}

// New version (5 params) – used for live score updates
std::string round_result(int winnerUserId,
                         const std::string& p1Move,
                         const std::string& p2Move,
                         int p1Wins,
                         int p2Wins) {
    return prefix("RES_ROUND_RESULT|" + std::to_string(winnerUserId) + "|" +
                  p1Move + "|" + p2Move + "|" +
                  std::to_string(p1Wins) + "|" + std::to_string(p2Wins));
}

std::string match_result(int winnerUserId,
                         int p1Wins,
                         int p2Wins) {
    return prefix("RES_MATCH_RESULT|" + std::to_string(winnerUserId) + "|" +
                  std::to_string(p1Wins) + "|" + std::to_string(p2Wins));
}

std::string rematch_ready() {
    return prefix("RES_REMATCH_READY");
}

std::string game_cannot_continue(const std::string& reason) {
    return prefix("RES_GAME_CANNOT_CONTINUE|" + reason);
}

std::string state(const std::string& debug) {
    return prefix("RES_STATE|" + debug);
}

std::string ping(const std::string& nonce) {
    return prefix("RES_PING|" + nonce);
}

std::string opponent_disconnected(int seconds) {
    return prefix("RES_OPPONENT_DISCONNECTED|" + std::to_string(seconds));
}

std::string game_resumed() {
    return prefix("RES_GAME_RESUMED");
}

// ---- Error responses ----
std::string error_unexpected_state() {
    return prefix("RES_ERROR|Unexpected state");
}
std::string error_invalid_magic() {
    return prefix("RES_ERROR|Invalid magic");
}
std::string error_invalid_move() {
    return prefix("RES_ERROR|Invalid move");
}
std::string error_not_in_lobby() {
    return prefix("RES_ERROR|Not in lobby");
}
std::string error_lobby_full() {
    return prefix("RES_ERROR|Lobby full");
}
std::string error_lobby_not_found() {
    return prefix("RES_ERROR|Lobby not found");
}
std::string error_unknown_request() {
    return prefix("RES_ERROR|Unknown request");
}
std::string error_not_in_game() {
    return prefix("RES_ERROR|Not in game");
}
std::string error_rematch_not_allowed() {
    return prefix("RES_ERROR|Rematch not allowed");
}
std::string error_malformed_request() {
    return prefix("RES_ERROR|Malformed request");
}
std::string error(const std::string& msg) {
    return prefix("RES_ERROR|" + msg);
}

} // namespace Responses

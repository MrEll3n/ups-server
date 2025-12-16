#include "Protocol.hpp"

#include <sstream>

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::string cur;
    std::istringstream iss(s);
    while (std::getline(iss, cur, delim)) {
        parts.push_back(cur);
    }
    return parts;
}

static void trim_trailing_empty(std::vector<std::string>& parts) {
    while (!parts.empty() && parts.back().empty()) {
        parts.pop_back();
    }
}

Request parse_request_line(const std::string& line) {
    Request req;
    auto parts = split(line, '|');
    trim_trailing_empty(parts);

    // Expect: MRLLN|REQ_...|p1|p2|
    if (parts.size() < 2) {
        req.type = RequestType::INVALID;
        return req;
    }

    const std::string magic = parts[0];
    if (magic != PROTOCOL_MAGIC) {
        req.valid_magic = false;
        req.type = RequestType::INVALID;
        return req;
    }

    const std::string tag = parts[1];

    if (tag == "REQ_LOGIN") req.type = RequestType::LOGIN;
    else if (tag == "REQ_LOGOUT") req.type = RequestType::LOGOUT;
    else if (tag == "REQ_CREATE_LOBBY") req.type = RequestType::CREATE_LOBBY;
    else if (tag == "REQ_JOIN_LOBBY") req.type = RequestType::JOIN_LOBBY;
    else if (tag == "REQ_LEAVE_LOBBY") req.type = RequestType::LEAVE_LOBBY;
    else if (tag == "REQ_MOVE") req.type = RequestType::MOVE;
    else if (tag == "REQ_REMATCH") req.type = RequestType::REMATCH;
    else if (tag == "REQ_STATE") req.type = RequestType::STATE;
    else if (tag == "REQ_PONG") req.type = RequestType::PONG;
    else req.type = RequestType::INVALID;

    for (size_t i = 2; i < parts.size(); i++) {
        req.params.push_back(parts[i]);
    }

    return req;
}

static std::string prefix(const std::string& body) {
    // Always keep trailing delimiter '|' to match the original wire format.
    return std::string(PROTOCOL_MAGIC) + "|" + body + "|";
}

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

std::string round_result(int winnerUserId,
                         const std::string& p1Move,
                         const std::string& p2Move) {
    return prefix("RES_ROUND_RESULT|" + std::to_string(winnerUserId) + "|" + p1Move + "|" + p2Move);
}

std::string match_result(int winnerUserId,
                         int p1Wins,
                         int p2Wins) {
    return prefix("RES_MATCH_RESULT|" + std::to_string(winnerUserId) + "|" + std::to_string(p1Wins) + "|" + std::to_string(p2Wins));
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

std::string error_unexpected_state() { return prefix("RES_ERROR|Unexpected request in current session state"); }
std::string error_invalid_magic()     { return prefix("RES_ERROR|Invalid protocol magic"); }
std::string error_invalid_move()      { return prefix("RES_ERROR|Invalid move"); }
std::string error_not_in_lobby()      { return prefix("RES_ERROR|Not in lobby"); }
std::string error_lobby_full()        { return prefix("RES_ERROR|Lobby full"); }
std::string error_lobby_not_found()   { return prefix("RES_ERROR|Lobby not found"); }
std::string error_unknown_request()   { return prefix("RES_ERROR|Unknown request"); }
std::string error_not_in_game()       { return prefix("RES_ERROR|Not in game"); }
std::string error_rematch_not_allowed(){ return prefix("RES_ERROR|Rematch not allowed"); }
std::string error_malformed_request() { return prefix("RES_ERROR|Malformed request"); }

std::string error(const std::string& msg) {
    return prefix("RES_ERROR|" + msg);
}

} // namespace Responses

#include "Protocol.hpp"
#include <sstream>

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        parts.push_back(item);
    }
    return parts;
}

static void trim_trailing_empty_token(std::vector<std::string>& parts) {
    // Client uses trailing '|' => split() produces last token = ""
    if (!parts.empty() && parts.back().empty()) {
        parts.pop_back();
    }
}

//
// ---- Parsing ----
//
Request parse_request_line(const std::string& line) {
    Request req;
    auto parts = split(line, '|');
    trim_trailing_empty_token(parts);

    if (parts.size() < 2) {
        req.valid_magic = false;
        return req;
    }

    // Magic check
    if (parts[0] != PROTOCOL_MAGIC) {
        req.valid_magic = false;
        return req;
    }

    const std::string& tag = parts[1];

    if (tag == "REQ_LOGIN") req.type = RequestType::LOGIN;
    else if (tag == "REQ_LOGOUT") req.type = RequestType::LOGOUT;
    else if (tag == "REQ_CREATE_LOBBY") req.type = RequestType::CREATE_LOBBY;
    else if (tag == "REQ_JOIN_LOBBY") req.type = RequestType::JOIN_LOBBY;
    else if (tag == "REQ_LEAVE_LOBBY") req.type = RequestType::LEAVE_LOBBY;
    else if (tag == "REQ_MOVE") req.type = RequestType::MOVE;
    else if (tag == "REQ_REMATCH") req.type = RequestType::REMATCH;
    else if (tag == "REQ_STATE") req.type = RequestType::STATE;
    else if (tag == "REQ_PING") req.type = RequestType::PING;
    else req.type = RequestType::INVALID;

    for (size_t i = 2; i < parts.size(); ++i) {
        if (!parts[i].empty()) { // be defensive
            req.params.push_back(parts[i]);
        }
    }

    return req;
}

//
// ---- Response prefix helper ----
// Always emits trailing '|' so wire is consistent with your Python client.
//
static std::string prefix(const std::string& body_without_trailing_pipe) {
    return std::string(PROTOCOL_MAGIC) + "|" + body_without_trailing_pipe + "|";
}

//
// ---- OK responses ----
//

std::string Responses::login_ok(int userId) {
    return prefix("RES_LOGIN_OK|" + std::to_string(userId));
}

std::string Responses::login_fail() {
    return prefix("RES_LOGIN_FAIL");
}

std::string Responses::logout_ok() {
    return prefix("RES_LOGOUT_OK");
}

std::string Responses::lobby_created(int lobbyId) {
    return prefix("RES_LOBBY_CREATED|" + std::to_string(lobbyId));
}

std::string Responses::lobby_joined(const std::string& lobbyName) {
    return prefix("RES_LOBBY_JOINED|" + lobbyName);
}

std::string Responses::lobby_left() {
    return prefix("RES_LOBBY_LEFT");
}

std::string Responses::move_accepted(const std::string& moveStr) {
    return prefix("RES_MOVE|" + moveStr);
}

std::string Responses::game_started() {
    return prefix("RES_GAME_STARTED");
}

std::string Responses::round_result(int winnerUserId,
                                    const std::string& p1Move,
                                    const std::string& p2Move)
{
    return prefix(
        "RES_ROUND_RESULT|" +
        std::to_string(winnerUserId) + "|" + p1Move + "|" + p2Move
    );
}

std::string Responses::match_result(int winnerUserId,
                                    int p1Wins,
                                    int p2Wins)
{
    return prefix(
        "RES_MATCH_RESULT|" +
        std::to_string(winnerUserId) + "|" +
        std::to_string(p1Wins) + "|" +
        std::to_string(p2Wins)
    );
}

std::string Responses::rematch_ready() {
    return prefix("RES_REMATCH_READY");
}

std::string Responses::game_cannot_continue(const std::string& reason) {
    return prefix("RES_GAME_CANNOT_CONTINUE|" + reason);
}

std::string Responses::state(const std::string& debug) {
    return prefix("RES_STATE|" + debug);
}

std::string Responses::pong() {
    return prefix("RES_PONG");
}

//
// ---- ERROR RESPONSES ----
//

std::string Responses::error_unexpected_state() {
    return prefix("RES_ERROR|Unexpected request in current session state");
}

std::string Responses::error_invalid_magic() {
    return prefix("RES_ERROR|Invalid protocol magic");
}

std::string Responses::error_invalid_move() {
    return prefix("RES_ERROR|Invalid or unknown move");
}

std::string Responses::error_not_in_lobby() {
    return prefix("RES_ERROR|Player not in lobby");
}

std::string Responses::error_lobby_full() {
    return prefix("RES_ERROR|Lobby already has 2 players");
}

std::string Responses::error_lobby_not_found() {
    return prefix("RES_ERROR|Lobby not found");
}

std::string Responses::error_unknown_request() {
    return prefix("RES_ERROR|Unknown or malformed request");
}

std::string Responses::error_not_in_game() {
    return prefix("RES_ERROR|Player not currently in a game");
}

std::string Responses::error_rematch_not_allowed() {
    return prefix("RES_ERROR|Rematch only allowed after match completion");
}

std::string Responses::error_malformed_request() {
    return prefix("RES_ERROR|Malformed request syntax");
}

std::string Responses::error(const std::string& msg) {
    return prefix("RES_ERROR|" + msg);
}

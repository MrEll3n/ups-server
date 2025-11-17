#pragma once
#include <string>
#include <vector>

// ---------- Request Types ----------

enum class RequestType {
    Login,
    Logout,
    CreateLobby,
    JoinLobby,
    LeaveLobby,
    Move,
    Rematch,
    Ping,
    Unknown
};

struct Request {
    RequestType type = RequestType::Unknown;
    std::vector<std::string> params;
};

// ---------- Functions ----------
std::vector<std::string> split(const std::string& s, char delim);

RequestType request_type_from_string(const std::string& s);

Request parse_request_line(const std::string& line);

void handle_request(int fd, const Request& req);

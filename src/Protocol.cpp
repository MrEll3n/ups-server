#include "Protocol.hpp"

#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

// Protocol magic prefix required on all incoming and outgoing messages.
static constexpr const char* PROTOCOL_MAGIC = "MRLLN";


// ============================================================================
//  Helper: split
// ============================================================================

/**
 * @brief Splits a string into segments separated by a delimiter.
 *
 * @param s The input string to split.
 * @param delim The delimiter character.
 * @return std::vector<std::string> A list of substrings.
 *
 * @note Empty segments are preserved:
 *       split("A||B", '|') → ["A", "", "B"]
 */
std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::string current;

    for (char ch : s) {
        if (ch == delim) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(current);
    return parts;
}


// ============================================================================
//  Mapping REQ_* strings to RequestType enum
// ============================================================================

/**
 * @brief Converts a textual REQ_* descriptor into a RequestType enum.
 *
 * @param s The request type string, e.g. "REQ_LOGIN".
 * @return RequestType Matching type, or RequestType::Unknown if not recognized.
 */
RequestType request_type_from_string(const std::string& s) {
    if (s == "REQ_LOGIN")         return RequestType::Login;
    if (s == "REQ_LOGOUT")        return RequestType::Logout;
    if (s == "REQ_CREATE_LOBBY")  return RequestType::CreateLobby;
    if (s == "REQ_JOIN_LOBBY")    return RequestType::JoinLobby;
    if (s == "REQ_LEAVE_LOBBY")   return RequestType::LeaveLobby;
    if (s == "REQ_MOVE")          return RequestType::Move;
    if (s == "REQ_REMATCH")       return RequestType::Rematch;
    if (s == "REQ_PING")          return RequestType::Ping;

    return RequestType::Unknown;
}


// ============================================================================
//  Parsing incoming MRLLN|REQ_... messages
// ============================================================================

/**
 * @brief Parses a raw protocol line into a structured Request.
 *
 * Expected input format:
 *    MRLLN|REQ_TYPE|param1|param2|...
 *
 * @param line A complete line received from the client (without trailing '\n').
 * @return Request Parsed request. If invalid or missing magic,
 *         returns Request with RequestType::Unknown.
 *
 * @note This function is STRICT: magic (MRLLN) is required.
 * @note Only REQ_* messages are accepted from clients.
 */
Request parse_request_line(const std::string& line) {
    Request req;

    auto parts = split(line, '|');
    if (parts.size() < 2) {
        return req; // Unknown
    }

    // Magic prefix required
    if (parts[0] != PROTOCOL_MAGIC) {
        return req;
    }

    const std::string& type_desc = parts[1];

    // Must start with REQ_
    if (type_desc.rfind("REQ_", 0) != 0) {
        return req;
    }

    req.type = request_type_from_string(type_desc);

    // Remaining segments are parameters
    for (std::size_t i = 2; i < parts.size(); ++i) {
        req.params.push_back(parts[i]);
    }

    return req;
}


// ============================================================================
//  Sending messages to client (always with MRLLN prefix)
// ============================================================================

/**
 * @brief Sends a protocol message to the client with magic prefix.
 *
 * Builds and sends:
 *    MRLLN|<body>\n
 *
 * @param fd Socket file descriptor.
 * @param body The message body, e.g. "RES_LOGIN_OK|123".
 *
 * @note This function logs errors using perror().
 */
static void send_with_magic(int fd, const std::string& body) {
    std::string msg;
    msg.reserve(6 + body.size() + 1);

    msg.append(PROTOCOL_MAGIC);
    msg.push_back('|');
    msg.append(body);
    msg.push_back('\n');

    ssize_t n = ::send(fd, msg.data(), msg.size(), 0);
    if (n < 0) {
        perror("send");
    }
}


// ============================================================================
//  Core request handler
// ============================================================================

/**
 * @brief Processes one parsed Request and sends an appropriate protocol response.
 *
 * @param fd Socket file descriptor of the client.
 * @param req The parsed Request object.
 *
 * This function handles:
 *   - REQ_LOGIN
 *   - REQ_LOGOUT
 *   - REQ_CREATE_LOBBY
 *   - REQ_JOIN_LOBBY
 *   - REQ_LEAVE_LOBBY
 *   - REQ_MOVE
 *   - REQ_REMATCH
 *   - REQ_PING
 *
 * @note Full lobby/game logic will be implemented in the next stage.
 * @note Unknown requests trigger EVT_ERROR.
 */
void handle_request(int fd, const Request& req) {
    switch (req.type) {

        case RequestType::Login: {
            if (req.params.size() < 1) {
                send_with_magic(fd, "EVT_ERROR|400|Missing username");
                return;
            }

            const std::string& username = req.params[0];

            // TODO: Implement real user authentication
            int fakeUserId = 1;

            std::cout << "Login request from fd=" << fd
                      << " username=" << username << "\n";

            send_with_magic(fd, "RES_LOGIN_OK|" + std::to_string(fakeUserId));
            break;
        }

        case RequestType::Logout: {
            send_with_magic(fd, "RES_LOGOUT_OK");
            break;
        }

        case RequestType::CreateLobby: {
            if (req.params.size() < 1) {
                send_with_magic(fd, "EVT_ERROR|400|Missing lobby_name");
                return;
            }

            const std::string& lobby_name = req.params[0];

            std::cout << "Create lobby request from fd=" << fd
                      << " lobby_name=" << lobby_name << "\n";

            // TODO: Implement lobby creation
            int fakeLobbyId = 1;

            send_with_magic(fd, "RES_LOBBY_CREATED|" + std::to_string(fakeLobbyId));
            break;
        }

        case RequestType::JoinLobby: {
            if (req.params.size() < 2) {
                send_with_magic(fd, "EVT_ERROR|400|Missing username or lobby_name");
                return;
            }

            const std::string& username   = req.params[0];
            const std::string& lobby_name = req.params[1];

            std::cout << "Join lobby request from fd=" << fd
                      << " user=" << username
                      << " lobby=" << lobby_name << "\n";

            // TODO: Implement real membership logic
            std::string fakeMembers = username;

            send_with_magic(fd,
                "RES_LOBBY_JOINED|" + lobby_name + "|" + fakeMembers);
            break;
        }

        case RequestType::LeaveLobby: {
            send_with_magic(fd, "RES_LOBBY_LEFT");
            break;
        }

        case RequestType::Move: {
            if (req.params.size() < 1) {
                send_with_magic(fd, "EVT_ERROR|400|Missing move");
                return;
            }

            const std::string& move = req.params[0];

            std::cout << "Move request from fd=" << fd
                      << " move=" << move << "\n";

            // TODO: Implement round/game evaluation
            send_with_magic(fd, "RES_MOVE_ACCEPTED");
            break;
        }

        case RequestType::Rematch: {
            std::cout << "Rematch request from fd=" << fd << "\n";

            // TODO: Implement real rematch logic
            int fakeMatchId = 1;

            send_with_magic(fd,
                "RES_REMATCH_ACCEPTED|" + std::to_string(fakeMatchId));
            break;
        }

        case RequestType::Ping: {
            send_with_magic(fd, "RES_PONG");
            break;
        }

        case RequestType::Unknown:
        default: {
            send_with_magic(fd, "EVT_ERROR|400|Unknown request");
            break;
        }
    }
}

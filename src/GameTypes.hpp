#pragma once

#include <string>

enum class MoveType {
    NONE,
    ROCK,
    PAPER,
    SCISSORS
};

inline std::string move_to_string(MoveType m) {
    switch (m) {
        case MoveType::ROCK: return "R";
        case MoveType::PAPER: return "P";
        case MoveType::SCISSORS: return "S";
        default: return "";
    }
}

inline bool string_to_move(const std::string& s, MoveType& out) {
    if (s == "R") { out = MoveType::ROCK; return true; }
    if (s == "P") { out = MoveType::PAPER; return true; }
    if (s == "S") { out = MoveType::SCISSORS; return true; }
    return false;
}

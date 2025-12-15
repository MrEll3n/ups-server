#pragma once
#include <string>

enum class Move {
    None,
    Rock,
    Paper,
    Scissors
};

enum class PlayerState {
    Connected,
    LoggedIn,
    InLobby,
};

struct MatchResult {
    int winnerUserId; // 0 = draw
    int p1Wins;
    int p2Wins;
};

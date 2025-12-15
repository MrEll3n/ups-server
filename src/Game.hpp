#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include "GameTypes.hpp"

enum class LobbyPhase {
    WaitingForOpponent,
    InGame,
    AfterMatch
};

struct Player {
    int id;                     // internal player id
    std::string username;
    PlayerState state = PlayerState::Connected;

    int lobbyId = -1;           // -1 = no lobby
    Move currentMove = Move::None;
    bool wantsRematch = false;
};

struct Lobby {
    int id;
    std::string name;
    std::vector<int> players;   // 2 players only

    LobbyPhase phase = LobbyPhase::WaitingForOpponent;

    // Best-of-3 scoreboard
    int p1Wins = 0;
    int p2Wins = 0;
    int roundsPlayed = 0;
};

struct RoundResult {
    int winnerUserId = 0; // 0 = draw
    Move p1Move = Move::None;
    Move p2Move = Move::None;
    int p1UserId = 0;
    int p2UserId = 0;
};

class Game {
public:
    Game() = default;

    // Player management
    int addPlayer(const std::string& username);
    void removePlayer(int playerId);

    // Lobby management
    int createLobby(int ownerPlayerId, const std::string& name);
    bool joinLobby(int playerId, const std::string& lobbyName);
    void leaveLobby(int playerId);

    // Gameplay
    std::optional<RoundResult> submitMove(int playerId, Move move);
    bool requestRematch(int playerId);

    std::optional<MatchResult> checkMatchEnd(int lobbyId);

    // Access helpers
    const Player* getPlayer(int playerId) const;
    Player* getPlayer(int playerId);

    const Lobby* getLobby(int lobbyId) const;
    Lobby* getLobby(int lobbyId);

private:
    std::unordered_map<int, Player> players;
    std::unordered_map<int, Lobby> lobbies;

    int nextPlayerId = 1;
    int nextLobbyId = 1;

    int evaluateRound(Move a, Move b) const; // 0 draw, 1 first wins, 2 second wins
};

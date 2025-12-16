#pragma once

#include "GameTypes.hpp"

#include <unordered_map>
#include <vector>
#include <optional>

struct Player {
    int userId;
    std::string username;
};

struct Lobby {
    int lobbyId;
    std::string name;
    std::vector<Player> players; // size 0..2

    bool inGame{false};
    bool matchJustEnded{false}; // Indikuje, že zápas právě skončil

    MoveType p1Move{MoveType::NONE};
    MoveType p2Move{MoveType::NONE};

    int p1Wins{0};
    int p2Wins{0};
    int roundsPlayed{0};

    bool p1Rematch{false};
    bool p2Rematch{false};
};

class Game {
public:
    int addPlayer(const std::string& username);
    void removePlayer(int userId);

    std::optional<int> createLobby(int userId, const std::string& lobbyName);
    bool joinLobby(int userId, const std::string& lobbyName);
    void leaveLobby(int userId);

    std::optional<Lobby*> getLobbyOf(int userId);

    bool canStartGame(Lobby* lobby) const;
    void startGame(Lobby* lobby);

    bool submitMove(int userId, MoveType move,
                    int& outRoundWinnerUserId,
                    MoveType& outP1Move,
                    MoveType& outP2Move,
                    bool& outMatchEnded,
                    int& outMatchWinnerUserId,
                    int& outP1Wins,
                    int& outP2Wins);

    bool requestRematch(int userId, Lobby* lobby);
    bool canStartRematch(Lobby* lobby) const;
    void startRematch(Lobby* lobby);

private:
    std::unordered_map<int, Player> players;
    std::unordered_map<int, Lobby> lobbies;

    int nextUserId{1};
    int nextLobbyId{1};

    int evaluate_round(MoveType p1, MoveType p2) const;
    bool checkMatchEnd(Lobby* lobby, int& outWinnerUserId) const;
};

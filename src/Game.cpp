#include "Game.hpp"
#include "GameTypes.hpp"

int Game::addPlayer(const std::string& username) {
    int id = nextUserId++;
    players[id] = Player{id, username};
    return id;
}

void Game::removePlayer(int userId) {
    // Remove player from any lobby
    auto lobbyOpt = getLobbyOf(userId);
    if (lobbyOpt.has_value()) {
        leaveLobby(userId);
    }
    players.erase(userId);
}

std::optional<int> Game::createLobby(int userId, const std::string& lobbyName) {
    auto lobbyOpt = getLobbyOf(userId);
    if (lobbyOpt.has_value()) return std::nullopt; // already in a lobby

    Lobby lobby;
    lobby.lobbyId = nextLobbyId++;
    lobby.name = lobbyName;
    lobby.players.push_back(players.at(userId));
    lobbies[lobby.lobbyId] = lobby;
    return lobby.lobbyId;
}

bool Game::joinLobby(int userId, const std::string& lobbyName) {
    auto lobbyOpt = getLobbyOf(userId);
    if (lobbyOpt.has_value()) return false; // already in a lobby

    for (auto& kv : lobbies) {
        Lobby& lobby = kv.second;
        if (lobby.name == lobbyName) {
            if (lobby.players.size() >= 2) return false;
            lobby.players.push_back(players.at(userId));
            return true;
        }
    }
    return false;
}

void Game::leaveLobby(int userId) {
    for (auto it = lobbies.begin(); it != lobbies.end(); ++it) {
        Lobby& lobby = it->second;
        for (size_t i = 0; i < lobby.players.size(); i++) {
            if (lobby.players[i].userId == userId) {
                lobby.players.erase(lobby.players.begin() + static_cast<long>(i));
                // If lobby empty, erase it
                if (lobby.players.empty()) {
                    lobbies.erase(it);
                } else {
                    // Reset game state when someone leaves
                    lobby.inGame = false;
                    lobby.matchJustEnded = false;
                    lobby.p1Move = MoveType::NONE;
                    lobby.p2Move = MoveType::NONE;
                    lobby.p1Wins = 0;
                    lobby.p2Wins = 0;
                    lobby.roundsPlayed = 0;
                    lobby.p1Rematch = false;
                    lobby.p2Rematch = false;
                }
                return;
            }
        }
    }
}

std::optional<Lobby*> Game::getLobbyOf(int userId) {
    for (auto& kv : lobbies) {
        Lobby& lobby = kv.second;
        for (auto& p : lobby.players) {
            if (p.userId == userId) return &lobby;
        }
    }
    return std::nullopt;
}

bool Game::canStartGame(Lobby* lobby) const {
    return lobby && lobby->players.size() == 2 && !lobby->inGame;
}

void Game::startGame(Lobby* lobby) {
    if (!lobby) return;
    lobby->inGame = true;
    lobby->matchJustEnded = false;
    lobby->p1Move = MoveType::NONE;
    lobby->p2Move = MoveType::NONE;
    lobby->p1Wins = 0;
    lobby->p2Wins = 0;
    lobby->roundsPlayed = 0;
    lobby->p1Rematch = false;
    lobby->p2Rematch = false;
}

int Game::evaluate_round(MoveType p1, MoveType p2) const {
    // returns: 0 draw, 1 p1 wins, 2 p2 wins
    if (p1 == p2) return 0;
    if (p1 == MoveType::ROCK && p2 == MoveType::SCISSORS) return 1;
    if (p1 == MoveType::PAPER && p2 == MoveType::ROCK) return 1;
    if (p1 == MoveType::SCISSORS && p2 == MoveType::PAPER) return 1;
    return 2;
}

bool Game::checkMatchEnd(Lobby* lobby, int& outWinnerUserId) const {
    if (!lobby) return false;

    // Fixed 3-round match: always play exactly 3 rounds, then decide winner by total wins.
    if (lobby->roundsPlayed == 3) {
        if (lobby->p1Wins > lobby->p2Wins) outWinnerUserId = lobby->players[0].userId;
        else if (lobby->p2Wins > lobby->p1Wins) outWinnerUserId = lobby->players[1].userId;
        else outWinnerUserId = 0;
        return true;
    }
    return false;
}

bool Game::submitMove(int userId, MoveType move,
                      int& outRoundWinnerUserId,
                      MoveType& outP1Move,
                      MoveType& outP2Move,
                      bool& outMatchEnded,
                      int& outMatchWinnerUserId,
                      int& outP1Wins,
                      int& outP2Wins) {

    outRoundWinnerUserId = 0;
    outMatchEnded = false;
    outMatchWinnerUserId = 0;

    auto lobbyOpt = getLobbyOf(userId);
    if (!lobbyOpt.has_value()) return false;
    Lobby* lobby = lobbyOpt.value();
    if (!lobby || !lobby->inGame) return false;
    if (lobby->players.size() != 2) return false;

    const int p1Id = lobby->players[0].userId;
    const int p2Id = lobby->players[1].userId;

    // CRITICAL: Check if player already submitted a move this round
    if (userId == p1Id) {
        if (lobby->p1Move != MoveType::NONE) {
            // Player 1 already submitted - reject
            return false;
        }
        lobby->p1Move = move;
    } else if (userId == p2Id) {
        if (lobby->p2Move != MoveType::NONE) {
            // Player 2 already submitted - reject
            return false;
        }
        lobby->p2Move = move;
    } else {
        return false;
    }

    // If both moves are in, resolve round
    if (lobby->p1Move != MoveType::NONE && lobby->p2Move != MoveType::NONE) {
        int winner = evaluate_round(lobby->p1Move, lobby->p2Move);

        if (winner == 1) lobby->p1Wins++;
        else if (winner == 2) lobby->p2Wins++;

        lobby->roundsPlayed++;

        // Winner userId (0 for draw)
        if (winner == 1) outRoundWinnerUserId = p1Id;
        else if (winner == 2) outRoundWinnerUserId = p2Id;
        else outRoundWinnerUserId = 0;

        outP1Move = lobby->p1Move;
        outP2Move = lobby->p2Move;

        // Reset moves for next round
        lobby->p1Move = MoveType::NONE;
        lobby->p2Move = MoveType::NONE;

        // Match end?
        int matchWinner = 0;
        if (checkMatchEnd(lobby, matchWinner)) {
            outMatchEnded = true;
            outMatchWinnerUserId = matchWinner;
            outP1Wins = lobby->p1Wins;
            outP2Wins = lobby->p2Wins;
            lobby->inGame = false;
            lobby->matchJustEnded = true; // PŘECHOD DO AFTER_GAME STAVU
        } else {
            outP1Wins = lobby->p1Wins;
            outP2Wins = lobby->p2Wins;
        }
    }

    return true;
}

bool Game::requestRematch(int userId, Lobby* lobby) {
    if (!lobby) return false;
    if (lobby->players.size() != 2) return false;
    if (lobby->inGame) return false;
    if (!lobby->matchJustEnded) return false; // Povoleno jen v AFTER_GAME

    const int p1Id = lobby->players[0].userId;
    const int p2Id = lobby->players[1].userId;

    if (userId == p1Id) lobby->p1Rematch = true;
    else if (userId == p2Id) lobby->p2Rematch = true;
    else return false;

    return true;
}

bool Game::canStartRematch(Lobby* lobby) const {
    if (!lobby) return false;
    if (lobby->players.size() != 2) return false;
    if (!lobby->matchJustEnded) return false; // Kontrola stavu AFTER_GAME
    return lobby->p1Rematch && lobby->p2Rematch;
}

void Game::startRematch(Lobby* lobby) {
    startGame(lobby);
}

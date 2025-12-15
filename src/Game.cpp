#include "Game.hpp"
#include <algorithm>

int Game::addPlayer(const std::string& username) {
    int id = nextPlayerId++;
    Player p;
    p.id = id;
    p.username = username;
    p.state = PlayerState::LoggedIn;
    players[id] = p;
    return id;
}

void Game::removePlayer(int playerId) {
    auto it = players.find(playerId);
    if (it == players.end()) return;

    if (it->second.lobbyId != -1) {
        leaveLobby(playerId);
    }
    players.erase(it);
}

int Game::createLobby(int ownerPlayerId, const std::string& name) {
    int id = nextLobbyId++;
    Lobby lobby;
    lobby.id = id;
    lobby.name = name;
    lobby.players.clear();
    lobby.players.push_back(ownerPlayerId);

    lobby.phase = LobbyPhase::WaitingForOpponent;
    lobby.p1Wins = 0;
    lobby.p2Wins = 0;
    lobby.roundsPlayed = 0;

    lobbies[id] = lobby;

    Player* p = getPlayer(ownerPlayerId);
    if (p) {
        p->lobbyId = id;
        p->state = PlayerState::InLobby;
        p->currentMove = Move::None;
        p->wantsRematch = false;
    }
    return id;
}

bool Game::joinLobby(int playerId, const std::string& lobbyName) {
    int foundId = -1;
    for (auto& [id, lobby] : lobbies) {
        if (lobby.name == lobbyName) {
            foundId = id;
            break;
        }
    }
    if (foundId == -1) return false;

    Lobby& lobby = lobbies[foundId];
    if (lobby.players.size() >= 2) return false;

    lobby.players.push_back(playerId);

    // If lobby is now full, start match in a clean state
    if (lobby.players.size() == 2) {
        lobby.phase = LobbyPhase::InGame;
        lobby.p1Wins = 0;
        lobby.p2Wins = 0;
        lobby.roundsPlayed = 0;

        // Reset both players round state
        Player* p1 = getPlayer(lobby.players[0]);
        Player* p2 = getPlayer(lobby.players[1]);
        if (p1) { p1->currentMove = Move::None; p1->wantsRematch = false; }
        if (p2) { p2->currentMove = Move::None; p2->wantsRematch = false; }
    }

    Player* p = getPlayer(playerId);
    if (p) {
        p->lobbyId = foundId;
        p->state = PlayerState::InLobby;
        p->currentMove = Move::None;
        p->wantsRematch = false;
    }
    return true;
}

void Game::leaveLobby(int playerId) {
    Player* leaving = getPlayer(playerId);
    if (!leaving || leaving->lobbyId == -1) {
        return;
    }

    int lobbyId = leaving->lobbyId;

    auto lit = lobbies.find(lobbyId);
    if (lit != lobbies.end()) {
        Lobby& lobby = lit->second;

        lobby.players.erase(
            std::remove(lobby.players.begin(), lobby.players.end(), playerId),
            lobby.players.end()
        );

        // In your design: if anyone leaves, the lobby is destroyed and remaining player is kicked out.
        if (!lobby.players.empty()) {
            for (int otherId : lobby.players) {
                Player* other = getPlayer(otherId);
                if (other) {
                    other->lobbyId = -1;
                    other->state = PlayerState::LoggedIn;
                    other->currentMove = Move::None;
                    other->wantsRematch = false;
                }
            }
        }

        lobbies.erase(lit);
    }

    leaving->lobbyId = -1;
    leaving->state = PlayerState::LoggedIn;
    leaving->currentMove = Move::None;
    leaving->wantsRematch = false;
}

std::optional<RoundResult> Game::submitMove(int playerId, Move move) {
    Player* p = getPlayer(playerId);
    if (!p || p->lobbyId == -1) return std::nullopt;

    Lobby* lobby = getLobby(p->lobbyId);
    if (!lobby) return std::nullopt;

    // Must be exactly 2 players and match must be running
    if (lobby->players.size() != 2) return std::nullopt;
    if (lobby->phase != LobbyPhase::InGame) return std::nullopt;

    p->currentMove = move;
    p->wantsRematch = false;

    Player* p1 = getPlayer(lobby->players[0]);
    Player* p2 = getPlayer(lobby->players[1]);
    if (!p1 || !p2) return std::nullopt;

    // Wait for both moves
    if (p1->currentMove == Move::None || p2->currentMove == Move::None)
        return std::nullopt;

    int res = evaluateRound(p1->currentMove, p2->currentMove);

    RoundResult rr;
    rr.p1Move = p1->currentMove;
    rr.p2Move = p2->currentMove;
    rr.p1UserId = p1->id;
    rr.p2UserId = p2->id;

    if (res == 1) {
        rr.winnerUserId = p1->id;
        lobby->p1Wins++;
    }
    else if (res == 2) {
        rr.winnerUserId = p2->id;
        lobby->p2Wins++;
    }
    else {
        rr.winnerUserId = 0; // draw
    }

    lobby->roundsPlayed++;

    // Reset moves for the next round
    p1->currentMove = Move::None;
    p2->currentMove = Move::None;

    return rr;
}

bool Game::requestRematch(int playerId) {
    Player* p = getPlayer(playerId);
    if (!p || p->lobbyId == -1) return false;

    Lobby* lobby = getLobby(p->lobbyId);
    if (!lobby || lobby->players.size() != 2) return false;

    // Rematch only after match finished
    if (lobby->phase != LobbyPhase::AfterMatch) {
        return false;
    }

    p->wantsRematch = true;

    Player* p1 = getPlayer(lobby->players[0]);
    Player* p2 = getPlayer(lobby->players[1]);
    if (!p1 || !p2) return false;

    if (!(p1->wantsRematch && p2->wantsRematch)) {
        return false;
    }

    // Full deterministic reset
    lobby->p1Wins = 0;
    lobby->p2Wins = 0;
    lobby->roundsPlayed = 0;
    lobby->phase = LobbyPhase::InGame;

    p1->currentMove = Move::None;
    p2->currentMove = Move::None;
    p1->wantsRematch = false;
    p2->wantsRematch = false;

    return true;
}

const Player* Game::getPlayer(int playerId) const {
    auto it = players.find(playerId);
    if (it == players.end()) return nullptr;
    return &it->second;
}

Player* Game::getPlayer(int playerId) {
    auto it = players.find(playerId);
    if (it == players.end()) return nullptr;
    return &it->second;
}

const Lobby* Game::getLobby(int lobbyId) const {
    auto it = lobbies.find(lobbyId);
    if (it == lobbies.end()) return nullptr;
    return &it->second;
}

Lobby* Game::getLobby(int lobbyId) {
    auto it = lobbies.find(lobbyId);
    if (it == lobbies.end()) return nullptr;
    return &it->second;
}

int Game::evaluateRound(Move a, Move b) const {
    if (a == b) return 0;
    if ((a == Move::Rock     && b == Move::Scissors) ||
        (a == Move::Scissors && b == Move::Paper)    ||
        (a == Move::Paper    && b == Move::Rock)) {
        return 1;
    }
    return 2;
}

std::optional<MatchResult> Game::checkMatchEnd(int lobbyId) {
    Lobby* lobby = getLobby(lobbyId);
    if (!lobby) return std::nullopt;

    if (lobby->phase != LobbyPhase::InGame) {
        return std::nullopt;
    }

    // Best-of-3: first to 2 wins OR max 3 rounds
    if (lobby->p1Wins == 2 || lobby->p2Wins == 2 || lobby->roundsPlayed == 3) {
        MatchResult mr;

        if (lobby->p1Wins > lobby->p2Wins) mr.winnerUserId = lobby->players[0];
        else if (lobby->p2Wins > lobby->p1Wins) mr.winnerUserId = lobby->players[1];
        else mr.winnerUserId = 0;

        mr.p1Wins = lobby->p1Wins;
        mr.p2Wins = lobby->p2Wins;

        lobby->phase = LobbyPhase::AfterMatch;
        return mr;
    }

    return std::nullopt;
}

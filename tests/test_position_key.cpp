#include "pgn_eco/chess.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace gambitree;

static std::string legacy_key(const Position& position)
{
    std::istringstream input(position.to_fen());
    std::string board, side, castle, ep;
    if (!(input >> board >> side >> castle >> ep)) return std::string();
    bool legal_en_passant = false;
    if (ep != "-") {
        const std::vector<Move> moves = position.legal_moves();
        for (std::size_t i = 0; i < moves.size(); ++i)
            if (moves[i].flags & MOVE_EN_PASSANT) {
                legal_en_passant = true;
                break;
            }
    }
    if (!legal_en_passant) ep = "-";
    return board + " " + side + " " + castle + " " + ep;
}

static bool compare(const Position& position, const char* label)
{
    const std::string old_key = legacy_key(position);
    const std::string new_key = position.opening_key();
    if (old_key == new_key) return true;
    std::cerr << label << ": opening key mismatch\nold: " << old_key
              << "\nnew: " << new_key << '\n';
    return false;
}

static bool play(const char* label, const std::vector<std::string>& sans)
{
    Position position = Position::standard();
    if (!compare(position, label)) return false;
    for (std::size_t i = 0; i < sans.size(); ++i) {
        Move move;
        Undo undo;
        if (!position.parse_san_and_make(sans[i], move, undo)) {
            std::cerr << label << ": test move was not legal: " << sans[i] << '\n';
            return false;
        }
        if (!compare(position, label)) return false;
    }
    return true;
}

int main()
{
    std::vector<std::string> opening;
    opening.push_back("e4"); opening.push_back("e5");
    opening.push_back("Nf3"); opening.push_back("Nc6");
    opening.push_back("Bb5"); opening.push_back("a6");
    opening.push_back("Ba4"); opening.push_back("Nf6");
    opening.push_back("O-O");
    if (!play("opening and castling", opening)) return 1;

    std::vector<std::string> en_passant;
    en_passant.push_back("d4"); en_passant.push_back("a6");
    en_passant.push_back("d5"); en_passant.push_back("e5");
    if (!play("legal en-passant right", en_passant)) return 1;

    Position pinned;
    std::string error;
    if (!Position::from_fen("k3r3/8/8/3pP3/8/8/8/4K3 w - d6 0 1",
                            pinned, &error)) {
        std::cerr << "could not load pinned en-passant fixture: " << error << '\n';
        return 1;
    }
    if (!compare(pinned, "pinned en-passant right") ||
        pinned.opening_key().substr(pinned.opening_key().size() - 2) != " -") {
        std::cerr << "pinned en-passant right was not discarded\n";
        return 1;
    }

    Position legal;
    if (!Position::from_fen("k7/8/8/3pP3/8/8/8/4K3 w - d6 0 1",
                            legal, &error)) {
        std::cerr << "could not load legal en-passant fixture: " << error << '\n';
        return 1;
    }
    if (!compare(legal, "legal en-passant FEN") ||
        legal.opening_key().substr(legal.opening_key().size() - 3) != " d6") {
        std::cerr << "legal en-passant right was not preserved: " << legal.to_fen()
                  << " -> " << legal.opening_key() << '\n';
        return 1;
    }
    return 0;
}

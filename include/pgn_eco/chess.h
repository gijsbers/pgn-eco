#ifndef GAMBITREE_CHESS_H
#define GAMBITREE_CHESS_H

#include <string>
#include <vector>

namespace gambitree {

enum Color { WHITE, BLACK };
enum ChessVariant { STANDARD_CHESS, CHESS960 };
enum PieceType { NO_PIECE, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };
enum Piece {
    EMPTY = 0,
    WHITE_PAWN = 1, WHITE_KNIGHT, WHITE_BISHOP, WHITE_ROOK, WHITE_QUEEN, WHITE_KING,
    BLACK_PAWN = -1, BLACK_KNIGHT = -2, BLACK_BISHOP = -3, BLACK_ROOK = -4,
    BLACK_QUEEN = -5, BLACK_KING = -6
};
enum CastlingRight { WHITE_KINGSIDE = 1, WHITE_QUEENSIDE = 2,
                     BLACK_KINGSIDE = 4, BLACK_QUEENSIDE = 8 };
enum MoveFlag { MOVE_NORMAL = 0, MOVE_CAPTURE = 1, MOVE_EN_PASSANT = 2,
                MOVE_CASTLE_KINGSIDE = 4, MOVE_CASTLE_QUEENSIDE = 8,
                MOVE_DOUBLE_PAWN = 16, MOVE_PROMOTION = 32 };

struct Move {
    int from;
    int to;
    PieceType promotion;
    unsigned int flags;
    Move();
    Move(int source, int destination, PieceType promoted_piece, unsigned int move_flags);
};

struct Undo {
    int board[64];
    Color side_to_move;
    unsigned int castling_rights;
    int en_passant_square;
    int halfmove_clock;
    int fullmove_number;
    ChessVariant variant;
    int king_home[2];
    int rook_home[4];
};

class Position {
public:
    Position();

    static Position standard(ChessVariant variant = STANDARD_CHESS);
    static bool from_fen(const std::string& fen, Position& position, std::string* error,
                         ChessVariant variant = STANDARD_CHESS);
    std::string to_fen() const;
    /* Canonical opening lookup key: FEN placement, mover, rights, legal EP. */
    std::string opening_key() const;

    int piece_at(int square) const;
    Color side_to_move() const;
    ChessVariant variant() const;
    unsigned int castling_rights() const;
    int en_passant_square() const;
    int halfmove_clock() const;
    int fullmove_number() const;

    static int square_from_string(const std::string& square);
    static std::string square_to_string(int square);

    std::vector<Move> pseudo_legal_moves() const;
    std::vector<Move> legal_moves() const;
    bool is_square_attacked(int square, Color by_color) const;
    bool in_check(Color color) const;
    bool is_checkmate() const;
    bool is_stalemate() const;

    bool make_move(const Move& requested, Undo& undo);
    /* Apply a PGN analytical null move without changing board occupancy or
       castling rights. The null move is rejected while the mover is in check. */
    bool make_null_move(Undo& undo);
    void unmake_move(const Undo& undo);

    std::string san(const Move& move) const;
    bool parse_san(const std::string& text, Move& move) const;
    /* Resolve SAN among all legal moves to its target and apply the unique
       match. Candidate filtering still checks self-check and SAN disambiguation. */
    bool parse_san_and_make(const std::string& text, Move& move, Undo& undo);
    bool move_from_uci(int from, int to, PieceType promotion, Move& move) const;
    bool equals(const Position& other) const;

private:
    int board_[64];
    Color side_to_move_;
    ChessVariant variant_;
    unsigned int castling_rights_;
    int king_home_[2];
    int rook_home_[4];
    int en_passant_square_;
    int halfmove_clock_;
    int fullmove_number_;

    void apply_unchecked(const Move& move);
    bool castle_legal(unsigned int right) const;
    std::string san_from_legal_moves(const Move& move,
                                    const std::vector<Move>& legal,
                                    bool include_check_suffix) const;
    bool find_san_move(const std::string& text,
                       const std::vector<Move>& legal,
                       Move& move) const;
    void save(Undo& undo) const;
    static Color opposite(Color color);
    static bool is_valid_square(int square);
    static Color color_of(int piece);
    static PieceType type_of(int piece);
    static int make_piece(Color color, PieceType type);
};

}

#endif

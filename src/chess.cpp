#include "pgn_eco/chess.h"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <sstream>

namespace gambitree {

namespace {

bool on_board(int file, int rank)
{
    return file >= 0 && file < 8 && rank >= 0 && rank < 8;
}

int square_of(int file, int rank)
{
    return rank * 8 + file;
}

char piece_letter(PieceType type)
{
    if (type == KNIGHT) return 'N';
    if (type == BISHOP) return 'B';
    if (type == ROOK) return 'R';
    if (type == QUEEN) return 'Q';
    if (type == KING) return 'K';
    return '\0';
}

char fen_letter(int piece)
{
    static const char letters[] = " PNBRQK";
    char letter = letters[std::abs(piece)];
    return piece < 0 ? static_cast<char>(std::tolower(letter)) : letter;
}

int fen_piece(char letter)
{
    Color color = std::isupper(static_cast<unsigned char>(letter)) ? WHITE : BLACK;
    char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
    PieceType type = NO_PIECE;
    if (upper == 'P') type = PAWN;
    else if (upper == 'N') type = KNIGHT;
    else if (upper == 'B') type = BISHOP;
    else if (upper == 'R') type = ROOK;
    else if (upper == 'Q') type = QUEEN;
    else if (upper == 'K') type = KING;
    return type == NO_PIECE ? EMPTY : (color == WHITE ? type : -type);
}

bool san_origin_matches(const std::string& supplied,
                        const std::string& canonical, const Move& move)
{
    if (move.from < 0 || move.from >= 64 ||
        (move.flags & (MOVE_CASTLE_KINGSIDE | MOVE_CASTLE_QUEENSIDE)))
        return false;
    if (supplied.empty() || canonical.empty() || supplied[0] != canonical[0] ||
        std::string("KQRBN").find(supplied[0]) == std::string::npos)
        return false;
    std::size_t supplied_end = supplied.size();
    std::size_t canonical_end = canonical.size();
    if (supplied_end > 1 && (supplied[supplied_end - 1] == '+' ||
                             supplied[supplied_end - 1] == '#')) --supplied_end;
    if (canonical_end > 1 && (canonical[canonical_end - 1] == '+' ||
                              canonical[canonical_end - 1] == '#')) --canonical_end;
    const bool supplied_suffix = supplied_end != supplied.size();
    const bool canonical_suffix = canonical_end != canonical.size();
    if (supplied_suffix != canonical_suffix ||
        (supplied_suffix && supplied[supplied_end] != canonical[canonical_end])) return false;
    const std::string supplied_core = supplied.substr(0, supplied_end);
    const std::string canonical_core = canonical.substr(0, canonical_end);
    if (supplied_core.size() <= 2 || canonical_core.size() <= 1) return false;
    std::size_t canonical_origin = 0;
    const std::size_t body_after_piece = 1;
    const std::size_t n = canonical_core.size();
    const bool one_char_origin = n > 3 &&
        (((canonical_core[body_after_piece] >= 'a' && canonical_core[body_after_piece] <= 'h') ||
          (canonical_core[body_after_piece] >= '1' && canonical_core[body_after_piece] <= '8')) &&
         ((canonical_core[body_after_piece + 1] == 'x' && body_after_piece + 3 < n) ||
          (canonical_core[body_after_piece + 1] >= 'a' && canonical_core[body_after_piece + 1] <= 'h' &&
           canonical_core[body_after_piece + 2] >= '1' && canonical_core[body_after_piece + 2] <= '8')));
    if (one_char_origin) canonical_origin = 1;
    const bool two_char_origin = !one_char_origin && n > 4 &&
        canonical_core[1] >= 'a' && canonical_core[1] <= 'h' &&
        canonical_core[2] >= '1' && canonical_core[2] <= '8' &&
        ((canonical_core[3] == 'x' && n > 5) ||
         (canonical_core[3] >= 'a' && canonical_core[3] <= 'h' &&
          canonical_core[4] >= '1' && canonical_core[4] <= '8'));
    if (two_char_origin) canonical_origin = 2;
    const std::string canonical_move_body =
        canonical_core.substr(1 + canonical_origin);
    const int source_file = move.from % 8;
    const int source_rank = move.from / 8;
    for (int extra = 1; extra <= 2; ++extra) {
        if (supplied_core.size() <= static_cast<std::size_t>(1 + extra) ||
            supplied_core.substr(static_cast<std::size_t>(1 + extra)) !=
                canonical_move_body) continue;
        bool origin_matches = false;
        if (extra == 1) {
            const char origin = supplied_core[1];
            origin_matches = (origin >= 'a' && origin <= 'h' &&
                              origin - 'a' == source_file) ||
                             (origin >= '1' && origin <= '8' &&
                              origin - '1' == source_rank);
        } else {
            origin_matches = supplied_core[1] >= 'a' && supplied_core[1] <= 'h' &&
                             supplied_core[2] >= '1' && supplied_core[2] <= '8' &&
                             supplied_core[1] - 'a' == source_file &&
                             supplied_core[2] - '1' == source_rank;
        }
        if (origin_matches) return true;
    }
    return false;
}

bool san_missing_check_suffix_matches(const std::string& supplied,
                                      const std::string& canonical)
{
    return canonical.size() == supplied.size() + 1 &&
           !supplied.empty() &&
           canonical.compare(0, supplied.size(), supplied) == 0 &&
           (canonical[canonical.size() - 1] == '+' ||
            canonical[canonical.size() - 1] == '#');
}

struct SanShape {
    bool castle;
    unsigned int castle_flag;
    int destination;
    PieceType piece;
    bool promotion_given;
    PieceType promotion;
    SanShape() : castle(false), castle_flag(0), destination(-1), piece(NO_PIECE),
        promotion_given(false), promotion(NO_PIECE) {}
};

bool san_square(const std::string& text, int at, int& square)
{
    if (at < 0 || static_cast<std::size_t>(at + 1) >= text.size() ||
        text[at] < 'a' || text[at] > 'h' ||
        text[at + 1] < '1' || text[at + 1] > '8') return false;
    square = (text[at + 1] - '1') * 8 + text[at] - 'a';
    return true;
}

bool parse_san_shape(const std::string& supplied, SanShape& shape)
{
    std::string text = supplied;
    if (!text.empty() && (text[text.size() - 1] == '+' ||
                          text[text.size() - 1] == '#')) text.erase(text.size() - 1);
    if (text == "O-O") {
        shape.castle = true; shape.castle_flag = MOVE_CASTLE_KINGSIDE;
        return true;
    }
    if (text == "O-O-O") {
        shape.castle = true; shape.castle_flag = MOVE_CASTLE_QUEENSIDE;
        return true;
    }
    if (text.size() >= 4 && text[text.size() - 2] == '=') {
        const char promotion = text[text.size() - 1];
        if (promotion == 'Q') shape.promotion = QUEEN;
        else if (promotion == 'R') shape.promotion = ROOK;
        else if (promotion == 'B') shape.promotion = BISHOP;
        else if (promotion == 'N') shape.promotion = KNIGHT;
        else return false;
        shape.promotion_given = true;
        text.erase(text.size() - 2);
    }
    if (text.size() < 2 || !san_square(text, static_cast<int>(text.size()) - 2,
                                       shape.destination)) return false;
    std::string prefix = text.substr(0, text.size() - 2);
    if (!prefix.empty() && std::string("KQRBN").find(prefix[0]) != std::string::npos) {
        const char piece = prefix[0];
        shape.piece = piece == 'K' ? KING : piece == 'Q' ? QUEEN :
            piece == 'R' ? ROOK : piece == 'B' ? BISHOP : KNIGHT;
        prefix.erase(0, 1);
    } else {
        shape.piece = PAWN;
    }
    std::string origin;
    std::size_t capture = prefix.find('x');
    if (capture != std::string::npos) {
        if (prefix.find('x', capture + 1) != std::string::npos) return false;
        prefix.erase(capture, 1);
    }
    origin = prefix;
    if (origin.size() > 2) return false;
    for (std::size_t i = 0; i < origin.size(); ++i) {
        const char c = origin[i];
        if (!((c >= 'a' && c <= 'h') || (c >= '1' && c <= '8'))) return false;
    }
    if (shape.piece == PAWN) {
        if (origin.size() > 1 || (!origin.empty() &&
            (origin[0] < 'a' || origin[0] > 'h'))) return false;
    } else if (origin.size() == 2 &&
               (origin[0] < 'a' || origin[0] > 'h' ||
                origin[1] < '1' || origin[1] > '8')) return false;
    return true;
}

}

Move::Move() : from(-1), to(-1), promotion(NO_PIECE), flags(MOVE_NORMAL) {}
Move::Move(int source, int destination, PieceType promoted_piece, unsigned int move_flags)
    : from(source), to(destination), promotion(promoted_piece), flags(move_flags) {}

Position::Position() : side_to_move_(WHITE), variant_(STANDARD_CHESS), castling_rights_(0), en_passant_square_(-1),
                       halfmove_clock_(0), fullmove_number_(1)
{
    int i;
    for (i = 0; i < 64; ++i) board_[i] = EMPTY;
    king_home_[WHITE] = 4; king_home_[BLACK] = 60;
    rook_home_[0] = 7; rook_home_[1] = 0;
    rook_home_[2] = 63; rook_home_[3] = 56;
}

Position Position::standard(ChessVariant variant)
{
    Position position;
    std::string ignored;
    from_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
             position, &ignored, variant);
    return position;
}

bool Position::from_fen(const std::string& fen, Position& position, std::string* error,
                        ChessVariant variant)
{
    std::istringstream input(fen);
    std::string placement, side, castling, ep, halfmove, fullmove, extra;
    Position parsed;
    int rank = 7;
    int file = 0;
    std::size_t i;
    if (!(input >> placement >> side >> castling >> ep >> halfmove >> fullmove) || (input >> extra)) {
        if (error) *error = "FEN must contain exactly six fields";
        return false;
    }
    parsed.variant_ = variant;
    for (i = 0; i < placement.size(); ++i) {
        char c = placement[i];
        if (c == '/') {
            if (file != 8 || rank == 0) { if (error) *error = "invalid board layout"; return false; }
            --rank; file = 0;
        } else if (c >= '1' && c <= '8') {
            file += c - '0';
            if (file > 8) { if (error) *error = "rank is too wide"; return false; }
        } else {
            int piece = fen_piece(c);
            if (piece == EMPTY || file >= 8) { if (error) *error = "invalid piece placement"; return false; }
            parsed.board_[square_of(file, rank)] = piece;
            ++file;
        }
    }
    if (rank != 0 || file != 8) { if (error) *error = "board must contain eight ranks"; return false; }
    int white_kings = 0;
    int black_kings = 0;
    for (i = 0; i < 64; ++i) {
        if (parsed.board_[i] == WHITE_KING) ++white_kings;
        if (parsed.board_[i] == BLACK_KING) ++black_kings;
    }
    if (white_kings != 1 || black_kings != 1) { if (error) *error = "position must contain one king of each color"; return false; }
    if (side == "w") parsed.side_to_move_ = WHITE;
    else if (side == "b") parsed.side_to_move_ = BLACK;
    else { if (error) *error = "invalid side to move"; return false; }
    /* Locate each king on its back rank. Material elsewhere in the position
       is deliberately unrestricted; only claimed castling rights are strict. */
    parsed.king_home_[WHITE] = -1;
    parsed.king_home_[BLACK] = -1;
    for (i = 0; i < 8; ++i) {
        if (parsed.board_[i] == WHITE_KING) parsed.king_home_[WHITE] = static_cast<int>(i);
        if (parsed.board_[56 + i] == BLACK_KING) parsed.king_home_[BLACK] = 56 + static_cast<int>(i);
    }
    if (parsed.king_home_[WHITE] < 0 || parsed.king_home_[BLACK] < 0) {
        /* A king may be away from the back rank after moves in an arbitrary
           FEN.  It simply cannot retain a castling right there. */
        for (i = 0; i < 64; ++i) {
            if (parsed.board_[i] == WHITE_KING) parsed.king_home_[WHITE] = static_cast<int>(i);
            if (parsed.board_[i] == BLACK_KING) parsed.king_home_[BLACK] = static_cast<int>(i);
        }
    }
    if (castling != "-") {
        for (i = 0; i < castling.size(); ++i) {
            char c = castling[i];
            Color color = std::isupper(static_cast<unsigned char>(c)) ? WHITE : BLACK;
            int base = color == WHITE ? 0 : 56;
            int king = parsed.king_home_[color];
            int king_file = king >= 0 ? king % 8 : -1;
            int rook_file = -1;
            unsigned int right = 0;
            int right_index = -1;
            bool symbolic = c == 'K' || c == 'Q' || c == 'k' || c == 'q';
            if (variant == STANDARD_CHESS) {
                if (c == 'K') { right = WHITE_KINGSIDE; right_index = 0; rook_file = 7; }
                else if (c == 'Q') { right = WHITE_QUEENSIDE; right_index = 1; rook_file = 0; }
                else if (c == 'k') { right = BLACK_KINGSIDE; right_index = 2; rook_file = 7; }
                else if (c == 'q') { right = BLACK_QUEENSIDE; right_index = 3; rook_file = 0; }
                if (right && king != base + 4) {
                    if (error) *error = "castling rights do not match orthodox king position";
                    return false;
                }
            } else if (symbolic) {
                bool king_side = c == 'K' || c == 'k';
                right = color == WHITE ? (king_side ? WHITE_KINGSIDE : WHITE_QUEENSIDE) :
                                         (king_side ? BLACK_KINGSIDE : BLACK_QUEENSIDE);
                right_index = color == WHITE ? (king_side ? 0 : 1) : (king_side ? 2 : 3);
                /* X-FEN K/Q names the outer rook on the corresponding side. */
                int candidate;
                for (candidate = 0; candidate < 8; ++candidate) {
                    if (parsed.board_[base + candidate] != make_piece(color, ROOK)) continue;
                    if (king_side && candidate > king_file && candidate > rook_file)
                        rook_file = candidate;
                    if (!king_side && candidate < king_file &&
                        (rook_file < 0 || candidate < rook_file)) rook_file = candidate;
                }
            } else if ((color == WHITE && c >= 'A' && c <= 'H') ||
                       (color == BLACK && c >= 'a' && c <= 'h')) {
                rook_file = color == WHITE ? c - 'A' : c - 'a';
                if (king_file < 0 || rook_file == king_file) {
                    if (error) *error = "ambiguous Chess960 castling rook file";
                    return false;
                }
                bool king_side = rook_file > king_file;
                right = color == WHITE ? (king_side ? WHITE_KINGSIDE : WHITE_QUEENSIDE) :
                                         (king_side ? BLACK_KINGSIDE : BLACK_QUEENSIDE);
                right_index = color == WHITE ? (king_side ? 0 : 1) : (king_side ? 2 : 3);
            }
            if (right == 0 || right_index < 0 ||
                (parsed.castling_rights_ & right) != 0 || rook_file < 0 ||
                king < base || king >= base + 8 ||
                parsed.board_[base + rook_file] != make_piece(color, ROOK)) {
                if (error) *error = "castling rights do not match king and rook positions";
                return false;
            }
            parsed.castling_rights_ |= right;
            parsed.rook_home_[right_index] = base + rook_file;
        }
    }
    if (variant == STANDARD_CHESS) {
        if (((parsed.castling_rights_ & WHITE_KINGSIDE) && parsed.board_[7] != WHITE_ROOK) ||
            ((parsed.castling_rights_ & WHITE_QUEENSIDE) && parsed.board_[0] != WHITE_ROOK) ||
            ((parsed.castling_rights_ & BLACK_KINGSIDE) && parsed.board_[63] != BLACK_ROOK) ||
            ((parsed.castling_rights_ & BLACK_QUEENSIDE) && parsed.board_[56] != BLACK_ROOK)) {
            if (error) *error = "castling rights do not match orthodox rook position";
            return false;
        }
    }
    if (ep != "-") {
        parsed.en_passant_square_ = square_from_string(ep);
        if (parsed.en_passant_square_ < 0 || (ep[1] != '3' && ep[1] != '6')) { if (error) *error = "invalid en-passant square"; return false; }
    }
    char* end = 0;
    errno = 0;
    long half = std::strtol(halfmove.c_str(), &end, 10);
    if (*halfmove.c_str() == '\0' || *end != '\0' || errno == ERANGE || half < 0 || half > INT_MAX) { if (error) *error = "invalid halfmove clock"; return false; }
    errno = 0;
    long full = std::strtol(fullmove.c_str(), &end, 10);
    if (*fullmove.c_str() == '\0' || *end != '\0' || errno == ERANGE || full < 1 || full > INT_MAX) { if (error) *error = "invalid fullmove number"; return false; }
    parsed.halfmove_clock_ = static_cast<int>(half);
    parsed.fullmove_number_ = static_cast<int>(full);
    position = parsed;
    return true;
}

std::string Position::to_fen() const
{
    std::ostringstream output;
    int rank, file;
    for (rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (file = 0; file < 8; ++file) {
            int piece = board_[square_of(file, rank)];
            if (piece == EMPTY) ++empty;
            else { if (empty) { output << empty; empty = 0; } output << fen_letter(piece); }
        }
        if (empty) output << empty;
        if (rank) output << '/';
    }
    output << (side_to_move_ == WHITE ? " w " : " b ");
    if (!castling_rights_) output << '-';
    else if (variant_ == STANDARD_CHESS) {
        if (castling_rights_ & WHITE_KINGSIDE) output << 'K';
        if (castling_rights_ & WHITE_QUEENSIDE) output << 'Q';
        if (castling_rights_ & BLACK_KINGSIDE) output << 'k';
        if (castling_rights_ & BLACK_QUEENSIDE) output << 'q';
    } else {
        int file;
        for (file = 0; file < 8; ++file) {
            if ((castling_rights_ & WHITE_KINGSIDE) && rook_home_[0] % 8 == file) output << static_cast<char>('A' + file);
            if ((castling_rights_ & WHITE_QUEENSIDE) && rook_home_[1] % 8 == file) output << static_cast<char>('A' + file);
        }
        for (file = 0; file < 8; ++file) {
            if ((castling_rights_ & BLACK_KINGSIDE) && rook_home_[2] % 8 == file) output << static_cast<char>('a' + file);
            if ((castling_rights_ & BLACK_QUEENSIDE) && rook_home_[3] % 8 == file) output << static_cast<char>('a' + file);
        }
    }
    output << ' ' << (en_passant_square_ < 0 ? "-" : square_to_string(en_passant_square_));
    output << ' ' << halfmove_clock_ << ' ' << fullmove_number_;
    return output.str();
}

std::string Position::opening_key() const
{
    std::string key;
    key.reserve(80);
    int rank, file;
    for (rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (file = 0; file < 8; ++file) {
            const int piece = board_[square_of(file, rank)];
            if (piece == EMPTY) ++empty;
            else {
                if (empty) {
                    key += static_cast<char>('0' + empty);
                    empty = 0;
                }
                key += fen_letter(piece);
            }
        }
        if (empty) key += static_cast<char>('0' + empty);
        if (rank) key += '/';
    }
    key += side_to_move_ == WHITE ? " w " : " b ";
    if (!castling_rights_) key += '-';
    else if (variant_ == STANDARD_CHESS) {
        if (castling_rights_ & WHITE_KINGSIDE) key += 'K';
        if (castling_rights_ & WHITE_QUEENSIDE) key += 'Q';
        if (castling_rights_ & BLACK_KINGSIDE) key += 'k';
        if (castling_rights_ & BLACK_QUEENSIDE) key += 'q';
    } else {
        for (file = 0; file < 8; ++file) {
            if ((castling_rights_ & WHITE_KINGSIDE) && rook_home_[0] % 8 == file)
                key += static_cast<char>('A' + file);
            if ((castling_rights_ & WHITE_QUEENSIDE) && rook_home_[1] % 8 == file)
                key += static_cast<char>('A' + file);
        }
        for (file = 0; file < 8; ++file) {
            if ((castling_rights_ & BLACK_KINGSIDE) && rook_home_[2] % 8 == file)
                key += static_cast<char>('a' + file);
            if ((castling_rights_ & BLACK_QUEENSIDE) && rook_home_[3] % 8 == file)
                key += static_cast<char>('a' + file);
        }
    }
    key += ' ';
    bool legal_en_passant = false;
    if (en_passant_square_ >= 0) {
        const std::vector<Move> moves = legal_moves();
        for (std::size_t i = 0; i < moves.size(); ++i) {
            if (moves[i].flags & MOVE_EN_PASSANT) {
                legal_en_passant = true;
                break;
            }
        }
    }
    key += legal_en_passant ? square_to_string(en_passant_square_) : "-";
    return key;
}

int Position::piece_at(int square) const { return is_valid_square(square) ? board_[square] : EMPTY; }
Color Position::side_to_move() const { return side_to_move_; }
ChessVariant Position::variant() const { return variant_; }
unsigned int Position::castling_rights() const { return castling_rights_; }
int Position::en_passant_square() const { return en_passant_square_; }
int Position::halfmove_clock() const { return halfmove_clock_; }
int Position::fullmove_number() const { return fullmove_number_; }

int Position::square_from_string(const std::string& square)
{
    if (square.size() != 2 || square[0] < 'a' || square[0] > 'h' || square[1] < '1' || square[1] > '8') return -1;
    return square_of(square[0] - 'a', square[1] - '1');
}
std::string Position::square_to_string(int square)
{
    std::string result;
    if (!is_valid_square(square)) return result;
    result += static_cast<char>('a' + square % 8);
    result += static_cast<char>('1' + square / 8);
    return result;
}
Color Position::opposite(Color color) { return color == WHITE ? BLACK : WHITE; }
bool Position::is_valid_square(int square) { return square >= 0 && square < 64; }
Color Position::color_of(int piece) { return piece > 0 ? WHITE : BLACK; }
PieceType Position::type_of(int piece) { return static_cast<PieceType>(std::abs(piece)); }
int Position::make_piece(Color color, PieceType type) { return color == WHITE ? type : -type; }

bool Position::castle_legal(unsigned int right) const
{
    Color color = (right == WHITE_KINGSIDE || right == WHITE_QUEENSIDE) ? WHITE : BLACK;
    int base = color == WHITE ? 0 : 56;
    int right_index = right == WHITE_KINGSIDE ? 0 : right == WHITE_QUEENSIDE ? 1 :
                      right == BLACK_KINGSIDE ? 2 : 3;
    bool king_side = right == WHITE_KINGSIDE || right == BLACK_KINGSIDE;
    int king_from = king_home_[color];
    int rook_from = rook_home_[right_index];
    int king_to = base + (king_side ? 6 : 2);
    int rook_to = base + (king_side ? 5 : 3);
    int low, high, file;
    Position probe;
    int transit_from;

    if (!(castling_rights_ & right) || king_from < base || king_from >= base + 8 ||
        rook_from < base || rook_from >= base + 8 || board_[king_from] != make_piece(color, KING) ||
        board_[rook_from] != make_piece(color, ROOK) || in_check(color)) return false;

    /* Both start pieces are lifted for the clearance test. This correctly
       permits Chess960 overlaps such as a destination being the other piece's
       start square. */
    low = king_from % 8 < king_to % 8 ? king_from % 8 : king_to % 8;
    high = king_from % 8 > king_to % 8 ? king_from % 8 : king_to % 8;
    for (file = low; file <= high; ++file) {
        int square = base + file;
        if (square != king_from && square != rook_from && board_[square] != EMPTY)
            return false;
    }
    low = rook_from % 8 < rook_to % 8 ? rook_from % 8 : rook_to % 8;
    high = rook_from % 8 > rook_to % 8 ? rook_from % 8 : rook_to % 8;
    for (file = low; file <= high; ++file) {
        int square = base + file;
        if (square != king_from && square != rook_from && board_[square] != EMPTY)
            return false;
    }

    /* Test every king transit square after lifting both castling pieces.
       This is essential when the rook's start or final square overlaps the
       king's route and when the king already starts on c/g. */
    probe = *this;
    probe.board_[king_from] = EMPTY;
    probe.board_[rook_from] = EMPTY;
    transit_from = king_from % 8;
    if (transit_from == king_to % 8) {
        probe.board_[king_to] = make_piece(color, KING);
        if (probe.is_square_attacked(king_to, opposite(color))) return false;
        probe.board_[king_to] = EMPTY;
    } else {
        int step = king_to % 8 > transit_from ? 1 : -1;
        for (file = transit_from + step; ; file += step) {
            int square = base + file;
            probe.board_[square] = make_piece(color, KING);
            if (probe.is_square_attacked(square, opposite(color))) return false;
            probe.board_[square] = EMPTY;
            if (file == king_to % 8) break;
        }
    }
    (void)rook_to;
    return true;
}

std::vector<Move> Position::pseudo_legal_moves() const
{
    std::vector<Move> moves;
    int square;
    for (square = 0; square < 64; ++square) {
        int piece = board_[square];
        int file, rank, df, target;
        PieceType type;
        if (piece == EMPTY || color_of(piece) != side_to_move_) continue;
        file = square % 8; rank = square / 8; type = type_of(piece);
        if (type == PAWN) {
            int direction = side_to_move_ == WHITE ? 1 : -1;
            int start_rank = side_to_move_ == WHITE ? 1 : 6;
            int promotion_rank = side_to_move_ == WHITE ? 7 : 0;
            int next_rank = rank + direction;
            if (on_board(file, next_rank) && board_[square_of(file, next_rank)] == EMPTY) {
                target = square_of(file, next_rank);
                if (next_rank == promotion_rank) {
                    PieceType promotions[] = { QUEEN, ROOK, BISHOP, KNIGHT };
                    int p; for (p = 0; p < 4; ++p) moves.push_back(Move(square, target, promotions[p], MOVE_PROMOTION));
                } else moves.push_back(Move(square, target, NO_PIECE, MOVE_NORMAL));
                if (rank == start_rank && board_[square_of(file, rank + 2 * direction)] == EMPTY)
                    moves.push_back(Move(square, square_of(file, rank + 2 * direction), NO_PIECE, MOVE_DOUBLE_PAWN));
            }
            for (df = -1; df <= 1; df += 2) {
                if (!on_board(file + df, next_rank)) continue;
                target = square_of(file + df, next_rank);
                if (board_[target] != EMPTY && color_of(board_[target]) != side_to_move_ && type_of(board_[target]) != KING) {
                    if (next_rank == promotion_rank) {
                        PieceType promotions[] = { QUEEN, ROOK, BISHOP, KNIGHT };
                        int p; for (p = 0; p < 4; ++p) moves.push_back(Move(square, target, promotions[p], MOVE_CAPTURE | MOVE_PROMOTION));
                    } else moves.push_back(Move(square, target, NO_PIECE, MOVE_CAPTURE));
                } else if (target == en_passant_square_ && board_[square_of(file + df, rank)] == make_piece(opposite(side_to_move_), PAWN)) {
                    moves.push_back(Move(square, target, NO_PIECE, MOVE_EN_PASSANT | MOVE_CAPTURE));
                }
            }
        } else if (type == KNIGHT || type == KING) {
            static const int knight_offsets[8][2] = {{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
            static const int king_offsets[8][2] = {{1,1},{1,0},{1,-1},{0,-1},{-1,-1},{-1,0},{-1,1},{0,1}};
            const int (*offsets)[2] = type == KNIGHT ? knight_offsets : king_offsets;
            int count = type == KNIGHT ? 8 : 8;
            int i;
            for (i = 0; i < count; ++i) {
                int nf = file + offsets[i][0], nr = rank + offsets[i][1];
                if (!on_board(nf, nr)) continue;
                target = square_of(nf, nr);
                if (board_[target] == EMPTY || (color_of(board_[target]) != side_to_move_ && type_of(board_[target]) != KING))
                    moves.push_back(Move(square, target, NO_PIECE, board_[target] == EMPTY ? MOVE_NORMAL : MOVE_CAPTURE));
            }
            if (type == KING) {
                unsigned int king_right = side_to_move_ == WHITE ? WHITE_KINGSIDE : BLACK_KINGSIDE;
                unsigned int queen_right = side_to_move_ == WHITE ? WHITE_QUEENSIDE : BLACK_QUEENSIDE;
                if (square == king_home_[side_to_move_] && castle_legal(king_right))
                    moves.push_back(Move(square, (side_to_move_ == WHITE ? 0 : 56) + 6,
                                         NO_PIECE, MOVE_CASTLE_KINGSIDE));
                if (square == king_home_[side_to_move_] && castle_legal(queen_right))
                    moves.push_back(Move(square, (side_to_move_ == WHITE ? 0 : 56) + 2,
                                         NO_PIECE, MOVE_CASTLE_QUEENSIDE));
            }
        } else {
            static const int bishop_dirs[4][2] = {{1,1},{1,-1},{-1,-1},{-1,1}};
            static const int rook_dirs[4][2] = {{1,0},{0,-1},{-1,0},{0,1}};
            static const int queen_dirs[8][2] = {{1,1},{1,-1},{-1,-1},{-1,1},{1,0},{0,-1},{-1,0},{0,1}};
            const int (*dirs)[2] = type == BISHOP ? bishop_dirs : (type == ROOK ? rook_dirs : queen_dirs);
            int count = type == QUEEN ? 8 : 4;
            int i;
            for (i = 0; i < count; ++i) {
                int ndx = dirs[i][0];
                int ndy = dirs[i][1];
                int nf = file + ndx, nr = rank + ndy;
                while (on_board(nf, nr)) {
                    target = square_of(nf, nr);
                    if (board_[target] == EMPTY) moves.push_back(Move(square, target, NO_PIECE, MOVE_NORMAL));
                    else { if (color_of(board_[target]) != side_to_move_ && type_of(board_[target]) != KING) moves.push_back(Move(square, target, NO_PIECE, MOVE_CAPTURE)); break; }
                    nf += ndx; nr += ndy;
                }
            }
        }
    }
    return moves;
}

bool Position::is_square_attacked(int square, Color by_color) const
{
    int file, rank, df;
    if (!is_valid_square(square)) return false;
    file = square % 8; rank = square / 8;
    int pawn_rank = rank + (by_color == WHITE ? -1 : 1);
    for (df = -1; df <= 1; df += 2)
        if (on_board(file + df, pawn_rank) && board_[square_of(file + df, pawn_rank)] == make_piece(by_color, PAWN)) return true;
    static const int knight_offsets[8][2] = {{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
    int i;
    for (i = 0; i < 8; ++i) if (on_board(file + knight_offsets[i][0], rank + knight_offsets[i][1]) && board_[square_of(file + knight_offsets[i][0], rank + knight_offsets[i][1])] == make_piece(by_color, KNIGHT)) return true;
    static const int king_offsets[8][2] = {{1,1},{1,0},{1,-1},{0,-1},{-1,-1},{-1,0},{-1,1},{0,1}};
    for (i = 0; i < 8; ++i) if (on_board(file + king_offsets[i][0], rank + king_offsets[i][1]) && board_[square_of(file + king_offsets[i][0], rank + king_offsets[i][1])] == make_piece(by_color, KING)) return true;
    static const int dirs[8][2] = {{1,1},{1,-1},{-1,-1},{-1,1},{1,0},{0,-1},{-1,0},{0,1}};
    for (i = 0; i < 8; ++i) {
        int nf = file + dirs[i][0], nr = rank + dirs[i][1];
        while (on_board(nf, nr)) {
            int piece = board_[square_of(nf, nr)];
            if (piece != EMPTY) {
                PieceType type = type_of(piece);
                if (color_of(piece) == by_color && (type == QUEEN || (i < 4 && type == BISHOP) || (i >= 4 && type == ROOK))) return true;
                break;
            }
            nf += dirs[i][0]; nr += dirs[i][1];
        }
    }
    return false;
}

bool Position::in_check(Color color) const
{
    int i; int king = make_piece(color, KING);
    for (i = 0; i < 64; ++i) if (board_[i] == king) return is_square_attacked(i, opposite(color));
    return true;
}

std::vector<Move> Position::legal_moves() const
{
    std::vector<Move> pseudo = pseudo_legal_moves(), legal;
    std::size_t i;
    for (i = 0; i < pseudo.size(); ++i) {
        Position copy = *this;
        copy.apply_unchecked(pseudo[i]);
        if (!copy.in_check(side_to_move_)) legal.push_back(pseudo[i]);
    }
    return legal;
}
bool Position::is_checkmate() const { return in_check(side_to_move_) && legal_moves().empty(); }
bool Position::is_stalemate() const { return !in_check(side_to_move_) && legal_moves().empty(); }

void Position::save(Undo& undo) const
{
    int i; for (i = 0; i < 64; ++i) undo.board[i] = board_[i];
    undo.side_to_move = side_to_move_; undo.castling_rights = castling_rights_; undo.en_passant_square = en_passant_square_;
    undo.halfmove_clock = halfmove_clock_; undo.fullmove_number = fullmove_number_;
    undo.variant = variant_;
    for (i = 0; i < 2; ++i) undo.king_home[i] = king_home_[i];
    for (i = 0; i < 4; ++i) undo.rook_home[i] = rook_home_[i];
}
void Position::apply_unchecked(const Move& move)
{
    int moving = board_[move.from];
    int captured = board_[move.to];
    Color mover = side_to_move_;
    int right_index;
    if (move.flags & (MOVE_CASTLE_KINGSIDE | MOVE_CASTLE_QUEENSIDE)) captured = EMPTY;
    en_passant_square_ = -1;
    if (type_of(moving) == PAWN || captured != EMPTY || (move.flags & MOVE_EN_PASSANT)) halfmove_clock_ = 0;
    else ++halfmove_clock_;
    if (move.flags & MOVE_EN_PASSANT) {
        board_[move.to + (mover == WHITE ? -8 : 8)] = EMPTY;
        captured = make_piece(opposite(mover), PAWN);
    }
    if (move.flags & (MOVE_CASTLE_KINGSIDE | MOVE_CASTLE_QUEENSIDE)) {
        bool king_side = (move.flags & MOVE_CASTLE_KINGSIDE) != 0;
        int base = mover == WHITE ? 0 : 56;
        right_index = mover == WHITE ? (king_side ? 0 : 1) : (king_side ? 2 : 3);
        int rook_from = rook_home_[right_index];
        int rook_to = base + (king_side ? 5 : 3);
        /* Clear both sources before placing either piece. Chess960 may have a
           king destination equal to the rook source or vice versa. */
        board_[move.from] = EMPTY;
        board_[rook_from] = EMPTY;
        board_[move.to] = moving;
        board_[rook_to] = make_piece(mover, ROOK);
    } else {
        board_[move.to] = moving; board_[move.from] = EMPTY;
        if (move.flags & MOVE_PROMOTION) board_[move.to] = make_piece(mover, move.promotion);
    }
    if (type_of(moving) == KING) castling_rights_ &= mover == WHITE ? ~(WHITE_KINGSIDE | WHITE_QUEENSIDE) : ~(BLACK_KINGSIDE | BLACK_QUEENSIDE);
    for (right_index = 0; right_index < 4; ++right_index) {
        unsigned int right = 1U << right_index;
        int captured_square = (move.flags & MOVE_EN_PASSANT) ?
            move.to + (mover == WHITE ? -8 : 8) : move.to;
        if ((castling_rights_ & right) &&
            (move.from == rook_home_[right_index] ||
             captured_square == rook_home_[right_index])) {
            castling_rights_ &= ~right;
        }
    }
    if (move.flags & MOVE_DOUBLE_PAWN) en_passant_square_ = move.from + (mover == WHITE ? 8 : -8);
    if (mover == BLACK) ++fullmove_number_;
    side_to_move_ = opposite(side_to_move_);
}
bool Position::make_move(const Move& requested, Undo& undo)
{
    std::vector<Move> moves = legal_moves();
    std::size_t i;
    for (i = 0; i < moves.size(); ++i) if (moves[i].from == requested.from && moves[i].to == requested.to && moves[i].promotion == requested.promotion &&
        (moves[i].flags & (MOVE_CASTLE_KINGSIDE | MOVE_CASTLE_QUEENSIDE)) ==
        (requested.flags & (MOVE_CASTLE_KINGSIDE | MOVE_CASTLE_QUEENSIDE))) {
        save(undo); apply_unchecked(moves[i]); return true;
    }
    return false;
}
bool Position::make_null_move(Undo& undo)
{
    if (in_check(side_to_move_)) return false;
    save(undo);
    en_passant_square_ = -1;
    ++halfmove_clock_;
    if (side_to_move_ == BLACK) ++fullmove_number_;
    side_to_move_ = opposite(side_to_move_);
    return true;
}
void Position::unmake_move(const Undo& undo)
{
    int i; for (i = 0; i < 64; ++i) board_[i] = undo.board[i];
    side_to_move_ = undo.side_to_move; castling_rights_ = undo.castling_rights; en_passant_square_ = undo.en_passant_square;
    halfmove_clock_ = undo.halfmove_clock; fullmove_number_ = undo.fullmove_number;
    variant_ = undo.variant;
    for (i = 0; i < 2; ++i) king_home_[i] = undo.king_home[i];
    for (i = 0; i < 4; ++i) rook_home_[i] = undo.rook_home[i];
}

std::string Position::san(const Move& requested) const
{
    const std::vector<Move> moves = legal_moves();
    return san_from_legal_moves(requested, moves, true);
}

std::string Position::san_from_legal_moves(const Move& requested,
                                           const std::vector<Move>& moves,
                                           bool include_check_suffix) const
{
    std::size_t i, j;
    Move move; bool found = false;
    for (i = 0; i < moves.size(); ++i) if (moves[i].from == requested.from && moves[i].to == requested.to && moves[i].promotion == requested.promotion &&
        (moves[i].flags & (MOVE_CASTLE_KINGSIDE | MOVE_CASTLE_QUEENSIDE)) ==
        (requested.flags & (MOVE_CASTLE_KINGSIDE | MOVE_CASTLE_QUEENSIDE))) { move = moves[i]; found = true; break; }
    if (!found) return std::string();
    PieceType type = type_of(board_[move.from]);
    std::string result;
    if (move.flags & MOVE_CASTLE_KINGSIDE) result = "O-O";
    else if (move.flags & MOVE_CASTLE_QUEENSIDE) result = "O-O-O";
    else {
        if (type != PAWN) {
            bool ambiguous = false, same_file = false, same_rank = false;
            result += piece_letter(type);
            for (j = 0; j < moves.size(); ++j) if (moves[j].to == move.to && moves[j].from != move.from && type_of(board_[moves[j].from]) == type) {
                ambiguous = true;
                if (moves[j].from % 8 == move.from % 8) same_file = true;
                if (moves[j].from / 8 == move.from / 8) same_rank = true;
            }
            if (ambiguous) {
                if (!same_file) result += static_cast<char>('a' + move.from % 8);
                else if (!same_rank) result += static_cast<char>('1' + move.from / 8);
                else result += square_to_string(move.from);
            }
        } else if (move.flags & MOVE_CAPTURE) result += static_cast<char>('a' + move.from % 8);
        if (move.flags & MOVE_CAPTURE) result += 'x';
        result += square_to_string(move.to);
        if (move.flags & MOVE_PROMOTION) { result += '='; result += piece_letter(move.promotion); }
    }
    if (include_check_suffix) {
        Position after = *this; after.apply_unchecked(move);
        if (after.in_check(after.side_to_move_))
            result += after.legal_moves().empty() ? '#' : '+';
    }
    return result;
}
bool Position::parse_san(const std::string& text, Move& move) const
{
    SanShape shape;
    if (parse_san_shape(text, shape)) {
        const std::vector<Move> pseudo = pseudo_legal_moves();
        std::vector<Move> candidates;
        std::size_t i;
        for (i = 0; i < pseudo.size(); ++i) {
            const Move& candidate = pseudo[i];
            if (shape.castle) {
                if (!(candidate.flags & shape.castle_flag)) continue;
            } else {
                if (candidate.to != shape.destination ||
                    type_of(board_[candidate.from]) != shape.piece ||
                    (shape.promotion_given && candidate.promotion != shape.promotion))
                    continue;
            }
            Position after = *this;
            after.apply_unchecked(candidate);
            if (!after.in_check(side_to_move_)) candidates.push_back(candidate);
        }
        return find_san_move(text, candidates, move);
    }
    const std::vector<Move> moves = legal_moves();
    return find_san_move(text, moves, move);
}

bool Position::parse_san_and_make(const std::string& text, Move& move, Undo& undo)
{
    Move selected;
    SanShape shape;
    if (parse_san_shape(text, shape)) {
        /* All legal moves to this SAN target are retained. That is sufficient
           for SAN disambiguation, and each candidate still passes the same
           self-check test as legal_moves(); unrelated destinations avoid
           their much more expensive board-copy and attack checks. */
        const std::vector<Move> pseudo = pseudo_legal_moves();
        std::vector<Move> candidates;
        std::size_t i;
        for (i = 0; i < pseudo.size(); ++i) {
            const Move& candidate = pseudo[i];
            if (shape.castle) {
                if (!(candidate.flags & shape.castle_flag)) continue;
            } else {
                if (candidate.to != shape.destination ||
                    type_of(board_[candidate.from]) != shape.piece ||
                    (shape.promotion_given && candidate.promotion != shape.promotion))
                    continue;
            }
            Position after = *this;
            after.apply_unchecked(candidate);
            if (!after.in_check(side_to_move_)) candidates.push_back(candidate);
        }
        if (!find_san_move(text, candidates, selected)) return false;
    } else {
        const std::vector<Move> moves = legal_moves();
        if (!find_san_move(text, moves, selected)) return false;
    }
    save(undo);
    apply_unchecked(selected);
    move = selected;
    return true;
}

bool Position::find_san_move(const std::string& text,
                             const std::vector<Move>& moves,
                             Move& move) const
{
    std::string supplied_base = text;
    if (!supplied_base.empty() &&
        (supplied_base[supplied_base.size() - 1] == '+' ||
         supplied_base[supplied_base.size() - 1] == '#'))
        supplied_base.erase(supplied_base.size() - 1);
    std::size_t i;
    bool found = false;
    Move match;
    for (i = 0; i < moves.size(); ++i) {
        /* Check suffixes require a reply-move generation to distinguish +
           from #. First compare the cheap SAN core so that this work is done
           only for moves that could match the supplied notation. */
        const std::string base = san_from_legal_moves(moves[i], moves, false);
        if (base != supplied_base &&
            !san_origin_matches(supplied_base, base, moves[i])) continue;
        const std::string canonical = san_from_legal_moves(moves[i], moves, true);
        if (canonical != text &&
            !san_missing_check_suffix_matches(text, canonical) &&
            !san_origin_matches(text, canonical, moves[i])) continue;
        if (found) return false;
        match = moves[i];
        found = true;
    }
    if (found) move = match;
    return found;
}
bool Position::move_from_uci(int from, int to, PieceType promotion, Move& move) const
{
    std::vector<Move> moves = legal_moves();
    std::size_t i;
    for (i = 0; i < moves.size(); ++i) {
        int uci_to = moves[i].to;
        if (variant_ == CHESS960 && (moves[i].flags & MOVE_CASTLE_KINGSIDE)) {
            int right_index = side_to_move_ == WHITE ? 0 : 2;
            uci_to = rook_home_[right_index];
        } else if (variant_ == CHESS960 && (moves[i].flags & MOVE_CASTLE_QUEENSIDE)) {
            int right_index = side_to_move_ == WHITE ? 1 : 3;
            uci_to = rook_home_[right_index];
        }
        if (moves[i].from == from && uci_to == to && moves[i].promotion == promotion) {
            move = moves[i];
            return true;
        }
    }
    return false;
}
bool Position::equals(const Position& other) const
{
    int i;
    if (side_to_move_ != other.side_to_move_ || variant_ != other.variant_ ||
        castling_rights_ != other.castling_rights_ || en_passant_square_ != other.en_passant_square_ || halfmove_clock_ != other.halfmove_clock_ || fullmove_number_ != other.fullmove_number_) return false;
    for (i = 0; i < 2; ++i) if (king_home_[i] != other.king_home_[i]) return false;
    for (i = 0; i < 4; ++i) if (rook_home_[i] != other.rook_home_[i]) return false;
    for (i = 0; i < 64; ++i) if (board_[i] != other.board_[i]) return false;
    return true;
}

}

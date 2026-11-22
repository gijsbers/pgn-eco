#ifndef GAMBITREE_PGN_H
#define GAMBITREE_PGN_H

#include <string>
#include <vector>
#include <stdint.h>

#include "pgn_eco/chess.h"

namespace gambitree {

struct PgnTag {
    std::string name;
    std::string value;
    PgnTag();
    PgnTag(const std::string& tag_name, const std::string& tag_value);
};

struct PgnError {
    int game_number;
    int line;
    int column;
    std::string message;
    PgnError();
    PgnError(int game, int error_line, int error_column, const std::string& error_message);
};

struct PgnWarning {
    int game_number;
    int line;
    int column;
    std::string message;
    PgnWarning();
    PgnWarning(int game, int warning_line, int warning_column,
               const std::string& warning_message);
};

struct MoveNode;

struct PgnMoveItem {
    enum Type { COMMENT, VARIATION };
    Type type;
    std::string comment;
    MoveNode* variation_root;
    PgnMoveItem(Type item_type, const std::string& item_comment,
                MoveNode* root)
        : type(item_type), comment(item_comment), variation_root(root) {}
};

struct MoveNode {
    MoveNode* parent;
    std::vector<MoveNode*> children;
    Move move;
    std::string san;
    std::vector<std::string> leading_comments;
    std::vector<std::string> comments;
    std::vector<int> nags;
    /* Comments and RAVs in the source order after this move. Variation roots
       are non-owning references; the regular children tree owns every node. */
    std::vector<PgnMoveItem> following_items;
    Position position_after;
    bool has_move;

    MoveNode();
    ~MoveNode();

private:
    MoveNode(const MoveNode&);
    MoveNode& operator=(const MoveNode&);
};

struct PgnTrailingItem {
    enum Type { COMMENT, HYPOTHETICAL_VARIATION };
    Type type;
    std::string comment;
    MoveNode* variation_root;
    PgnTrailingItem(Type item_type, const std::string& item_comment,
                    MoveNode* root)
        : type(item_type), comment(item_comment), variation_root(root) {}
};

class PgnGame {
public:
    std::vector<PgnTag> tags;
    MoveNode* root;
    Position initial_position;
    std::string result;
    std::vector<PgnTrailingItem> trailing_items;
    uint64_t instance_identity() const;

    PgnGame();
    ~PgnGame();
    const std::string* tag_value(const std::string& name) const;

private:
    PgnGame(const PgnGame&);
    PgnGame& operator=(const PgnGame&);
    uint64_t instance_identity_;
};

class PgnDocument {
public:
    std::vector<PgnGame*> games;
    std::vector<PgnError> errors;
    std::vector<PgnWarning> warnings;
    std::size_t skipped_variation_count;

    PgnDocument();
    ~PgnDocument();
    void clear();

private:
    PgnDocument(const PgnDocument&);
    PgnDocument& operator=(const PgnDocument&);
};

struct PgnParseTiming {
    double tokenize_ms;
    double semantic_ms;
    double san_ms;
    unsigned long validated_mainline_plies;
    unsigned long skipped_rav_san_tokens;
    unsigned long skipped_late_mainline_san_tokens;
    PgnParseTiming() : tokenize_ms(0.0), semantic_ms(0.0), san_ms(0.0),
        validated_mainline_plies(0), skipped_rav_san_tokens(0),
        skipped_late_mainline_san_tokens(0) {}
};

class PgnParser {
public:
    /* Parses all recoverable games. On an invalid game, tokens are skipped until
       the next tag pair, which is the parser's reliable resynchronization point. */
    static bool parse(const std::string& text, PgnDocument& document,
                      bool allow_initial_bom = true,
                      PgnParseTiming* timing = 0);

    /* ECO-only path. SAN is validated on the main line only through
       max_mainline_plies; later main-line tokens and every RAV are scanned
       structurally without chess analysis. The returned game stores only the
       validated opening prefix. */
    static bool parse_opening(const std::string& text, PgnDocument& document,
                              int max_mainline_plies,
                              bool allow_initial_bom = true,
                              PgnParseTiming* timing = 0);
};

}

#endif

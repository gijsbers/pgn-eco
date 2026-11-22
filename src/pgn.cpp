#include "pgn_eco/pgn.h"

#include <cctype>
#include <climits>
#include <cstdlib>
#include <pthread.h>
#include <sstream>
#include <time.h>

namespace gambitree {

namespace {
uint64_t next_game_identity = 1;
pthread_mutex_t game_identity_mutex = PTHREAD_MUTEX_INITIALIZER;

uint64_t allocate_game_identity()
{
    pthread_mutex_lock(&game_identity_mutex);
    uint64_t identity = next_game_identity++;
    if (identity == 0) identity = next_game_identity++;
    pthread_mutex_unlock(&game_identity_mutex);
    return identity;
}

double parser_clock_ms()
{
    struct timespec value;
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0.0;
    return static_cast<double>(value.tv_sec) * 1000.0 +
        static_cast<double>(value.tv_nsec) / 1000000.0;
}
}

PgnTag::PgnTag() {}
PgnTag::PgnTag(const std::string& tag_name, const std::string& tag_value)
    : name(tag_name), value(tag_value) {}
PgnError::PgnError() : game_number(0), line(0), column(0) {}
PgnError::PgnError(int game, int error_line, int error_column, const std::string& error_message)
    : game_number(game), line(error_line), column(error_column), message(error_message) {}
PgnWarning::PgnWarning() : game_number(0), line(0), column(0) {}
PgnWarning::PgnWarning(int game, int warning_line, int warning_column,
                       const std::string& warning_message)
    : game_number(game), line(warning_line), column(warning_column),
      message(warning_message) {}
MoveNode::MoveNode() : parent(0), has_move(false) {}
MoveNode::~MoveNode()
{
    std::size_t index;
    for (index = 0; index < children.size(); ++index) delete children[index];
}
PgnGame::PgnGame() : root(new MoveNode()), initial_position(Position::standard()),
    result("*"), instance_identity_(allocate_game_identity())
{}
PgnGame::~PgnGame()
{
    std::size_t index;
    delete root;
    for (index = 0; index < trailing_items.size(); ++index)
        if (trailing_items[index].type == PgnTrailingItem::HYPOTHETICAL_VARIATION)
            delete trailing_items[index].variation_root;
}
uint64_t PgnGame::instance_identity() const { return instance_identity_; }
const std::string* PgnGame::tag_value(const std::string& name) const
{
    std::size_t index;
    for (index = 0; index < tags.size(); ++index)
        if (tags[index].name == name) return &tags[index].value;
    return 0;
}
PgnDocument::PgnDocument() : skipped_variation_count(0) {}
PgnDocument::~PgnDocument() { clear(); }
void PgnDocument::clear()
{
    std::size_t index;
    for (index = 0; index < games.size(); ++index) delete games[index];
    games.clear(); errors.clear(); warnings.clear(); skipped_variation_count = 0;
}

namespace {

enum TokenType { TOKEN_TAG, TOKEN_SAN, TOKEN_MOVE_NUMBER, TOKEN_BARE_MOVE_NUMBER,
                 TOKEN_COMMENT, TOKEN_NAG,
                 TOKEN_RAV_START, TOKEN_RAV_END, TOKEN_RESULT, TOKEN_ESCAPE_LINE,
                 TOKEN_ERROR, TOKEN_END };

struct Token {
    TokenType type;
    std::string text;
    int line;
    int column;
    Token(TokenType token_type, const std::string& token_text, int token_line, int token_column)
        : type(token_type), text(token_text), line(token_line), column(token_column) {}
};

class Lexer {
public:
    explicit Lexer(const std::string& input, bool allow_initial_bom)
        : input_(input),
          offset_(allow_initial_bom && input.size() >= 3 &&
                  static_cast<unsigned char>(input[0]) == 0xef &&
                  static_cast<unsigned char>(input[1]) == 0xbb &&
                  static_cast<unsigned char>(input[2]) == 0xbf ? 3 : 0),
          line_(1), column_(1), pending_nag_(false), pending_nag_line_(1),
          pending_nag_column_(1) {}
    Token next()
    {
        if (pending_nag_) {
            pending_nag_ = false;
            return Token(TOKEN_NAG, pending_nag_text_, pending_nag_line_,
                         pending_nag_column_);
        }
        skip_space();
        if (offset_ >= input_.size()) return Token(TOKEN_END, "", line_, column_);
        int line = line_, column = column_;
        char c = input_[offset_];
        if (c == '[') return tag();
        if (c == '{') return brace_comment();
        if (c == ';') return line_comment();
        if (c == '%' && column_ == 1) return escape_line();
        if (c == '(') { advance(); return Token(TOKEN_RAV_START, "(", line, column); }
        if (c == ')') { advance(); return Token(TOKEN_RAV_END, ")", line, column); }
        if (c == '$') return nag();
        return word();
    }
private:
    const std::string& input_;
    std::size_t offset_;
    int line_;
    int column_;
    bool pending_nag_;
    std::string pending_nag_text_;
    int pending_nag_line_;
    int pending_nag_column_;
    void advance()
    {
        if (input_[offset_] == '\n') { ++line_; column_ = 1; }
        else ++column_;
        ++offset_;
    }
    void skip_space()
    {
        while (offset_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[offset_]))) advance();
    }
    Token tag()
    {
        int line = line_, column = column_;
        std::string value;
        bool quoted = false, escaped = false;
        advance();
        while (offset_ < input_.size()) {
            char c = input_[offset_];
            if (!escaped && c == '"') quoted = !quoted;
            if (!escaped && !quoted && c == ']') { advance(); return Token(TOKEN_TAG, value, line, column); }
            value += c;
            escaped = !escaped && c == '\\';
            if (c != '\\') escaped = false;
            advance();
        }
        return Token(TOKEN_ERROR, "unterminated tag pair", line, column);
    }
    Token brace_comment()
    {
        int line = line_, column = column_;
        std::string value;
        advance();
        while (offset_ < input_.size() && input_[offset_] != '}') { value += input_[offset_]; advance(); }
        if (offset_ >= input_.size()) return Token(TOKEN_ERROR, "unterminated brace comment", line, column);
        advance();
        return Token(TOKEN_COMMENT, value, line, column);
    }
    Token line_comment()
    {
        int line = line_, column = column_;
        std::string value;
        advance();
        while (offset_ < input_.size() && input_[offset_] != '\n') { value += input_[offset_]; advance(); }
        return Token(TOKEN_COMMENT, value, line, column);
    }
    Token escape_line()
    {
        int line = line_, column = column_;
        while (offset_ < input_.size() && input_[offset_] != '\n') advance();
        return Token(TOKEN_ESCAPE_LINE, "", line, column);
    }
    Token nag()
    {
        int line = line_, column = column_;
        std::string value;
        advance();
        while (offset_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[offset_]))) { value += input_[offset_]; advance(); }
        if (value.empty()) return Token(TOKEN_ERROR, "NAG requires a numeric value", line, column);
        return Token(TOKEN_NAG, value, line, column);
    }
    Token word()
    {
        int line = line_, column = column_;
        std::string value;
        while (offset_ < input_.size()) {
            char c = input_[offset_];
            if (std::isspace(static_cast<unsigned char>(c)) || c == '[' || c == ']' || c == '{' || c == '}' || c == '(' || c == ')' || c == ';' || c == '$') break;
            value += c; advance();
        }
        if (value.empty()) { advance(); return Token(TOKEN_ERROR, "unexpected character", line, column); }
        if (value == "1-0" || value == "0-1" || value == "1/2-1/2" || value == "*") return Token(TOKEN_RESULT, value, line, column);
        if (value == "!" || value == "?" || value == "!!" || value == "??" || value == "!?" || value == "?!")
            return Token(TOKEN_NAG, value, line, column);
        static const char* evaluation_suffixes[] = { "+/-", "-/+", "+-", "-+" };
        static const int evaluation_nags[] = { 16, 17, 18, 19 };
        std::size_t evaluation_index;
        for (evaluation_index = 0; evaluation_index < 4; ++evaluation_index) {
            const std::string suffix(evaluation_suffixes[evaluation_index]);
            if (value.size() > suffix.size() &&
                value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0) {
                value.erase(value.size() - suffix.size());
                pending_nag_ = true;
                std::ostringstream nag_text;
                nag_text << evaluation_nags[evaluation_index];
                pending_nag_text_ = nag_text.str();
                pending_nag_line_ = line;
                pending_nag_column_ = column + static_cast<int>(value.size());
                break;
            }
        }
        std::size_t annotation_start = value.size();
        while (annotation_start > 0 &&
               (value[annotation_start - 1] == '!' ||
                value[annotation_start - 1] == '?'))
            --annotation_start;
        if (!pending_nag_ && annotation_start > 0 && annotation_start < value.size()) {
            std::string annotation = value.substr(annotation_start);
            if (annotation == "!" || annotation == "?" || annotation == "!!" ||
                annotation == "??" || annotation == "!?" || annotation == "?!") {
                value.erase(annotation_start);
                pending_nag_ = true;
                pending_nag_text_ = annotation;
                pending_nag_line_ = line;
                pending_nag_column_ = column + static_cast<int>(annotation_start);
            }
        }
        std::size_t index = 0;
        while (index < value.size() && std::isdigit(static_cast<unsigned char>(value[index]))) ++index;
        if (index == value.size()) return Token(TOKEN_BARE_MOVE_NUMBER, value, line, column);
        if (index > 0 && index < value.size()) {
            std::size_t dots = index;
            while (dots < value.size() && value[dots] == '.') ++dots;
            std::size_t dot_count = dots - index;
            if (dots == value.size() && dot_count > 0) return Token(TOKEN_MOVE_NUMBER, value, line, column);
            if ((dot_count == 1 || dot_count == 3) && dots < value.size())
                return Token(TOKEN_SAN, value.substr(dots), line, column + static_cast<int>(dots));
        }
        return Token(TOKEN_SAN, value, line, column);
    }
};

Token next_opening_token(Lexer& lexer, PgnParseTiming* timing)
{
    const double start = timing ? parser_clock_ms() : 0.0;
    Token token = lexer.next();
    if (timing) timing->tokenize_ms += parser_clock_ms() - start;
    return token;
}

bool parse_tag(const Token& token, PgnTag& tag)
{
    std::size_t index = 0;
    while (index < token.text.size() && std::isspace(static_cast<unsigned char>(token.text[index]))) ++index;
    std::size_t start = index;
    while (index < token.text.size() && !std::isspace(static_cast<unsigned char>(token.text[index]))) ++index;
    if (start == index) return false;
    tag.name = token.text.substr(start, index - start);
    while (index < token.text.size() && std::isspace(static_cast<unsigned char>(token.text[index]))) ++index;
    if (index >= token.text.size() || token.text[index] != '"') return false;
    ++index;
    tag.value.clear();
    bool escaped = false;
    for (; index < token.text.size(); ++index) {
        char c = token.text[index];
        if (escaped) { tag.value += c; escaped = false; }
        else if (c == '\\') escaped = true;
        else if (c == '"') { ++index; break; }
        else tag.value += c;
    }
    if (index > token.text.size() || (index == token.text.size() && (token.text.empty() || token.text[index - 1] != '"'))) return false;
    while (index < token.text.size() && std::isspace(static_cast<unsigned char>(token.text[index]))) ++index;
    return index == token.text.size();
}

struct LineState {
    MoveNode* current;
    Position position;
    std::vector<std::string> leading_comments;
    bool variation_start;
    bool post_result_analysis;
    LineState(MoveNode* node, const Position& value, bool is_variation = false,
              bool is_post_result_analysis = false)
        : current(node), position(value), variation_start(is_variation),
          post_result_analysis(is_post_result_analysis) {}
};

struct VariationFrame {
    Token opening;
    MoveNode* post_result_root;
    MoveNode* source_anchor;
    explicit VariationFrame(const Token& start, MoveNode* root = 0,
                           MoveNode* anchor = 0)
        : opening(start), post_result_root(root), source_anchor(anchor) {}
};

const std::size_t maximum_stored_variation_warnings = 10;

void add_skipped_variation_warning(PgnDocument& document, int game_number,
                                   const Token& token, const std::string& message)
{
    ++document.skipped_variation_count;
    if (document.warnings.size() < maximum_stored_variation_warnings)
        document.warnings.push_back(PgnWarning(game_number, token.line,
                                               token.column, message));
}

bool find_variation_end(const std::vector<Token>& tokens, std::size_t start,
                        std::size_t& after_end)
{
    std::size_t depth = 1;
    std::size_t index;
    for (index = start; index < tokens.size(); ++index) {
        if (tokens[index].type == TOKEN_RAV_START) ++depth;
        else if (tokens[index].type == TOKEN_RAV_END) {
            --depth;
            if (depth == 0) {
                after_end = index + 1;
                return true;
            }
        } else if (tokens[index].type == TOKEN_ERROR ||
                   tokens[index].type == TOKEN_TAG ||
                   tokens[index].type == TOKEN_RESULT ||
                   tokens[index].type == TOKEN_END) {
            return false;
        }
    }
    return false;
}

void add_error(PgnDocument& document, int game, const Token& token, const std::string& message)
{
    document.errors.push_back(PgnError(game, token.line, token.column, message));
}

bool configure_start(PgnGame& game, PgnDocument& document, int game_number, const Token& location)
{
    const std::string* setup = game.tag_value("SetUp");
    const std::string* fen = game.tag_value("FEN");
    const std::string* variant_tag = game.tag_value("Variant");
    const std::string* result = game.tag_value("Result");
    ChessVariant variant = STANDARD_CHESS;
    if (variant_tag) {
        std::string name = *variant_tag;
        std::string normalized;
        std::size_t i;
        for (i = 0; i < name.size(); ++i) {
            if (!std::isspace(static_cast<unsigned char>(name[i])))
                normalized += static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
        }
        if (normalized == "chess960" || normalized == "fischerandom" ||
            normalized == "fischerrandom") variant = CHESS960;
        else if (normalized != "standard" && normalized != "chess") {
            add_error(document, game_number, location,
                      "unsupported PGN Variant \"" + *variant_tag + "\"");
            return false;
        }
    }
    game.initial_position = Position::standard(variant);
    if (result && (*result == "1-0" || *result == "0-1" || *result == "1/2-1/2" || *result == "*"))
        game.result = *result;
    if (setup && *setup == "1") {
        std::string error;
        if (!fen) { add_error(document, game_number, location, "SetUp \"1\" requires a FEN tag"); return false; }
        if (!Position::from_fen(*fen, game.initial_position, &error, variant)) { add_error(document, game_number, location, "invalid FEN: " + error); return false; }
    }
    return true;
}

bool parse_opening_game(const std::string& text, PgnDocument& document,
                        int max_mainline_plies, bool allow_initial_bom,
                        PgnParseTiming* timing)
{
    PgnGame* game = new PgnGame();
    Lexer lexer(text, allow_initial_bom);
    Token token = next_opening_token(lexer, timing);
    while (token.type == TOKEN_TAG) {
        PgnTag tag;
        if (!parse_tag(token, tag)) {
            add_error(document, 1, token, "malformed tag pair");
            delete game;
            return false;
        }
        game->tags.push_back(tag);
        token = next_opening_token(lexer, timing);
    }
    const Token location = token;
    if (max_mainline_plies < 0) {
        add_error(document, 1, location, "negative ECO opening horizon");
        delete game;
        return false;
    }
    if (!configure_start(*game, document, 1, location)) {
        delete game;
        return false;
    }

    Position position = game->initial_position;
    MoveNode* parent = game->root;
    parent->position_after = position;
    std::vector<bool> rav_has_move;
    bool mainline_has_move = false;
    bool result_seen = false;
    bool saw_content = !game->tags.empty();
    int analyzed_plies = 0;

    while (token.type != TOKEN_END) {
        if (token.type == TOKEN_ERROR) {
            add_error(document, 1, token, token.text);
            delete game;
            return false;
        }
        if (token.type == TOKEN_TAG) {
            add_error(document, 1, token, "unexpected tag pair inside PGN record");
            delete game;
            return false;
        }

        if (!rav_has_move.empty()) {
            if (token.type == TOKEN_RAV_START) {
                if (!rav_has_move.back()) {
                    add_error(document, 1, token, "nested variation has no preceding move");
                    delete game;
                    return false;
                }
                rav_has_move.push_back(false);
            } else if (token.type == TOKEN_RAV_END) {
                rav_has_move.pop_back();
            } else if (token.type == TOKEN_RESULT) {
                add_error(document, 1, token, "result inside unclosed variation");
                delete game;
                return false;
            } else if (token.type == TOKEN_SAN) {
                if (timing) ++timing->skipped_rav_san_tokens;
                rav_has_move.back() = true;
                saw_content = true;
            } else {
                saw_content = true;
            }
            token = next_opening_token(lexer, timing);
            continue;
        }

        if (result_seen) {
            if (token.type == TOKEN_COMMENT || token.type == TOKEN_ESCAPE_LINE) {
                token = next_opening_token(lexer, timing);
                continue;
            }
            if (token.type == TOKEN_RAV_START && mainline_has_move) {
                rav_has_move.push_back(false);
                token = next_opening_token(lexer, timing);
                continue;
            }
            add_error(document, 1, token, "unexpected token after game termination marker");
            delete game;
            return false;
        }

        if (token.type == TOKEN_RAV_START) {
            if (!mainline_has_move) {
                add_error(document, 1, token, "variation has no preceding move");
                delete game;
                return false;
            }
            rav_has_move.push_back(false);
            saw_content = true;
        } else if (token.type == TOKEN_RAV_END) {
            add_error(document, 1, token, "unexpected variation end");
            delete game;
            return false;
        } else if (token.type == TOKEN_RESULT) {
            game->result = token.text;
            result_seen = true;
            saw_content = true;
        } else if (token.type == TOKEN_SAN) {
            saw_content = true;
            mainline_has_move = true;
            if (analyzed_plies >= max_mainline_plies) {
                if (timing) ++timing->skipped_late_mainline_san_tokens;
            } else {
                const std::string& san = token.text;
                Move move;
                Undo undo;
                bool valid = false;
                if (san == "--") {
                    valid = !position.in_check(position.side_to_move()) &&
                        position.make_null_move(undo);
                } else {
                    const double san_start = timing ? parser_clock_ms() : 0.0;
                    valid = position.parse_san_and_make(san, move, undo);
                    if (timing) timing->san_ms += parser_clock_ms() - san_start;
                }
                if (!valid) {
                    std::ostringstream detail;
                    detail << "illegal or invalid SAN move \"" << san
                           << "\" (expected "
                           << (position.side_to_move() == WHITE ? "White" : "Black")
                           << " to move, FEN " << position.to_fen() << ")";
                    add_error(document, 1, token, detail.str());
                    delete game;
                    return false;
                }
                MoveNode* node = new MoveNode();
                node->parent = parent;
                node->move = move;
                node->san = san;
                node->has_move = true;
                node->position_after = position;
                parent->children.push_back(node);
                parent = node;
                ++analyzed_plies;
                if (timing) ++timing->validated_mainline_plies;
            }
        } else if (token.type != TOKEN_MOVE_NUMBER &&
                   token.type != TOKEN_BARE_MOVE_NUMBER &&
                   token.type != TOKEN_COMMENT && token.type != TOKEN_NAG &&
                   token.type != TOKEN_ESCAPE_LINE) {
            add_error(document, 1, token, "unexpected token in PGN movetext");
            delete game;
            return false;
        } else {
            saw_content = true;
        }
        token = next_opening_token(lexer, timing);
    }

    if (!rav_has_move.empty()) {
        add_error(document, 1, location, "unexpected EOF inside variation");
        delete game;
        return false;
    }
    if (!saw_content) {
        add_error(document, 1, location, "empty PGN game");
        delete game;
        return false;
    }
    document.games.push_back(game);
    return true;
}

bool parse_game(const std::vector<Token>& tokens, std::size_t& index,
                int game_number, PgnDocument& document, PgnGame*& game,
                PgnParseTiming* timing)
{
    game = new PgnGame();
    while (index < tokens.size() && tokens[index].type == TOKEN_TAG) {
        PgnTag tag;
        if (!parse_tag(tokens[index], tag)) { add_error(document, game_number, tokens[index], "malformed tag pair"); return false; }
        game->tags.push_back(tag); ++index;
    }
    Token location = index < tokens.size() ? tokens[index] : Token(TOKEN_END, "", 1, 1);
    if (!configure_start(*game, document, game_number, location)) return false;
    LineState line(game->root, game->initial_position);
    std::vector<LineState> stack;
    std::vector<VariationFrame> variation_frames;
    bool saw_content = !game->tags.empty();
    bool split_move_number_period = false;
    bool result_seen = false;
    int result_line = 0;
    while (index < tokens.size()) {
        const Token& token = tokens[index];
        if (token.type == TOKEN_END || (token.type == TOKEN_TAG && stack.empty())) break;
        if (result_seen && stack.empty()) {
            if (token.type == TOKEN_MOVE_NUMBER && token.text == "1." &&
                token.line >= result_line + 2) break;
            if (token.type == TOKEN_COMMENT) {
                game->trailing_items.push_back(PgnTrailingItem(
                    PgnTrailingItem::COMMENT, token.text, 0));
                saw_content = true;
                ++index;
                continue;
            }
            if (token.type == TOKEN_ESCAPE_LINE) { ++index; continue; }
            if (token.type == TOKEN_RAV_START) {
                MoveNode* variation_root = new MoveNode();
                variation_root->position_after = line.position;
                game->trailing_items.push_back(PgnTrailingItem(
                    PgnTrailingItem::HYPOTHETICAL_VARIATION, "",
                    variation_root));
                stack.push_back(line);
                variation_frames.push_back(VariationFrame(token, variation_root));
                line = LineState(variation_root, line.position, true, true);
                saw_content = true;
                ++index;
                continue;
            }
            add_error(document, game_number, token,
                      "unexpected token after game termination marker");
            return false;
        }
        if (token.type == TOKEN_ERROR) { add_error(document, game_number, token, token.text); return false; }
        if (token.type == TOKEN_MOVE_NUMBER) { saw_content = true; ++index; continue; }
        if (token.type == TOKEN_BARE_MOVE_NUMBER) {
            char* end = 0;
            unsigned long number = std::strtoul(token.text.c_str(), &end, 10);
            bool next_move_is_legal = index + 1 < tokens.size() &&
                tokens[index + 1].type == TOKEN_SAN;
            std::string next_san;
            bool separated_period = false;
            if (next_move_is_legal) {
                next_san = tokens[index + 1].text;
                if (next_san.size() > 1 && next_san[0] == '.') {
                    std::size_t period_count = 0;
                    while (period_count < next_san.size() &&
                           next_san[period_count] == '.') ++period_count;
                    if (period_count == 1 &&
                        line.position.side_to_move() == WHITE) {
                        next_san.erase(0, period_count);
                        separated_period = true;
                    } else {
                        next_move_is_legal = false;
                    }
                }
            }
            Move next_move;
            if (next_move_is_legal) {
                const double san_start = timing ? parser_clock_ms() : 0.0;
                next_move_is_legal = line.position.parse_san(
                    separated_period ? next_san : tokens[index + 1].text,
                    next_move);
                if (timing) timing->san_ms += parser_clock_ms() - san_start;
            }
            if (end != token.text.c_str() && *end == '\0' &&
                number == static_cast<unsigned long>(line.position.fullmove_number()) &&
                next_move_is_legal) {
                saw_content = true;
                split_move_number_period = separated_period;
                ++index;
                continue;
            }
            std::ostringstream detail;
            detail << "unexpected standalone number \"" << token.text
                   << "\"; expected a move number followed by a legal SAN move";
            add_error(document, game_number, token, detail.str());
            return false;
        }
        if (token.type == TOKEN_COMMENT) {
            if (line.variation_start) line.leading_comments.push_back(token.text);
            else {
                line.current->comments.push_back(token.text);
                if (line.current->has_move)
                    line.current->following_items.push_back(PgnMoveItem(
                        PgnMoveItem::COMMENT, token.text, 0));
            }
            saw_content = true; ++index; continue;
        }
        if (token.type == TOKEN_NAG) {
            int symbolic_nag = 0;
            if (token.text == "!") symbolic_nag = 1;
            if (token.text == "?") symbolic_nag = 2;
            if (token.text == "!!") symbolic_nag = 3;
            if (token.text == "??") symbolic_nag = 4;
            if (token.text == "!?") symbolic_nag = 5;
            if (token.text == "?!") symbolic_nag = 6;
            if (symbolic_nag) { line.current->nags.push_back(symbolic_nag); saw_content = true; ++index; continue; }
            char* end = 0; long value = std::strtol(token.text.c_str(), &end, 10);
            if (*end != '\0' || value < 0 || value > INT_MAX) { add_error(document, game_number, token, "invalid NAG"); return false; }
            line.current->nags.push_back(static_cast<int>(value)); saw_content = true; ++index; continue;
        }
        if (token.type == TOKEN_RAV_START) {
            if (!line.current->has_move) { add_error(document, game_number, token, "variation has no preceding move"); return false; }
            MoveNode* parent = line.current->parent;
            Position start = parent->has_move ? parent->position_after : game->initial_position;
            stack.push_back(line);
            variation_frames.push_back(VariationFrame(token, 0, line.current));
            line = LineState(parent, start, true, line.post_result_analysis); saw_content = true; ++index; continue;
        }
        if (token.type == TOKEN_RAV_END) {
            if (stack.empty()) { add_error(document, game_number, token, "unexpected variation end"); return false; }
            if (variation_frames.back().post_result_root &&
                variation_frames.back().post_result_root->children.empty()) {
                add_error(document, game_number, variation_frames.back().opening,
                          "empty post-result hypothetical variation");
                return false;
            }
            line = stack.back(); stack.pop_back(); variation_frames.pop_back(); ++index; continue;
        }
        if (token.type == TOKEN_ESCAPE_LINE) { ++index; continue; }
        if (token.type == TOKEN_RESULT) {
            if (!stack.empty()) { add_error(document, game_number, token, "result inside unclosed variation"); return false; }
            game->result = token.text; saw_content = true; result_seen = true;
            result_line = token.line; ++index; continue;
        }
        if (token.type == TOKEN_SAN) {
            std::string san_text = token.text;
            if (split_move_number_period) {
                san_text.erase(0, 1);
                split_move_number_period = false;
            }
            const bool null_move = san_text == "--";
            Move move;
            Undo undo;
            bool valid_move;
            if (null_move) valid_move = !line.position.in_check(line.position.side_to_move());
            else {
                const double san_start = timing ? parser_clock_ms() : 0.0;
                valid_move = line.position.parse_san_and_make(san_text, move, undo);
                if (timing) timing->san_ms += parser_clock_ms() - san_start;
            }
            if (!valid_move) {
                std::ostringstream detail;
                detail << "illegal or invalid SAN move \"" << san_text << "\""
                       << " (RAV depth " << stack.size() << ", expected "
                       << (line.position.side_to_move() == WHITE ? "White" : "Black")
                       << " to move, FEN " << line.position.to_fen() << ")";
                if (line.post_result_analysis) {
                    add_error(document, game_number, token,
                              "invalid post-result hypothetical variation: " +
                              detail.str());
                    return false;
                }
                if (variation_frames.empty()) {
                    add_error(document, game_number, token, detail.str());
                    return false;
                }
                std::size_t after_variation;
                if (!find_variation_end(tokens, index + 1, after_variation)) {
                    add_error(document, game_number, variation_frames.back().opening,
                              "cannot safely skip invalid variation; its structure is damaged");
                    return false;
                }
                std::ostringstream warning;
                warning << "truncating variation at illegal move \"" << san_text
                        << "\" (RAV depth " << stack.size() << ")";
                add_skipped_variation_warning(document, game_number, token,
                                              warning.str());
                line = stack.back();
                stack.pop_back();
                variation_frames.pop_back();
                index = after_variation;
                split_move_number_period = false;
                continue;
            }
            MoveNode* node = new MoveNode();
            if (null_move && !line.position.make_null_move(undo)) {
                delete node;
                add_error(document, game_number, token,
                          "could not apply PGN move \"" + san_text + "\"");
                return false;
            }
            node->parent = line.current; node->move = move; node->san = san_text; node->has_move = true; node->position_after = line.position;
            if (line.variation_start) {
                node->leading_comments = line.leading_comments;
                line.leading_comments.clear();
                if (!variation_frames.empty() &&
                    !variation_frames.back().post_result_root &&
                    variation_frames.back().source_anchor)
                    variation_frames.back().source_anchor->following_items.push_back(
                        PgnMoveItem(PgnMoveItem::VARIATION, "", node));
                line.variation_start = false;
            }
            line.current->children.push_back(node); line.current = node; saw_content = true; ++index; continue;
        }
        add_error(document, game_number, token, "unexpected token"); return false;
    }
    if (!stack.empty()) { add_error(document, game_number, tokens[index - 1], "unexpected EOF inside variation"); return false; }
    if (!saw_content) { add_error(document, game_number, location, "empty PGN game"); return false; }
    return true;
}

}

bool PgnParser::parse(const std::string& text, PgnDocument& document,
                      bool allow_initial_bom, PgnParseTiming* timing)
{
    if (timing) *timing = PgnParseTiming();
    Lexer lexer(text, allow_initial_bom);
    std::vector<Token> tokens;
    const double tokenize_start = timing ? parser_clock_ms() : 0.0;
    Token token = lexer.next();
    while (token.type != TOKEN_END) { tokens.push_back(token); token = lexer.next(); }
    tokens.push_back(token);
    if (timing) timing->tokenize_ms = parser_clock_ms() - tokenize_start;
    document.clear();
    std::size_t index = 0;
    int game_number = 1;
    const double semantic_start = timing ? parser_clock_ms() : 0.0;
    while (index < tokens.size() && tokens[index].type != TOKEN_END) {
        PgnGame* game = 0;
        std::size_t start = index;
        if (parse_game(tokens, index, game_number, document, game, timing)) document.games.push_back(game);
        else {
            delete game;
            while (index < tokens.size() && tokens[index].type != TOKEN_END && tokens[index].type != TOKEN_TAG) ++index;
            if (index == start) ++index;
        }
        ++game_number;
    }
    if (timing) timing->semantic_ms = parser_clock_ms() - semantic_start;
    return document.errors.empty();
}

bool PgnParser::parse_opening(const std::string& text, PgnDocument& document,
                              int max_mainline_plies, bool allow_initial_bom,
                              PgnParseTiming* timing)
{
    if (timing) *timing = PgnParseTiming();
    document.clear();
    /* The opening parser owns a Lexer and consumes tokens incrementally. */
    const double semantic_start = timing ? parser_clock_ms() : 0.0;
    const bool okay = parse_opening_game(text, document, max_mainline_plies,
                                         allow_initial_bom, timing);
    if (timing) {
        const double elapsed = parser_clock_ms() - semantic_start;
        timing->semantic_ms = elapsed > timing->tokenize_ms ?
            elapsed - timing->tokenize_ms : 0.0;
    }
    return okay && document.errors.empty();
}

}

#include "pgn_eco/pgn.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <vector>
#include <deque>
#include <pthread.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <signal.h>

using namespace gambitree;

namespace {
const std::size_t MAX_GAME_BYTES = 64U * 1024U * 1024U;
const std::size_t MAX_BATCH_GAMES = 512U;
const std::size_t MAX_BATCH_BYTES = 128U * 1024U * 1024U;
/* Lichess' production opening lookup examines positions through ply 40. */
const int MAX_CLASSIFY_PLIES = 40;
const char* const LICHESS_REVISION = "c67912be581f0793dbaa776be5ccf111e01f88d9";
const char* const SCID_REVISION = "fc26337165346192b5399780357561401930dff4";
const char* const SCID_SHA256 = "acd73837668a0791aa4d1b174fdfe8b19efa361ac2437b45735b397b0e20c4a7";
const unsigned long SCID_BYTES = 1089733UL;

double monotonic_ms();

struct ClassificationTiming {
    double fen_ms;
    double key_ms;
    double lookup_ms;
    double history_ms;
    ClassificationTiming() : fen_ms(0.0), key_ms(0.0), lookup_ms(0.0),
        history_ms(0.0) {}
};

struct TagSpan {
    std::string name;
    std::string value;
    std::size_t value_begin;
    std::size_t value_end;
    std::size_t line_begin;
    std::size_t line_end;
    std::size_t line_after;
};

struct GameTags {
    std::vector<TagSpan> tags;
    std::size_t insertion_point;
    bool valid;
    GameTags() : insertion_point(0), valid(true) {}
};

struct HistoryDistance {
    bool same_side_multisets;
    int unmatched_plies;
    int removed_game_plies;
    int removed_reference_plies;
    int swaps;
    int removed_cycles;
    int reference_plies;
    int white_lcs;
    int black_lcs;
    int white_swaps;
    int black_swaps;
    int common_prefix;
    HistoryDistance() : same_side_multisets(false), unmatched_plies(0), removed_game_plies(0),
        removed_reference_plies(0), swaps(0), removed_cycles(0), reference_plies(0),
        white_lcs(0), black_lcs(0), white_swaps(0), black_swaps(0), common_prefix(0) {}
};

struct Classification {
    enum State { FOUND, NONE, AMBIGUOUS, INVALID } state;
    std::string eco;
    int ply;
    int examined_plies;
    bool fallback;
    std::string detail;
    std::string position_key;
    std::string fen;
    std::string examined_fen;
    std::set<std::string> alternatives;
    std::set<std::string> exact_alternatives;
    std::set<std::string> matching_codes;
    std::map<std::string, HistoryDistance> history_scores;
    bool exact_history_match;
    bool book_conflict;
    std::string history_evidence;
    std::string mode4_priority;
    std::string audit_json;
    Classification() : state(NONE), ply(-1), examined_plies(0), fallback(false),
        exact_history_match(false), book_conflict(false) {}
};

struct OpeningReference {
    std::string eco;
    std::vector<std::string> positions;
    std::vector<Move> moves;
    int plies;
    bool native_pgn;
    OpeningReference() : plies(0), native_pgn(false) {}
};
typedef std::map<std::string, std::vector<OpeningReference> > OpeningBook;
struct BookStats {
    unsigned long rows;
    unsigned long positions;
    unsigned long multi_reference_positions;
    unsigned long conflicts;
    unsigned long invalid_rows;
    unsigned long source_records;
    unsigned long ignored_scid_depth;
    unsigned long corrected_check_suffixes;
    unsigned long corrected_missing_capture_markers;
    int max_plies;
    int max_match_plies;
    std::map<std::string, unsigned long> references;
    std::vector<std::string> rejected_details;
    std::vector<std::string> conflict_details;
    BookStats() : rows(0), positions(0), multi_reference_positions(0), conflicts(0),
        invalid_rows(0), source_records(0), ignored_scid_depth(0),
        corrected_check_suffixes(0), corrected_missing_capture_markers(0),
        max_plies(0), max_match_plies(0) {}
    std::map<std::string, unsigned long> code_counts;
};

struct RouteDiagnostic {
    std::set<std::string> codes;
    std::string start;
    unsigned long plies;
    RouteDiagnostic() : plies(0) {}
};
typedef std::map<std::string, RouteDiagnostic> RouteDiagnostics;

enum ReportMode { REPORT_BRIEF, REPORT_DETAILS, REPORT_PER_GAME, REPORT_DEBUG };

struct CodeStats {
    unsigned long total, equal, prefix_compatible, different, missing, invalid,
        ambiguous, unclassifiable, invalid_opening;
    CodeStats() : total(0), equal(0), prefix_compatible(0), different(0),
        missing(0), invalid(0), ambiguous(0), unclassifiable(0), invalid_opening(0) {}
};

struct ShiftOrder {
    bool operator()(const std::pair<std::string, unsigned long>& a,
                    const std::pair<std::string, unsigned long>& b) const
    {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    }
};

bool parse_limit(const std::string& value, unsigned long& result)
{
    if (value.empty()) return false;
    unsigned long parsed = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
        const unsigned long digit = static_cast<unsigned long>(value[i] - '0');
        if (parsed > (ULONG_MAX - digit) / 10UL) return false;
        parsed = parsed * 10UL + digit;
    }
    if (parsed == 0) return false;
    result = parsed;
    return true;
}

bool parse_threads(const std::string& value, unsigned long& result)
{
    if (value.empty()) return false;
    unsigned long parsed = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
        const unsigned long digit = static_cast<unsigned long>(value[i] - '0');
        if (parsed > (ULONG_MAX - digit) / 10UL) return false;
        parsed = parsed * 10UL + digit;
    }
    result = parsed;
    return true;
}

bool quota_limit(const std::string& path, bool cgroup_v2, unsigned long& cpus)
{
    std::ifstream input(path.c_str());
    if (!input) return false;
    if (cgroup_v2) {
        std::string quota, period;
        if (!(input >> quota >> period) || quota == "max") return false;
        char* qend = 0;
        char* pend = 0;
        const unsigned long q = std::strtoul(quota.c_str(), &qend, 10);
        const unsigned long p = std::strtoul(period.c_str(), &pend, 10);
        if (qend == quota.c_str() || *qend != '\0' || pend == period.c_str() ||
            *pend != '\0' || p == 0) return false;
        cpus = q / p;
        if (cpus == 0) cpus = 1;
        return true;
    }
    long quota = -1, period = 0;
    if (!(input >> quota) || quota <= 0) return false;
    const std::string period_path = path.substr(0, path.find_last_of('/') + 1) +
        "cpu.cfs_period_us";
    std::ifstream period_input(period_path.c_str());
    if (!(period_input >> period) || period <= 0) return false;
    cpus = static_cast<unsigned long>(quota / period);
    if (cpus == 0) cpus = 1;
    return true;
}

void apply_quota(const std::string& file, bool cgroup_v2, unsigned long& available)
{
    unsigned long limit = 0;
    if (quota_limit(file, cgroup_v2, limit) && limit < available) available = limit;
}

void apply_cgroup_hierarchy(const std::string& root, const std::string& relative,
                            bool cgroup_v2, unsigned long& available)
{
    std::string current = root;
    const std::string file = cgroup_v2 ? "cpu.max" : "cpu.cfs_quota_us";
    if (relative.empty() || relative == "/") {
        apply_quota(current + "/" + file, cgroup_v2, available);
        return;
    }
    std::size_t at = 0;
    while (at < relative.size() && relative[at] == '/') ++at;
    apply_quota(current + "/" + file, cgroup_v2, available);
    while (at < relative.size()) {
        const std::size_t end = relative.find('/', at);
        const std::string component = relative.substr(at,
            end == std::string::npos ? std::string::npos : end - at);
        if (component.empty() || component == "." || component == "..") return;
        current += "/" + component;
        apply_quota(current + "/" + file, cgroup_v2, available);
        if (end == std::string::npos) break;
        at = end + 1;
    }
}

unsigned long online_cpu_count()
{
    long count = ::sysconf(_SC_NPROCESSORS_ONLN);
    unsigned long available = count > 0 ? static_cast<unsigned long>(count) : 1UL;
#ifdef __linux__
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    if (::sched_getaffinity(0, sizeof(affinity), &affinity) == 0) {
        const int affinity_count = CPU_COUNT(&affinity);
        if (affinity_count > 0 && static_cast<unsigned long>(affinity_count) < available)
            available = static_cast<unsigned long>(affinity_count);
    }
    /* Apply every quota from the cgroup root down to this process' group. */
    std::ifstream memberships("/proc/self/cgroup");
    std::string line;
    std::string v2_path;
    std::vector<std::string> v1_cpu_paths;
    while (std::getline(memberships, line)) {
        if (line.compare(0, 3, "0::") == 0) {
            v2_path = line.substr(3);
            continue;
        }
        const std::string::size_type first = line.find(':');
        const std::string::size_type second = first == std::string::npos ? first : line.find(':', first + 1);
        if (first == std::string::npos || second == std::string::npos) continue;
        const std::string controllers = line.substr(first + 1, second - first - 1);
        bool has_cpu = false;
        std::size_t begin = 0;
        while (begin <= controllers.size()) {
            const std::size_t end = controllers.find(',', begin);
            if (controllers.substr(begin, end == std::string::npos ? std::string::npos : end - begin) == "cpu")
                has_cpu = true;
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        if (has_cpu) v1_cpu_paths.push_back(line.substr(second + 1));
    }
    if (!v2_path.empty()) {
        apply_cgroup_hierarchy("/sys/fs/cgroup", v2_path, true, available);
    } else {
        apply_cgroup_hierarchy("/sys/fs/cgroup/cpu", "/", false, available);
        apply_cgroup_hierarchy("/sys/fs/cgroup/cpu,cpuacct", "/", false, available);
        for (std::size_t i = 0; i < v1_cpu_paths.size(); ++i) {
            apply_cgroup_hierarchy("/sys/fs/cgroup/cpu", v1_cpu_paths[i], false, available);
            apply_cgroup_hierarchy("/sys/fs/cgroup/cpu,cpuacct", v1_cpu_paths[i], false, available);
        }
    }
#endif
    return available ? available : 1UL;
}

std::string percent(unsigned long part, unsigned long total)
{
    if (!total) return "0.0%";
    std::ostringstream out;
    out.setf(std::ios::fixed, std::ios::floatfield);
    out.precision(1);
    out << (100.0 * static_cast<double>(part) / static_cast<double>(total)) << '%';
    return out.str();
}

std::string code_list(const std::set<std::string>& codes)
{
    std::ostringstream out;
    bool first = true;
    for (std::set<std::string>::const_iterator i = codes.begin(); i != codes.end(); ++i) {
        if (!first) out << ',';
        out << *i;
        first = false;
    }
    return first ? "-" : out.str();
}

bool same_move(const Move& left, const Move& right)
{
    return left.from == right.from && left.to == right.to &&
        left.promotion == right.promotion &&
        (left.flags & (MOVE_CASTLE_KINGSIDE | MOVE_CASTLE_QUEENSIDE)) ==
        (right.flags & (MOVE_CASTLE_KINGSIDE | MOVE_CASTLE_QUEENSIDE));
}

std::string canonical_reference_route(const OpeningReference& reference)
{
    if (reference.positions.empty() || reference.positions.size() != reference.moves.size() + 1)
        return std::string();
    std::string route = reference.positions[0];
    route += '\0';
    for (std::size_t i = 0; i < reference.moves.size(); ++i) {
        route += static_cast<char>(reference.moves[i].from);
        route += static_cast<char>(reference.moves[i].to);
        route += static_cast<char>(reference.moves[i].promotion);
        route += static_cast<char>(reference.moves[i].flags &
            (MOVE_CASTLE_KINGSIDE | MOVE_CASTLE_QUEENSIDE));
    }
    return route;
}

bool same_move_history(const std::vector<Move>& reference,
                       const std::vector<Move>& played)
{
    if (reference.size() != played.size()) return false;
    for (std::size_t i = 0; i < reference.size(); ++i)
        if (!same_move(reference[i], played[i])) return false;
    return true;
}

Color mover_for_position(const std::string& key)
{
    const std::size_t side = key.find(' ');
    return side != std::string::npos && side + 1 < key.size() && key[side + 1] == 'b' ? BLACK : WHITE;
}

struct ReducedHistory {
    std::vector<std::string> positions;
    std::vector<Move> moves;
    std::vector<std::string> erased_cycles;
    int removed_plies;
    int removed_cycles;
    ReducedHistory() : removed_plies(0), removed_cycles(0) {}
};

typedef std::map<std::string, ReducedHistory> ReferenceHistoryCache;

/* Erase only complete loops that return to the same chess state. The key
   includes pieces, mover, castling and legal en-passant rights, so the
   remaining suffix is still legal from the exact same state. */
ReducedHistory erase_state_cycles(const std::vector<std::string>& positions,
                                  const std::vector<Move>& moves,
                                  bool record_cycles = false)
{
    ReducedHistory reduced;
    if (positions.empty() || positions.size() != moves.size() + 1) return reduced;
    reduced.positions.push_back(positions[0]);
    std::map<std::string, std::size_t> seen;
    seen[positions[0]] = 0;
    for (std::size_t i = 0; i < moves.size(); ++i) {
        const std::string& next = positions[i + 1];
        std::map<std::string, std::size_t>::const_iterator repeated = seen.find(next);
        if (repeated != seen.end()) {
            const std::size_t keep = repeated->second;
            if (record_cycles) {
                std::ostringstream cycle;
                cycle << "start=" << keep << ";end=" << (reduced.moves.size() + 1)
                      << ";moves=";
                for (std::size_t j = keep; j < reduced.moves.size(); ++j) {
                    if (j != keep) cycle << ',';
                    cycle << Position::square_to_string(reduced.moves[j].from)
                          << Position::square_to_string(reduced.moves[j].to);
                    if (reduced.moves[j].promotion != NO_PIECE)
                        cycle << static_cast<int>(reduced.moves[j].promotion);
                }
                if (reduced.moves.size() > keep) cycle << ',';
                cycle << Position::square_to_string(moves[i].from)
                      << Position::square_to_string(moves[i].to);
                if (moves[i].promotion != NO_PIECE)
                    cycle << static_cast<int>(moves[i].promotion);
                cycle << ";state=" << next;
                reduced.erased_cycles.push_back(cycle.str());
            }
            reduced.removed_plies += static_cast<int>(reduced.moves.size() + 1 - keep);
            ++reduced.removed_cycles;
            reduced.moves.resize(keep);
            reduced.positions.resize(keep + 1);
            seen.clear();
            for (std::size_t j = 0; j < reduced.positions.size(); ++j)
                seen[reduced.positions[j]] = j;
        } else {
            reduced.moves.push_back(moves[i]);
            reduced.positions.push_back(next);
            seen[next] = reduced.positions.size() - 1;
        }
    }
    return reduced;
}

/* Build this cache before workers start. The key deduplicates the same full
   route, which is indexed at many positions in the opening book. The cache is
   then read-only during classification. */
void prepare_reference_histories(const OpeningBook& book,
                                 ReferenceHistoryCache& cache)
{
    for (OpeningBook::const_iterator entry = book.begin(); entry != book.end(); ++entry) {
        for (std::vector<OpeningReference>::const_iterator reference = entry->second.begin();
             reference != entry->second.end(); ++reference) {
            const std::string route = canonical_reference_route(*reference);
            if (route.empty() || cache.find(route) != cache.end()) continue;
            cache.insert(std::make_pair(route,
                erase_state_cycles(reference->positions, reference->moves)));
        }
    }
}

const ReducedHistory& reduced_reference_history(
    const OpeningReference& reference, const ReferenceHistoryCache& cache)
{
    const std::string route = canonical_reference_route(reference);
    ReferenceHistoryCache::const_iterator found = cache.find(route);
    if (found == cache.end())
        throw std::logic_error("opening reference history was not prepared");
    return found->second;
}

/* A longest common subsequence is the maximum legal-history order-preserving
   correspondence for one mover. Its symmetric insertion/deletion distance
   identifies unmatched extra or omitted move occurrences without confusing
   them with same-set transpositions (which are handled by inversions). */
int side_history_lcs(const ReducedHistory& played,
                     const ReducedHistory& reference, Color side)
{
    std::vector<Move> left, right;
    for (std::size_t i = 0; i < played.moves.size(); ++i)
        if (mover_for_position(played.positions[i]) == side) left.push_back(played.moves[i]);
    for (std::size_t i = 0; i < reference.moves.size(); ++i)
        if (mover_for_position(reference.positions[i]) == side) right.push_back(reference.moves[i]);
    std::vector<int> previous(right.size() + 1, 0), current(right.size() + 1, 0);
    for (std::size_t i = 0; i < left.size(); ++i) {
        current[0] = 0;
        for (std::size_t j = 0; j < right.size(); ++j) {
            if (same_move(left[i], right[j])) current[j + 1] = previous[j] + 1;
            else current[j + 1] = std::max(previous[j + 1], current[j]);
        }
        previous.swap(current);
    }
    return previous[right.size()];
}

/* Match canonical moves within each side's sequence. If the move multisets
   agree, the inversions of the stable occurrence mapping are exactly the
   minimum adjacent same-side swaps (including repeated moves). Otherwise,
   unmatched_plies is a per-side LCS insertion/deletion distance and swaps
   counts reorderings among the move occurrences that still match. */
HistoryDistance compare_histories(const ReducedHistory& played,
                                 const ReducedHistory& reference,
                                 bool include_metrics = false)
{
    HistoryDistance result;
    result.removed_game_plies = played.removed_plies;
    result.removed_reference_plies = reference.removed_plies;
    result.removed_cycles = played.removed_cycles + reference.removed_cycles;
    std::vector<int> mapping(played.moves.size(), -1);
    std::vector<bool> used(reference.moves.size(), false);
    int common = 0;
    for (std::size_t i = 0; i < played.moves.size(); ++i) {
        const Color side = mover_for_position(played.positions[i]);
        for (std::size_t j = 0; j < reference.moves.size(); ++j) {
            if (used[j] || side != mover_for_position(reference.positions[j]) ||
                !same_move(played.moves[i], reference.moves[j])) continue;
            used[j] = true;
            mapping[i] = static_cast<int>(j);
            ++common;
            break;
        }
    }
    result.unmatched_plies = static_cast<int>(played.moves.size() + reference.moves.size() -
                                               2 * static_cast<std::size_t>(common));
    result.same_side_multisets = result.unmatched_plies == 0;
    if (include_metrics) {
        while (static_cast<std::size_t>(result.common_prefix) < played.moves.size() &&
               static_cast<std::size_t>(result.common_prefix) < reference.moves.size() &&
               same_move(played.moves[static_cast<std::size_t>(result.common_prefix)],
                         reference.moves[static_cast<std::size_t>(result.common_prefix)]))
            ++result.common_prefix;
        result.white_lcs = side_history_lcs(played, reference, WHITE);
        result.black_lcs = side_history_lcs(played, reference, BLACK);
    }
    if (!result.same_side_multisets) {
        const int common_in_order = include_metrics ? result.white_lcs + result.black_lcs :
            side_history_lcs(played, reference, WHITE) + side_history_lcs(played, reference, BLACK);
        result.unmatched_plies = static_cast<int>(played.moves.size() + reference.moves.size() -
                                                   2 * static_cast<std::size_t>(common_in_order));
    }
    for (std::size_t i = 0; i < mapping.size(); ++i) {
        if (mapping[i] < 0) continue;
        const Color side_i = mover_for_position(played.positions[i]);
        for (std::size_t j = i + 1; j < mapping.size(); ++j) {
            if (mapping[j] < 0 || side_i != mover_for_position(played.positions[j])) continue;
            if (mapping[i] > mapping[j]) {
                ++result.swaps;
                if (include_metrics) {
                    if (side_i == WHITE) ++result.white_swaps;
                    else ++result.black_swaps;
                }
            }
        }
    }
    return result;
}

int compare_distance(const HistoryDistance& left, const HistoryDistance& right)
{
    if (left.same_side_multisets != right.same_side_multisets)
        return left.same_side_multisets ? -1 : 1;
    if (left.same_side_multisets) {
        const int left_removed = left.removed_game_plies + left.removed_reference_plies;
        const int right_removed = right.removed_game_plies + right.removed_reference_plies;
        if (left_removed != right_removed) return left_removed < right_removed ? -1 : 1;
        if (left.swaps != right.swaps) return left.swaps < right.swaps ? -1 : 1;
        return 0;
    }
    if (left.unmatched_plies != right.unmatched_plies)
        return left.unmatched_plies < right.unmatched_plies ? -1 : 1;
    const int left_removed = left.removed_game_plies + left.removed_reference_plies;
    const int right_removed = right.removed_game_plies + right.removed_reference_plies;
    if (left_removed != right_removed) return left_removed < right_removed ? -1 : 1;
    if (left.swaps != right.swaps) return left.swaps < right.swaps ? -1 : 1;
    return 0;
}

std::string distance_text(const HistoryDistance& distance)
{
    std::ostringstream out;
    out << (distance.same_side_multisets ? "same-side-multisets" : "different-move-set")
        << ",erased-game=" << distance.removed_game_plies
        << ",erased-reference=" << distance.removed_reference_plies
        << ",unmatched=" << distance.unmatched_plies
        << ",swaps=" << distance.swaps
        << ",cycles=" << distance.removed_cycles
        << ",reference-depth=" << distance.reference_plies;
    return out.str();
}

std::string json_string(const std::string& value)
{
    std::ostringstream out;
    out << '"';
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c == '\n') out << "\\n";
        else if (c == '\r') out << "\\r";
        else if (c == '\t') out << "\\t";
        else if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            out << "\\u00" << hex[c >> 4] << hex[c & 15];
        } else out << static_cast<char>(c);
    }
    out << '"';
    return out.str();
}

std::string diagnostic_tag_values(const std::string& raw, const GameTags& tags)
{
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (std::size_t i = 0; i < tags.tags.size(); ++i) {
        const TagSpan& tag = tags.tags[i];
        if (tag.name != "ECO") continue;
        if (!first) out << ',';
        first = false;
        const std::string value = raw.substr(tag.value_begin, tag.value_end - tag.value_begin);
        const std::string encoded = json_string(value);
        for (std::size_t j = 0; j < encoded.size(); ++j) {
            if (encoded[j] == '|') out << "\\u007c";
            else out << encoded[j];
        }
    }
    out << ']';
    return out.str();
}

std::string move_token(const Move& move)
{
    std::ostringstream out;
    out << Position::square_to_string(move.from) << Position::square_to_string(move.to);
    if (move.promotion != NO_PIECE) out << static_cast<int>(move.promotion);
    return out.str();
}

void json_string_array(std::ostringstream& out, const std::vector<std::string>& values)
{
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << json_string(values[i]);
    }
    out << ']';
}

std::vector<std::string> move_tokens(const std::vector<Move>& moves)
{
    std::vector<std::string> result;
    for (std::size_t i = 0; i < moves.size(); ++i) result.push_back(move_token(moves[i]));
    return result;
}

std::vector<std::string> mover_tokens(const ReducedHistory& history, Color side)
{
    std::vector<std::string> result;
    for (std::size_t i = 0; i < history.moves.size(); ++i)
        if (mover_for_position(history.positions[i]) == side)
            result.push_back(move_token(history.moves[i]));
    return result;
}

void reference_replay(const OpeningReference& reference,
                      std::vector<std::string>& sans,
                      std::vector<std::string>& fens)
{
    if (reference.positions.empty()) return;
    Position position;
    std::string error;
    if (!Position::from_fen(reference.positions[0] + " 0 1", position, &error)) return;
    fens.push_back(position.to_fen());
    for (std::size_t i = 0; i < reference.moves.size(); ++i) {
        sans.push_back(position.san(reference.moves[i]));
        Undo undo;
        if (!position.make_move(reference.moves[i], undo)) break;
        fens.push_back(position.to_fen());
    }
}

std::string audit_match_json(const std::vector<const OpeningReference*>& eligible,
                             const std::vector<std::pair<std::string, const OpeningReference*> >& ep_siblings,
                             const std::set<std::string>& positional,
                             const std::set<std::string>& exact,
                             const std::map<std::string, HistoryDistance>& scores,
                             const std::set<std::string>& matching,
                             bool exact_match,
                             const std::vector<std::string>& game_history,
                             const std::vector<Move>& game_moves,
                             const std::vector<std::string>& game_sans,
                             const std::vector<std::string>& game_fens,
                             const std::string& key, const std::string& fen,
                             const std::string& decision)
{
    const ReducedHistory reduced_game = erase_state_cycles(game_history, game_moves, true);
    std::ostringstream out;
    out << "{\"match_ply\":" << game_moves.size()
        << ",\"position_key_epd\":" << json_string(key)
        << ",\"match_fen\":" << json_string(fen)
        << ",\"game_fen_ep_field\":";
    {
        std::istringstream fen_fields(fen);
        std::string field;
        for (int i = 0; i < 4; ++i) fen_fields >> field;
        out << json_string(field);
    }
    out << ",\"game_legal_ep_key_field\":";
    {
        std::istringstream key_fields(key);
        std::string field;
        for (int i = 0; i < 4; ++i) key_fields >> field;
        out << json_string(field);
    }
    out
        << ",\"exact_codes\":";
    json_string_array(out, std::vector<std::string>(exact.begin(), exact.end()));
    out << ",\"position_codes\":";
    json_string_array(out, std::vector<std::string>(positional.begin(), positional.end()));
    out << ",\"selected_codes\":";
    json_string_array(out, std::vector<std::string>(matching.begin(), matching.end()));
    out << ",\"any_exact_reference_history\":" << (exact_match ? "true" : "false")
        << ",\"decision\":" << json_string(decision)
        << ",\"played_san\":";
    json_string_array(out, game_sans);
    out << ",\"played_uci\":";
    json_string_array(out, move_tokens(game_moves));
    out << ",\"played_position_keys_epd\":";
    json_string_array(out, game_history);
    out << ",\"played_fens\":";
    json_string_array(out, game_fens);
    out << ",\"reduced_played_uci\":";
    json_string_array(out, move_tokens(reduced_game.moves));
    out << ",\"removed_played_cycles\":";
    json_string_array(out, reduced_game.erased_cycles);
    out << ",\"same_board_castling_different_ep_references\":[";
    for (std::size_t i = 0; i < ep_siblings.size(); ++i) {
        if (i) out << ',';
        const OpeningReference& ref = *ep_siblings[i].second;
        std::vector<std::string> ref_sans, ref_fens;
        reference_replay(ref, ref_sans, ref_fens);
        std::string ep_field;
        std::istringstream fields(ep_siblings[i].first);
        for (int j = 0; j < 4; ++j) fields >> ep_field;
        out << "{\"eco\":" << json_string(ref.eco)
            << ",\"position_key_epd\":" << json_string(ep_siblings[i].first)
            << ",\"legal_ep_key_field\":" << json_string(ep_field)
            << ",\"reference_plies\":" << ref.plies
            << ",\"reference_san\":";
        json_string_array(out, ref_sans);
        out << ",\"reference_uci\":";
        json_string_array(out, move_tokens(ref.moves));
        out << ",\"reference_position_keys_epd\":";
        json_string_array(out, ref.positions);
        out << ",\"reference_fens\":";
        json_string_array(out, ref_fens);
        out << '}';
    }
    out << ']';
    out << ",\"references\":[";
    for (std::size_t i = 0; i < eligible.size(); ++i) {
        if (i) out << ',';
        const OpeningReference& ref = *eligible[i];
        const bool ref_exact = ref.positions.size() == game_history.size() &&
            std::equal(ref.positions.begin(), ref.positions.end(), game_history.begin()) &&
            same_move_history(ref.moves, game_moves);
        const ReducedHistory reduced_ref = erase_state_cycles(ref.positions, ref.moves, true);
        HistoryDistance distance = compare_histories(reduced_game, reduced_ref, true);
        distance.reference_plies = ref.plies;
        std::vector<std::string> ref_sans, ref_fens;
        reference_replay(ref, ref_sans, ref_fens);
        std::map<std::string, HistoryDistance>::const_iterator code_best = scores.find(ref.eco);
        const bool is_code_best = code_best != scores.end() &&
            compare_distance(distance, code_best->second) == 0 &&
            distance.reference_plies == code_best->second.reference_plies;
        out << "{\"eco\":" << json_string(ref.eco)
            << ",\"reference_plies\":" << ref.plies
            << ",\"native_pgn\":" << (ref.native_pgn ? "true" : "false")
            << ",\"exact_history\":" << (ref_exact ? "true" : "false")
            << ",\"best_reference_for_code\":" << (is_code_best ? "true" : "false")
            << ",\"reference_san\":";
        json_string_array(out, ref_sans);
        out << ",\"reference_uci\":";
        json_string_array(out, move_tokens(ref.moves));
        out << ",\"reference_position_keys_epd\":";
        json_string_array(out, ref.positions);
        out << ",\"reference_fens\":";
        json_string_array(out, ref_fens);
        out << ",\"reduced_reference_uci\":";
        json_string_array(out, move_tokens(reduced_ref.moves));
        out << ",\"removed_reference_cycles\":";
        json_string_array(out, reduced_ref.erased_cycles);
        out << ",\"score\":{\"same_side_multisets\":"
            << (distance.same_side_multisets ? "true" : "false")
            << ",\"common_prefix_plies\":" << distance.common_prefix
            << ",\"white_moves\":";
        json_string_array(out, mover_tokens(reduced_ref, WHITE));
        out << ",\"played_white_moves\":";
        json_string_array(out, mover_tokens(reduced_game, WHITE));
        out << ",\"black_moves\":";
        json_string_array(out, mover_tokens(reduced_ref, BLACK));
        out << ",\"played_black_moves\":";
        json_string_array(out, mover_tokens(reduced_game, BLACK));
        out << ",\"white_lcs\":" << distance.white_lcs
            << ",\"black_lcs\":" << distance.black_lcs
            << ",\"white_kendall_inversions\":" << distance.white_swaps
            << ",\"black_kendall_inversions\":" << distance.black_swaps
            << ",\"total_kendall_inversions\":" << distance.swaps
            << ",\"unmatched_plies\":" << distance.unmatched_plies
            << ",\"removed_game_plies\":" << distance.removed_game_plies
            << ",\"removed_reference_plies\":" << distance.removed_reference_plies
            << ",\"removed_game_cycles\":" << reduced_game.removed_cycles
            << ",\"removed_reference_cycles_count\":" << reduced_ref.removed_cycles
            << ",\"reference_depth\":" << ref.plies << "}}";
    }
    out << "]}";
    return out.str();
}

void append_audit_timeline(Classification& result, const OpeningBook& book,
                           const std::vector<std::string>& positions,
                           const std::vector<Move>& moves,
                           const std::vector<std::string>& sans,
                           const std::vector<std::string>& fens)
{
    if (result.audit_json.empty()) return;
    std::vector<std::string> castling;
    for (unsigned int mask = 0; mask < 16; ++mask) {
        std::string rights;
        if (mask & WHITE_KINGSIDE) rights += 'K';
        if (mask & WHITE_QUEENSIDE) rights += 'Q';
        if (mask & BLACK_KINGSIDE) rights += 'k';
        if (mask & BLACK_QUEENSIDE) rights += 'q';
        castling.push_back(rights.empty() ? "-" : rights);
    }
    std::string timeline;
    std::ostringstream out;
    out << ",\"game_timeline\":[";
    for (std::size_t ply = 0; ply < positions.size(); ++ply) {
        if (ply) out << ',';
        const std::string& key = positions[ply];
        const std::size_t first_space = key.find(' ');
        const std::size_t second_space = first_space == std::string::npos ? std::string::npos :
            key.find(' ', first_space + 1);
        const std::string board_side = second_space == std::string::npos ? key : key.substr(0, second_space);
        const std::string mover = first_space != std::string::npos && first_space + 1 < key.size() ?
            key.substr(first_space + 1, 1) : "w";
        std::vector<std::string> ep_fields;
        ep_fields.push_back("-");
        const char rank = mover == "b" ? '3' : '6';
        for (char file = 'a'; file <= 'h'; ++file) {
            std::string square;
            square += file; square += rank;
            ep_fields.push_back(square);
        }
        std::set<const OpeningReference*> candidates;
        for (std::size_t c = 0; c < castling.size(); ++c) {
            for (std::size_t e = 0; e < ep_fields.size(); ++e) {
                const std::string candidate_key = board_side + " " + castling[c] + " " + ep_fields[e];
                OpeningBook::const_iterator found = book.find(candidate_key);
                if (found == book.end()) continue;
                for (std::vector<OpeningReference>::const_iterator ref = found->second.begin();
                     ref != found->second.end(); ++ref) {
                    if (ref->eco.size() != 3 || !ref->native_pgn ||
                        ref->positions.empty() ||
                        ply < static_cast<std::size_t>(ref->plies) ||
                        ply - static_cast<std::size_t>(ref->plies) > 6) continue;
                    const std::size_t ref_first_space = ref->positions.back().find(' ');
                    const std::size_t ref_second_space = ref_first_space == std::string::npos ?
                        std::string::npos : ref->positions.back().find(' ', ref_first_space + 1);
                    const std::string ref_board_side = ref_second_space == std::string::npos ?
                        ref->positions.back() : ref->positions.back().substr(0, ref_second_space);
                    /* pgn-extract registers only terminal ECO positions. The book map
                       also indexes intermediate states, so exclude those here; its
                       weak hash then ignores castling/EP, but not piece placement
                       or side-to-move. */
                    if (ref_board_side != board_side) continue;
                    candidates.insert(&*ref);
                }
            }
        }
        std::set<std::string> strict_codes;
        OpeningBook::const_iterator strict = book.find(key);
        if (strict != book.end())
            for (std::vector<OpeningReference>::const_iterator ref = strict->second.begin();
                 ref != strict->second.end(); ++ref)
                if (ref->eco.size() == 3 && ref->native_pgn &&
                    ply >= static_cast<std::size_t>(ref->plies) &&
                    ply - static_cast<std::size_t>(ref->plies) <= 6)
                    strict_codes.insert(ref->eco);
        std::string game_ep;
        std::istringstream key_fields(key);
        for (int i = 0; i < 4; ++i) key_fields >> game_ep;
        out << "{\"ply\":" << ply
            << ",\"played_san\":" << json_string(ply == 0 ? "" : sans[ply - 1])
            << ",\"played_uci\":" << json_string(ply == 0 ? "" : move_token(moves[ply - 1]))
            << ",\"position_key_epd\":" << json_string(key)
            << ",\"legal_ep_key_field\":" << json_string(game_ep)
            << ",\"fen\":" << json_string(ply < fens.size() ? fens[ply] : std::string())
            << ",\"strict_eco_codes\":";
        json_string_array(out, std::vector<std::string>(strict_codes.begin(), strict_codes.end()));
        out << ",\"pgn_extract_piece_hash_candidates\":[";
        bool first = true;
        for (std::set<const OpeningReference*>::const_iterator ref = candidates.begin();
             ref != candidates.end(); ++ref) {
            if (!first) out << ',';
            first = false;
            std::vector<std::string> ref_sans, ref_fens;
            reference_replay(**ref, ref_sans, ref_fens);
            out << "{\"eco\":" << json_string((*ref)->eco)
                << ",\"reference_plies\":" << (*ref)->plies
                << ",\"reference_end_position_key_epd\":"
                << json_string((*ref)->positions.empty() ? std::string() : (*ref)->positions.back())
                << ",\"reference_end_fen\":"
                << json_string(ref_fens.empty() ? std::string() : ref_fens.back())
                << ",\"reference_position_keys_epd\":";
            json_string_array(out, (*ref)->positions);
            out << ",\"reference_fens\":";
            json_string_array(out, ref_fens);
            out << ",\"reference_san\":";
            json_string_array(out, ref_sans);
            out << ",\"reference_uci\":";
            json_string_array(out, move_tokens((*ref)->moves));
            out << '}';
        }
        out << "]}";
    }
    out << ']';
    timeline = out.str();
    const std::size_t close = result.audit_json.rfind('}');
    if (close != std::string::npos) result.audit_json.insert(close, timeline);
}

std::string source_game_id(const GameTags& tags, unsigned long number)
{
    for (std::size_t i = 0; i < tags.tags.size(); ++i)
        if (tags.tags[i].name == "Mega2PGNRecordId") return tags.tags[i].value;
    std::ostringstream out;
    out << '#' << number;
    return out.str();
}

std::string trim(const std::string& value)
{
    std::size_t first = 0, last = value.size();
    while (first < last && (value[first] == ' ' || value[first] == '\t' ||
                            value[first] == '\r' || value[first] == '\n')) ++first;
    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t' ||
                            value[last - 1] == '\r' || value[last - 1] == '\n')) --last;
    return value.substr(first, last - first);
}

bool valid_eco(const std::string& value)
{
    return value.size() == 3 && value[0] >= 'A' && value[0] <= 'E' &&
        value[1] >= '0' && value[1] <= '9' && value[2] >= '0' && value[2] <= '9';
}

bool valid_eco4(const std::string& value)
{
    return value.size() == 4 && valid_eco(value.substr(0, 3)) &&
        value[3] >= 'a' && value[3] <= 'z';
}

bool valid_eco5(const std::string& value)
{
    return value.size() == 5 && valid_eco4(value.substr(0, 4)) &&
        value[4] >= '1' && value[4] <= '4';
}

bool valid_eco_any(const std::string& value)
{
    return valid_eco(value) || valid_eco4(value);
}

bool prefix_compatible_eco(const std::string& left, const std::string& right)
{
    return ((valid_eco(left) && valid_eco4(right)) ||
            (valid_eco4(left) && valid_eco(right))) &&
        left.compare(0, 3, right, 0, 3) == 0;
}

std::string unescape_tag(const std::string& raw)
{
    std::string value;
    bool escaped = false;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (escaped) { value += raw[i]; escaped = false; }
        else if (raw[i] == '\\') escaped = true;
        else value += raw[i];
    }
    if (escaped) value += '\\';
    return value;
}

bool parse_tag_line(const std::string& line, std::size_t base, TagSpan& tag)
{
    std::size_t i = 0;
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xef &&
        static_cast<unsigned char>(line[1]) == 0xbb &&
        static_cast<unsigned char>(line[2]) == 0xbf) i = 3;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size() || line[i++] != '[') return false;
    std::size_t name_start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != ']') ++i;
    if (i == name_start || i >= line.size()) return false;
    tag.name = line.substr(name_start, i - name_start);
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size() || line[i++] != '"') return false;
    const std::size_t value_start = i;
    bool escaped = false;
    while (i < line.size()) {
        if (!escaped && line[i] == '"') break;
        if (!escaped && line[i] == '\\') escaped = true;
        else escaped = false;
        ++i;
    }
    if (i >= line.size()) return false;
    const std::size_t value_end = i;
    ++i;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
    if (i >= line.size() || line[i] != ']') return false;
    tag.value = unescape_tag(line.substr(value_start, value_end - value_start));
    tag.value_begin = base + value_start;
    tag.value_end = base + value_end;
    tag.line_begin = base;
    tag.line_end = base + line.size();
    tag.line_after = tag.line_end;
    return true;
}

GameTags scan_header(const std::string& raw)
{
    GameTags result;
    std::size_t at = 0;
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xef &&
        static_cast<unsigned char>(raw[1]) == 0xbb &&
        static_cast<unsigned char>(raw[2]) == 0xbf) at = 3;
    std::size_t last_line_end = at;
    bool saw_tag = false;
    while (at < raw.size()) {
        std::size_t end = raw.find('\n', at);
        const std::size_t content_end = end == std::string::npos ? raw.size() : end;
        std::string line = raw.substr(at, content_end - at);
        if (!saw_tag && trim(line).empty()) {
            at = end == std::string::npos ? content_end : end + 1;
            last_line_end = at;
            continue;
        }
        TagSpan tag;
        if (!parse_tag_line(line, at, tag)) break;
        tag.line_after = end == std::string::npos ? content_end : end + 1;
        result.tags.push_back(tag);
        saw_tag = true;
        last_line_end = end == std::string::npos ? content_end : end + 1;
        at = last_line_end;
    }
    result.insertion_point = saw_tag ? last_line_end : at;
    return result;
}

bool tag_line_candidate(const std::string& line)
{
    TagSpan tag;
    return parse_tag_line(line, 0, tag);
}

class DescriptorInputBuffer : public std::streambuf {
public:
    explicit DescriptorInputBuffer(int descriptor) : descriptor_(descriptor), failed_(false)
    { setg(buffer_, buffer_, buffer_); }

    ~DescriptorInputBuffer()
    {
        if (descriptor_ >= 0) ::close(descriptor_);
    }

    bool failed() const { return failed_; }

    bool close()
    {
        if (descriptor_ < 0) return true;
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return ::close(descriptor) == 0;
    }

protected:
    virtual int_type underflow()
    {
        if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
        ssize_t count;
        do { count = ::read(descriptor_, buffer_, sizeof(buffer_)); }
        while (count < 0 && errno == EINTR);
        if (count < 0) {
            failed_ = true;
            return traits_type::eof();
        }
        if (count == 0) return traits_type::eof();
        setg(buffer_, buffer_, buffer_ + count);
        return traits_type::to_int_type(*gptr());
    }

private:
    int descriptor_;
    bool failed_;
    char buffer_[16384];
};

class GameReader {
public:
    explicit GameReader(std::istream& input) : input_(input), eof_(false),
        has_pending_(false), pending_newline_(false)
#ifdef PGN_ECO_TEST_READ_FAILURE
        , bytes_read_(0), fail_after_(test_fail_after())
#endif
        {}
    bool next(std::string& game, std::string& error)
    {
        game.clear();
        bool have_header = false, saw_movetext = false;
        bool in_brace = false;
        int rav_depth = 0;
        if (has_pending_) {
            game = pending_line_;
            if (pending_newline_) game += '\n';
            have_header = true;
            bool ignored_movetext = false;
            scan_line(pending_line_, in_brace, rav_depth, ignored_movetext, true);
            saw_movetext = false;
            has_pending_ = false;
        }
        std::string line;
        while (!eof_) {
            line.clear();
            char c;
            bool got = false;
            while (true) {
#ifdef PGN_ECO_TEST_READ_FAILURE
                if (fail_after_ >= 0 && bytes_read_ >=
                    static_cast<std::size_t>(fail_after_)) {
                    error = "simulated input read error";
                    return false;
                }
#endif
                if (!input_.get(c)) break;
                got = true;
#ifdef PGN_ECO_TEST_READ_FAILURE
                ++bytes_read_;
#endif
                if (c == '\n') break;
                line += c;
                if (game.size() + line.size() > MAX_GAME_BYTES) {
                    error = "one PGN game exceeds the 64 MiB safety limit";
                    return false;
                }
            }
            if (input_.bad()) {
                error = "input read error";
                return false;
            }
            if (input_.eof()) {
                eof_ = true;
                if (!got) break;
            } else if (input_.fail()) {
                error = "input stream failed before EOF";
                return false;
            }
            const bool newline = got && c == '\n';
            if (game.empty() && line.empty() && newline) { game += '\n'; continue; }
            const bool tag_candidate = !in_brace && tag_line_candidate(line);
            const bool candidate = tag_candidate && rav_depth == 0;
            if (tag_candidate && have_header && saw_movetext) {
                /* Keep separator bytes with the previous record; the new tag starts a record. */
                pending_line_ = line;
                pending_newline_ = newline;
                has_pending_ = true;
                return true;
            }
            game += line;
            if (newline) game += '\n';
            if (candidate) have_header = true;
            scan_line(line, in_brace, rav_depth, saw_movetext, candidate);
            if (eof_) break;
        }
        return !game.empty();
    }
private:
    std::istream& input_;
    bool eof_;
    bool has_pending_;
    bool pending_newline_;
    std::string pending_line_;
#ifdef PGN_ECO_TEST_READ_FAILURE
    std::size_t bytes_read_;
    long fail_after_;
    static long test_fail_after()
    {
        const char* value = std::getenv("PGN_ECO_TEST_READ_ERROR_AFTER");
        if (!value || !*value) return -1;
        char* end = 0;
        long result = std::strtol(value, &end, 10);
        return end != value && *end == '\0' && result >= 0 ? result : -1;
    }
#endif
    static void scan_line(const std::string& line, bool& in_brace, int& rav_depth,
                          bool& saw_movetext, bool tag_line)
    {
        bool semicolon = false;
        if (tag_line) return;
        for (std::size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (semicolon) break;
            if (in_brace) { if (c == '}') in_brace = false; continue; }
            if (c == '{') { in_brace = true; continue; }
            if (c == ';') { semicolon = true; continue; }
            if (c == '%' && i == 0) break;
            if (c == '(') { ++rav_depth; saw_movetext = true; continue; }
            if (c == ')') { if (rav_depth > 0) --rav_depth; continue; }
            if (!std::isspace(static_cast<unsigned char>(c))) saw_movetext = true;
        }
    }
};

/* GameReader needs to replay a detected next header. This wrapper keeps one
   pending line outside the bounded record buffer. */

std::vector<std::string> split_tabs(const std::string& line)
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
        std::size_t tab = line.find('\t', start);
        if (tab == std::string::npos) { fields.push_back(line.substr(start)); break; }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return fields;
}

std::string position_key(const Position& position, double* fen_ms = 0,
                         double* key_ms = 0)
{
    (void)fen_ms; /* Opening keys are serialized directly, without a FEN string. */
    const double start = key_ms ? monotonic_ms() : 0.0;
    const std::string key = position.opening_key();
    if (key_ms) *key_ms += monotonic_ms() - start;
    return key;
}

void insert_reference(OpeningBook& book, BookStats& stats,
                      const std::string& key, const std::string& eco,
                      const std::vector<std::string>& positions,
                      const std::vector<Move>& moves, bool native_pgn)
{
    OpeningReference reference;
    reference.eco = eco;
    reference.positions = positions;
    reference.moves = moves;
    reference.plies = positions.empty() ? 0 : static_cast<int>(positions.size() - 1);
    reference.native_pgn = native_pgn;
    book[key].push_back(reference);
    ++stats.references[key];
    ++stats.code_counts[eco];
    ++stats.rows;
    if (reference.plies > stats.max_plies) stats.max_plies = reference.plies;
    const int match_limit = reference.plies + (native_pgn ? 6 : 0);
    if (match_limit > stats.max_match_plies) stats.max_match_plies = match_limit;
}

bool opening_path(const PgnGame& game, std::vector<std::string>& positions,
                  std::vector<Move>& moves, std::string& error)
{
    if (!game.initial_position.equals(Position::standard())) {
        error = "opening PGN does not start from the standard position";
        return false;
    }
    positions.clear();
    moves.clear();
    std::string initial = position_key(game.initial_position);
    if (initial.empty()) { error = "could not construct opening position key"; return false; }
    positions.push_back(initial);
    const MoveNode* node = game.root;
    while (!node->children.empty()) {
        node = node->children[0];
        if (node->san == "--") { error = "null move in opening line"; return false; }
        moves.push_back(node->move);
        std::string key = position_key(node->position_after);
        if (key.empty()) { error = "could not construct opening position key"; return false; }
        positions.push_back(key);
    }
    return true;
}

bool add_pgn_opening(const std::string& eco, const std::string& pgn,
                     OpeningBook& book, BookStats& stats, std::string& error,
                     bool native_pgn = false)
{
    if (!valid_eco_any(eco)) { error = "invalid ECO code"; return false; }
    PgnDocument document;
    if (!PgnParser::parse(pgn + " *", document, false) || document.games.size() != 1 ||
        !document.errors.empty() || !document.warnings.empty() ||
        document.skipped_variation_count != 0) {
        if (!document.errors.empty()) error = document.errors[0].message;
        else if (!document.warnings.empty()) error = document.warnings[0].message;
        else error = "invalid or unsupported SAN in opening line";
        return false;
    }
    PgnGame* game = document.games[0];
    if (!game->initial_position.equals(Position::standard())) {
        error = "opening PGN does not start from the standard position";
        return false;
    }
    std::vector<std::string> positions;
    std::vector<Move> moves;
    if (!opening_path(*game, positions, moves, error)) return false;
    insert_reference(book, stats, positions.back(), eco, positions, moves, native_pgn);
    return true;
}

/* Scid ECO data has a few stale '+' check suffixes. Retry only book lines,
   removing a terminal '+' from SAN tokens; all remaining SAN and chess legality
   checks still go through the ordinary parser. Stored game PGNs never use this. */
bool add_scid_opening_moves(const std::string& eco, const std::string& moves,
                            OpeningBook& book, BookStats& stats,
                            std::string& error,
                            bool allow_missing_capture_marker = false)
{
    if (add_pgn_opening(eco, moves, book, stats, error)) return true;
    const std::string original_error = error;
    std::size_t at = 0;
    while (at < moves.size()) {
        while (at < moves.size() && std::isspace(static_cast<unsigned char>(moves[at]))) ++at;
        const std::size_t start = at;
        std::size_t end = start;
        while (end < moves.size() &&
               !std::isspace(static_cast<unsigned char>(moves[end]))) ++end;
        if (end > start + 1 && moves[end - 1] == '+') {
            std::string corrected = moves;
            corrected.erase(end - 1, 1);
            std::string retry_error;
            if (add_pgn_opening(eco, corrected, book, stats, retry_error)) {
                ++stats.corrected_check_suffixes;
                error.clear();
                return true;
            }
        }
        at = end;
    }
    /* A small number of original Scid records omit the SAN capture marker.
       Only native Scid-book import enables this retry. Each candidate inserts
       one 'x' into one move token, then the complete line must pass the normal
       strict PGN parser. Accept only a single uniquely valid repaired line. */
    if (allow_missing_capture_marker) {
        std::set<std::string> valid_repairs;
        at = 0;
        while (at < moves.size()) {
            while (at < moves.size() &&
                   std::isspace(static_cast<unsigned char>(moves[at]))) ++at;
            const std::size_t start = at;
            while (at < moves.size() &&
                   !std::isspace(static_cast<unsigned char>(moves[at]))) ++at;
            const std::size_t length = at - start;
            if (length < 3 || moves.find('x', start) < at ||
                (std::isdigit(static_cast<unsigned char>(moves[start])) &&
                 moves.find('.', start) < at)) continue;
            for (std::size_t offset = 1; offset < length; ++offset) {
                std::string candidate = moves;
                candidate.insert(start + offset, "x");
                PgnDocument document;
                if (PgnParser::parse(candidate + " *", document, false) &&
                    document.games.size() == 1 && document.errors.empty() &&
                    document.warnings.empty() &&
                    document.skipped_variation_count == 0)
                    valid_repairs.insert(candidate);
                if (valid_repairs.size() > 1) break;
            }
            if (valid_repairs.size() > 1) break;
        }
        if (valid_repairs.size() == 1) {
            if (add_pgn_opening(eco, *valid_repairs.begin(), book, stats, error)) {
                ++stats.corrected_missing_capture_markers;
                return true;
            }
        }
    }
    error = original_error;
    return false;
}

bool add_parsed_opening(const std::string& eco, const PgnGame& game,
                        OpeningBook& book, BookStats& stats, std::string& error)
{
    if (!valid_eco_any(eco)) { error = "invalid ECO code"; return false; }
    std::vector<std::string> positions;
    std::vector<Move> moves;
    if (!opening_path(game, positions, moves, error)) return false;
    insert_reference(book, stats, positions.back(), eco, positions, moves, true);
    return true;
}

bool load_pgn_extract_book(const std::string& path, OpeningBook& book,
                           BookStats& stats, std::string& error)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) { error = "cannot open ECO book " + path; return false; }
    GameReader reader(input);
    const unsigned long rows_before = stats.rows;
    std::string raw;
    unsigned long records = 0;
    while (true) {
        std::string read_error;
        if (!reader.next(raw, read_error)) {
            if (!read_error.empty()) { error = read_error + " in ECO book " + path; return false; }
            break;
        }
        if (trim(raw).empty()) continue;
        if (records == 0) {
            std::size_t start = 0, header_at = std::string::npos;
            while (start < raw.size()) {
                std::size_t end = raw.find('\n', start);
                if (end == std::string::npos) end = raw.size();
                if (tag_line_candidate(raw.substr(start, end - start))) { header_at = start; break; }
                start = end == raw.size() ? end : end + 1;
            }
            if (header_at != std::string::npos && header_at > 0) raw.erase(0, header_at);
        }
        ++records;
        ++stats.source_records;
        PgnDocument document;
        const bool okay = PgnParser::parse(raw, document, true);
        std::string reason;
        if (!okay || document.games.size() != 1 || !document.errors.empty() ||
            !document.warnings.empty() || document.skipped_variation_count != 0) {
            reason = !document.errors.empty() ? document.errors[0].message :
                "invalid, truncated, or structurally ambiguous PGN record";
        } else {
            PgnGame* game = document.games[0];
            std::string eco;
            unsigned int eco_count = 0;
            for (std::size_t i = 0; i < game->tags.size(); ++i) {
                if (game->tags[i].name == "ECO") { eco = game->tags[i].value; ++eco_count; }
            }
            if (eco_count != 1) reason = eco_count == 0 ? "missing ECO tag" : "duplicate ECO tags";
            else if (!valid_eco(eco)) reason = "invalid three-character ECO tag: " + eco;
            else if (!add_parsed_opening(eco, *game, book, stats, reason)) { }
        }
        if (!reason.empty()) {
            ++stats.invalid_rows;
            if (stats.rejected_details.size() < 20) {
                std::ostringstream detail;
                detail << path << ": reference " << records << ": " << reason;
                stats.rejected_details.push_back(detail.str());
            }
        }
    }
    if (input.bad()) { error = "I/O error reading ECO book " + path; return false; }
    if (records == 0 || stats.rows == rows_before) {
        error = "pgn-extract ECO book contains no valid references"; return false;
    }
    return true;
}

bool uci_line_position(const std::string& text, Position& position, int& plies,
                       std::vector<std::string>* history = 0,
                       std::vector<Move>* move_history = 0)
{
    position = Position::standard();
    plies = 0;
    if (history) {
        history->clear();
        history->push_back(position_key(position));
    }
    if (move_history) move_history->clear();
    std::istringstream moves(text);
    std::string token;
    while (moves >> token) {
        if (token.size() != 4 && token.size() != 5) return false;
        int from = Position::square_from_string(token.substr(0, 2));
        int to = Position::square_from_string(token.substr(2, 2));
        PieceType promotion = NO_PIECE;
        if (token.size() == 5) {
            switch (token[4]) {
            case 'q': promotion = QUEEN; break;
            case 'r': promotion = ROOK; break;
            case 'b': promotion = BISHOP; break;
            case 'n': promotion = KNIGHT; break;
            default: return false;
            }
        }
        Move move;
        Undo undo;
        if (from < 0 || to < 0 || !position.move_from_uci(from, to, promotion, move) ||
            !position.make_move(move, undo)) return false;
        ++plies;
        if (move_history) move_history->push_back(move);
        if (history) history->push_back(position_key(position));
    }
    return true;
}

bool add_scid_entry(const std::string& code, const std::string& line,
                    const std::string& path, int source_line,
                    OpeningBook& book, BookStats& stats, std::string& error)
{
    ++stats.source_records;
    if (code.size() == 5) {
        if (!valid_eco5(code)) { error = "invalid Scid five-character ECO code " + code; return false; }
        ++stats.ignored_scid_depth;
        return true;
    }
    if (!valid_eco_any(code)) { error = "invalid Scid ECO code " + code; return false; }
    std::size_t quote = line.find('"');
    std::size_t close = quote == std::string::npos ? quote : line.find('"', quote + 1);
    if (quote == std::string::npos || close == std::string::npos) {
        error = "malformed Scid ECO entry"; return false;
    }
    std::string moves = trim(line.substr(close + 1));
    if (moves.size() >= 1 && moves[moves.size() - 1] == '*') moves.erase(moves.size() - 1);
    if (!add_scid_opening_moves(code, moves, book, stats, error, true)) {
        /* Scid's shipped table contains a small number of lines with stale
           SAN suffixes. Keep only position lines the strict legal parser accepts. */
        ++stats.invalid_rows;
        std::ostringstream detail;
        detail << path << ':' << source_line << ": skipped illegal ECO line: " << error;
        stats.rejected_details.push_back(detail.str());
        error.clear();
        return true;
    }
    return true;
}

bool parse_escaped_export_field(const std::string& line, std::size_t& at,
                                std::string& field, bool more_fields)
{
    if (line.compare(at, 2, "\\\"") != 0) return false;
    at += 2;
    field.clear();
    while (at < line.size()) {
        if (line[at] == '\\' && at + 1 < line.size() && line[at + 1] == '"') {
            std::size_t after = at + 2;
            if (more_fields) {
                while (after < line.size() && std::isspace(static_cast<unsigned char>(line[after]))) ++after;
                if (line.compare(after, 2, "\\\"") == 0) {
                    at = after;
                    return true;
                }
            } else {
                while (after < line.size() && std::isspace(static_cast<unsigned char>(line[after]))) ++after;
                if (after == line.size()) { at = after; return true; }
            }
            field += '"';
            at += 2;
        } else if (line[at] == '\\' && at + 1 < line.size() && line[at + 1] == '\\') {
            field += '\\';
            at += 2;
        } else {
            field += line[at++];
        }
    }
    return false;
}

bool add_escaped_export_entry(const std::string& line, const std::string& path,
                              int source_line, OpeningBook& book, BookStats& stats,
                              std::string& error)
{
    ++stats.source_records;
    std::size_t at = 0;
    std::string code, name, moves;
    if (!parse_escaped_export_field(line, at, code, true) ||
        !parse_escaped_export_field(line, at, name, true) ||
        !parse_escaped_export_field(line, at, moves, false)) {
        error = "malformed escaped Scid ECO export entry";
        return false;
    }
    if (code.size() == 5) {
        if (!valid_eco5(code)) { error = "invalid Scid five-character ECO code " + code; return false; }
        ++stats.ignored_scid_depth;
        return true;
    }
    if (!valid_eco_any(code)) { error = "invalid Scid ECO code " + code; return false; }
    (void)name;
    if (!add_scid_opening_moves(code, moves, book, stats, error, false)) {
        ++stats.invalid_rows;
        std::ostringstream detail;
        detail << path << ':' << source_line << ": skipped illegal ECO line: " << error;
        stats.rejected_details.push_back(detail.str());
        error.clear();
    }
    return true;
}

bool load_scid_book(const std::string& path, OpeningBook& book, BookStats& stats,
                    std::string& error)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) { error = "cannot open ECO book " + path; return false; }
    std::string line, entry, code;
    const unsigned long rows_before = stats.rows;
    int number = 0, entry_line = 0;
    bool escaped_export = false;
    bool format_known = false;
    while (std::getline(input, line)) {
        ++number;
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        if (line.empty() || line[0] == '#') continue;
        if (!format_known) {
            format_known = true;
            escaped_export = line.compare(0, 2, "\\\"") == 0;
        }
        if (escaped_export) {
            if (!add_escaped_export_entry(line, path, number, book, stats, error)) {
                std::ostringstream out; out << path << ':' << number << ": " << error;
                error = out.str(); return false;
            }
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(line[0]))) {
            if (!entry.empty()) entry += " " + trim(line);
            continue;
        }
        if (!entry.empty()) {
            if (!add_scid_entry(code, entry, path, entry_line, book, stats, error)) {
                std::ostringstream out; out << path << ':' << entry_line << ": " << error;
                error = out.str(); return false;
            }
        }
        std::size_t end = line.find_first_of(" \t");
        if (end == std::string::npos) { error = path + ": malformed Scid ECO entry"; return false; }
        code = line.substr(0, end);
        entry = line;
        entry_line = number;
    }
    if (input.bad()) { error = "I/O error reading ECO book " + path; return false; }
    if (!entry.empty()) {
        if (!add_scid_entry(code, entry, path, entry_line, book, stats, error)) {
            std::ostringstream out; out << path << ':' << entry_line << ": " << error;
            error = out.str(); return false;
        }
    }
    if (stats.rows == rows_before) { error = "Scid ECO book contains no supported 3- or 4-character rows"; return false; }
    return true;
}

void finish_book_stats(const OpeningBook& book, BookStats& stats)
{
    stats.positions = static_cast<unsigned long>(book.size());
    stats.multi_reference_positions = 0;
    stats.conflicts = 0;
    stats.conflict_details.clear();
    for (std::map<std::string, unsigned long>::const_iterator i = stats.references.begin();
         i != stats.references.end(); ++i)
        if (i->second > 1) ++stats.multi_reference_positions;
    RouteDiagnostics routes3, routes4;
    for (OpeningBook::const_iterator i = book.begin(); i != book.end(); ++i) {
        for (std::vector<OpeningReference>::const_iterator ref = i->second.begin();
             ref != i->second.end(); ++ref) {
            const std::string route = canonical_reference_route(*ref);
            if (route.empty()) continue;
            RouteDiagnostics* level = ref->eco.size() == 3 ? &routes3 :
                (ref->eco.size() == 4 ? &routes4 : 0);
            if (!level) continue;
            RouteDiagnostic& diagnostic = (*level)[route];
            diagnostic.codes.insert(ref->eco);
            diagnostic.start = ref->positions[0];
            diagnostic.plies = static_cast<unsigned long>(ref->moves.size());
        }
    }
    const RouteDiagnostics* levels[2] = {&routes3, &routes4};
    for (int level = 0; level < 2; ++level) {
        for (RouteDiagnostics::const_iterator route = levels[level]->begin();
             route != levels[level]->end(); ++route) {
            if (route->second.codes.size() < 2) continue;
            ++stats.conflicts;
            if (stats.conflict_details.size() < 20) {
                std::ostringstream detail;
                detail << "same start and exact legal line (" << route->second.plies
                       << " ply, start " << route->second.start
                       << ") has incompatible ECO codes ";
                bool first = true;
                for (std::set<std::string>::const_iterator code = route->second.codes.begin();
                     code != route->second.codes.end(); ++code) {
                    if (!first) detail << ',';
                    detail << *code;
                    first = false;
                }
                stats.conflict_details.push_back(detail.str());
            }
        }
    }
}

bool load_book(const std::string& path, OpeningBook& book, BookStats& stats,
               std::string& error)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) { error = "cannot open ECO book " + path; return false; }
    std::string first;
    while (std::getline(input, first)) {
        if (!first.empty() && first[first.size() - 1] == '\r') first.erase(first.size() - 1);
        if (!first.empty() && first[0] != '#') break;
    }
    if (!input && first.empty()) { error = "ECO book is unreadable or empty"; return false; }
    bool pgn_format = !first.empty() && first[0] == '[';
    if (!pgn_format && first.find('\t') == std::string::npos) {
        input.clear(); input.seekg(0);
        std::string prefix(8192, '\0');
        input.read(&prefix[0], static_cast<std::streamsize>(prefix.size()));
        prefix.resize(static_cast<std::size_t>(input.gcount()));
        pgn_format = prefix.find("[ECO \"") != std::string::npos;
        input.clear(); input.seekg(0);
    }
    if (pgn_format) {
        if (!load_pgn_extract_book(path, book, stats, error)) return false;
        finish_book_stats(book, stats);
        return true;
    }
    if (first.find('\t') == std::string::npos && first.find('"') != std::string::npos) {
        input.close();
        if (!load_scid_book(path, book, stats, error)) return false;
        finish_book_stats(book, stats);
        return true;
    }
    input.clear(); input.seekg(0);
    if (!input) { error = "cannot rewind ECO book " + path; return false; }
    std::string line;
    int line_number = 0;
    const unsigned long rows_before = stats.rows;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        if (line.empty() || line[0] == '#') continue;
        if (line_number == 1 && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xef &&
            static_cast<unsigned char>(line[1]) == 0xbb &&
            static_cast<unsigned char>(line[2]) == 0xbf) line.erase(0, 3);
        std::vector<std::string> fields = split_tabs(line);
        if (fields.size() >= 3 && fields[0] == "eco") continue;
        if (fields.size() == 1 && line.find("---") == 0) continue;
        if (fields.size() != 3 && fields.size() != 5) {
            std::ostringstream out; out << path << ':' << line_number << ": expected 3 or 5 TSV fields";
            error = out.str(); return false;
        }
        const std::string eco = trim(fields[0]);
        ++stats.source_records;
        if (!valid_eco_any(eco)) {
            std::ostringstream out; out << path << ':' << line_number << ": invalid 3- or 4-character ECO code";
            error = out.str(); return false;
        }
        if (fields.size() >= 5 && !trim(fields[4]).empty()) {
            std::istringstream epd(trim(fields[4]));
            std::string placement, side, rights, ep, extra;
            if (!(epd >> placement >> side >> rights >> ep)) {
                std::ostringstream out; out << path << ':' << line_number << ": malformed EPD field";
                error = out.str(); return false;
            }
            Position pos;
            if (!Position::from_fen(placement + " " + side + " " + rights + " " + ep + " 0 1", pos, 0)) {
                std::ostringstream out; out << path << ':' << line_number << ": invalid EPD position";
                error = out.str(); return false;
            }
            Position from_uci;
            int uci_plies = 0;
            std::vector<std::string> positions;
            std::vector<Move> moves;
            if (!uci_line_position(fields[3], from_uci, uci_plies, &positions, &moves) ||
                position_key(from_uci) != position_key(pos)) {
                std::ostringstream out; out << path << ':' << line_number << ": UCI line does not reach its EPD position";
                error = out.str(); return false;
            }
            insert_reference(book, stats, position_key(pos), eco, positions, moves, false);
        } else if (!add_pgn_opening(eco, fields[2], book, stats, error)) {
            std::ostringstream out; out << path << ':' << line_number << ": " << error;
            error = out.str(); return false;
        }
    }
    if (input.bad() || stats.rows == rows_before) { error = "ECO book is unreadable or contains no opening rows"; return false; }
    finish_book_stats(book, stats);
    return true;
}

void record_match(Classification& best,
                  const OpeningBook& book,
                  const ReferenceHistoryCache& reference_histories,
                  const std::vector<OpeningReference>& references,
                  int wanted_length, int ply,
                  const std::vector<std::string>& game_history,
                  const std::vector<Move>& game_moves,
                  const std::vector<std::string>& game_sans,
                  const std::vector<std::string>& game_fens,
                  const std::string& key, const std::string& fen,
                  bool include_debug, bool audit_details,
                  bool compare_unique_history)
{
    if (ply < best.ply) return;
    std::set<std::string> exact, positional;
    std::vector<const OpeningReference*> eligible;
    std::vector<std::pair<std::string, const OpeningReference*> > ep_siblings;
    bool has_other_code_level = false;
    for (std::vector<OpeningReference>::const_iterator i = references.begin(); i != references.end(); ++i) {
        if (i->native_pgn && (ply < i->plies || ply - i->plies > 6)) continue;
        if (static_cast<int>(i->eco.size()) != wanted_length) {
            if (compare_unique_history && (i->eco.size() == 3 || i->eco.size() == 4))
                has_other_code_level = true;
            continue;
        }
        positional.insert(i->eco);
        eligible.push_back(&*i);
        if (i->positions.size() == game_history.size() &&
            std::equal(i->positions.begin(), i->positions.end(), game_history.begin()) &&
            same_move_history(i->moves, game_moves))
            exact.insert(i->eco);
    }
    if (positional.empty()) return;
    if (audit_details) {
        const std::size_t last_space = key.rfind(' ');
        if (last_space != std::string::npos) {
            const std::string base = key.substr(0, last_space);
            std::istringstream fields(base);
            std::string placement, mover, castling;
            fields >> placement >> mover >> castling;
            std::vector<std::string> ep_fields;
            ep_fields.push_back("-");
            const char rank = mover == "b" ? '3' : '6';
            for (char file = 'a'; file <= 'h'; ++file) {
                std::string square;
                square += file; square += rank;
                ep_fields.push_back(square);
            }
            for (std::size_t e = 0; e < ep_fields.size(); ++e) {
                const std::string sibling_key = base + " " + ep_fields[e];
                if (sibling_key == key) continue;
                OpeningBook::const_iterator sibling = book.find(sibling_key);
                if (sibling == book.end()) continue;
                for (std::vector<OpeningReference>::const_iterator ref = sibling->second.begin();
                     ref != sibling->second.end(); ++ref) {
                    if (static_cast<int>(ref->eco.size()) != wanted_length) continue;
                    if (ref->native_pgn && (ply < ref->plies || ply - ref->plies > 6)) continue;
                    ep_siblings.push_back(std::make_pair(sibling_key, &*ref));
                }
            }
        }
    }
    const bool exact_match = !exact.empty();
    std::map<std::string, HistoryDistance> scores;
    std::string evidence;
    if (positional.size() == 1) {
        if (include_debug)
            evidence = "one ECO code names this exact legal position; history comparison skipped";
        /* Normally an unambiguous position needs no history scoring.  In
           mode 4, however, a same-depth three/four-character cross-prefix
           decision must compare the two existing histories on equal terms. */
        if (has_other_code_level) {
            const ReducedHistory reduced_game = erase_state_cycles(game_history, game_moves);
            for (std::vector<const OpeningReference*>::const_iterator i = eligible.begin();
                 i != eligible.end(); ++i) {
                const OpeningReference& ref = **i;
                HistoryDistance distance = compare_histories(reduced_game,
                                                              reduced_reference_history(ref, reference_histories),
                                                              audit_details);
                distance.reference_plies = ref.plies;
                std::map<std::string, HistoryDistance>::iterator prior = scores.find(ref.eco);
                if (prior == scores.end() || compare_distance(distance, prior->second) < 0 ||
                    (compare_distance(distance, prior->second) == 0 &&
                     distance.reference_plies < prior->second.reference_plies))
                    scores[ref.eco] = distance;
            }
            if (include_debug && !scores.empty()) {
                std::ostringstream summary;
                for (std::map<std::string, HistoryDistance>::const_iterator i = scores.begin();
                     i != scores.end(); ++i) {
                    if (i != scores.begin()) summary << ';';
                    summary << i->first << '[' << distance_text(i->second) << ']';
                }
                evidence = summary.str();
            }
        }
    } else {
        const ReducedHistory reduced_game = erase_state_cycles(game_history, game_moves);
        std::map<std::string, bool> have_score;
        for (std::vector<const OpeningReference*>::const_iterator i = eligible.begin();
             i != eligible.end(); ++i) {
            const OpeningReference& ref = **i;
            HistoryDistance distance = compare_histories(reduced_game,
                                                          reduced_reference_history(ref, reference_histories),
                                                          audit_details);
            distance.reference_plies = ref.plies;
            std::map<std::string, HistoryDistance>::iterator prior = scores.find(ref.eco);
            if (!have_score[ref.eco] || compare_distance(distance, prior->second) < 0 ||
                (compare_distance(distance, prior->second) == 0 &&
                 distance.reference_plies < prior->second.reference_plies)) {
                scores[ref.eco] = distance;
                have_score[ref.eco] = true;
            }
        }
        if (include_debug) {
            std::ostringstream summary;
            bool first = true;
            for (std::map<std::string, HistoryDistance>::const_iterator i = scores.begin();
                 i != scores.end(); ++i) {
                if (!first) summary << ';';
                summary << i->first << '[' << distance_text(i->second) << ']';
                first = false;
            }
            evidence = summary.str();
        }
    }
    std::set<std::string> distance_best;
    if (!exact_match && positional.size() > 1) {
        std::map<std::string, HistoryDistance>::const_iterator best_score = scores.begin();
        for (std::map<std::string, HistoryDistance>::const_iterator i = scores.begin();
             i != scores.end(); ++i)
            if (compare_distance(i->second, best_score->second) < 0) best_score = i;
        for (std::map<std::string, HistoryDistance>::const_iterator i = scores.begin();
             i != scores.end(); ++i)
            if (compare_distance(i->second, best_score->second) == 0) distance_best.insert(i->first);
    }
    const std::set<std::string>& matching = exact_match ? exact :
        (positional.size() == 1 ? positional : distance_best);
    if (matching.empty()) return;
    best.ply = ply;
    best.position_key = include_debug ? key : std::string();
    best.fen = include_debug ? fen : std::string();
    best.alternatives = include_debug ? positional : std::set<std::string>();
    best.exact_alternatives = exact;
    best.matching_codes = matching;
    best.history_scores = scores;
    best.exact_history_match = exact_match;
    best.book_conflict = exact_match && exact.size() > 1;
    best.history_evidence = evidence;
    best.eco.clear();
    if (include_debug) {
        if (exact_match) best.detail = "exact reference moves and full legal position history";
        else if (positional.size() == 1) best.detail = "unique ECO code for the deepest legal position";
        else if (matching.size() == 1) {
            const HistoryDistance& distance = scores[*matching.begin()];
            std::ostringstream detail;
            detail << "minimum historical distance selected " << *matching.begin() << ": "
                   << distance_text(distance);
            best.detail = detail.str();
        } else {
            best.detail = "historical distance ties between " + code_list(matching);
        }
    }
    if (matching.size() != 1) {
        best.state = Classification::AMBIGUOUS;
        if (include_debug) {
            if (exact_match) best.detail = "conflicting ECO codes have the same exact reference history";
            else if (positional.size() > 1) best.detail = "no unique ECO code has minimum historical distance";
        }
    } else {
        best.state = Classification::FOUND;
        best.eco = *matching.begin();
    }
    if (audit_details) {
        const std::string decision = exact_match ?
            (matching.size() == 1 ? "exact-reference-history" : "conflicting-exact-reference-histories") :
            (positional.size() == 1 ? "unique-code-at-position" :
             (matching.size() == 1 ? "unique-minimum-historical-distance" :
                                     "historical-distance-tie"));
        best.audit_json = audit_match_json(eligible, ep_siblings, positional, exact, scores, matching,
                                           exact_match, game_history, game_moves,
                                           game_sans, game_fens, key, fen, decision);
    }
}

std::set<std::string> join_codes(const std::set<std::string>& left,
                                const std::set<std::string>& right)
{
    std::set<std::string> joined = left;
    joined.insert(right.begin(), right.end());
    return joined;
}

std::string history_evidence_text(const Classification& three,
                                  const Classification& four)
{
    std::ostringstream out;
    out << "three-char{" << (three.history_evidence.empty() ? "-" : three.history_evidence)
        << "};four-char{" << (four.history_evidence.empty() ? "-" : four.history_evidence)
        << '}';
    return out.str();
}

std::string level_candidate_text(const char* label, const Classification& candidate)
{
    std::ostringstream out;
    out << label << "{";
    if (candidate.ply < 0) out << "none";
    else out << "ply=" << candidate.ply
             << ",state=" << (candidate.state == Classification::FOUND ? "found" : "ambiguous")
             << ",codes=" << code_list(candidate.matching_codes)
             << ",exact=" << code_list(candidate.exact_alternatives);
    out << '}';
    return out.str();
}

std::string mode4_priority_text(const Classification& three,
                                const Classification& four)
{
    return level_candidate_text("three-char", three) + ";" +
           level_candidate_text("four-char", four);
}

/* Resolve equally deep candidates only after exact histories and the existing
   per-reference history distance have been compared. A longer code refines a
   shorter one only when every remaining candidate has the same ECO prefix. */
bool resolve_same_prefix(const std::set<std::string>& candidates, bool mode4,
                         std::string& selected, std::set<std::string>& remaining)
{
    if (candidates.empty()) return false;
    std::set<std::string> prefixes;
    for (std::set<std::string>::const_iterator i = candidates.begin(); i != candidates.end(); ++i)
        prefixes.insert(i->substr(0, 3));
    if (prefixes.size() != 1) {
        remaining = candidates;
        return false;
    }
    if (mode4) {
        std::set<std::string> four;
        for (std::set<std::string>::const_iterator i = candidates.begin(); i != candidates.end(); ++i)
            if (i->size() == 4) four.insert(*i);
        if (!four.empty()) {
            remaining = four;
            if (four.size() == 1) { selected = *four.begin(); return true; }
            return false;
        }
    }
    remaining = candidates;
    if (remaining.size() == 1) { selected = *remaining.begin(); return true; }
    return false;
}

Classification select_mode4_match(const Classification& best3,
                                  const Classification& best4)
{
    Classification result;
    if (best3.ply < 0) {
        result = best4;
        result.mode4_priority = mode4_priority_text(best3, best4);
        return result;
    }
    if (best4.ply < 0) {
        result = best3;
        result.fallback = result.state == Classification::FOUND;
        result.mode4_priority = mode4_priority_text(best3, best4);
        return result;
    }
    if (best3.ply > best4.ply) {
        result = best3;
        result.fallback = result.state == Classification::FOUND;
        result.mode4_priority = mode4_priority_text(best3, best4);
        if (result.state == Classification::FOUND)
            result.detail = "deeper three-character match selected over shallower four-character match";
        else result.detail = "deeper ambiguous three-character match retained over shallower four-character match";
        return result;
    }
    if (best4.ply > best3.ply) {
        result = best4;
        result.mode4_priority = mode4_priority_text(best3, best4);
        return result;
    }

    const std::set<std::string> exact = join_codes(best3.exact_alternatives,
                                                    best4.exact_alternatives);
    const std::set<std::string> candidates = join_codes(best3.matching_codes,
                                                         best4.matching_codes);
    std::set<std::string> contenders;
    std::map<std::string, HistoryDistance> scores = best3.history_scores;
    for (std::map<std::string, HistoryDistance>::const_iterator i = best4.history_scores.begin();
         i != best4.history_scores.end(); ++i)
        scores[i->first] = i->second;
    std::string selected;
    std::string reason;
    if (!exact.empty()) {
        if (resolve_same_prefix(exact, true, selected, contenders))
            reason = "equal-depth exact history; the unique same-prefix four-character code is more specific";
        else reason = "equal-depth exact histories do not establish one code and prefix";
    } else {
        bool all_scores = !candidates.empty();
        for (std::set<std::string>::const_iterator i = candidates.begin(); i != candidates.end(); ++i)
            if (scores.find(*i) == scores.end()) all_scores = false;
        if (!all_scores) {
            contenders = candidates;
            reason = "equal-depth cross-level candidates lack comparable historical scores";
        } else {
            std::map<std::string, HistoryDistance>::const_iterator minimum = scores.find(*candidates.begin());
            for (std::set<std::string>::const_iterator i = candidates.begin(); i != candidates.end(); ++i)
                if (compare_distance(scores[*i], minimum->second) < 0) minimum = scores.find(*i);
            std::set<std::string> distance_best;
            for (std::set<std::string>::const_iterator i = candidates.begin(); i != candidates.end(); ++i)
                if (compare_distance(scores[*i], minimum->second) == 0) distance_best.insert(*i);
            if (resolve_same_prefix(distance_best, true, selected, contenders))
                reason = "equal-depth candidates compared by historical distance; unique best candidate selected";
            else {
                contenders = distance_best;
                reason = "equal-depth historical evidence ties across incompatible codes or prefixes";
            }
        }
    }

    const bool chosen_four = !selected.empty() && selected.size() == 4;
    result = chosen_four ? best4 : best3;
    result.ply = best3.ply;
    result.position_key = best3.position_key.empty() ? best4.position_key : best3.position_key;
    result.fen = best3.fen.empty() ? best4.fen : best3.fen;
    result.alternatives = candidates;
    result.exact_alternatives = exact;
    result.matching_codes = contenders.empty() ? candidates : contenders;
    result.history_scores = scores;
    result.exact_history_match = !exact.empty();
    result.history_evidence = history_evidence_text(best3, best4);
    result.mode4_priority = mode4_priority_text(best3, best4);
    result.book_conflict = best3.book_conflict || best4.book_conflict;
    result.audit_json.clear();
    if (selected.empty()) {
        result.state = Classification::AMBIGUOUS;
        result.eco.clear();
        result.fallback = false;
        result.detail = reason;
    } else {
        result.state = Classification::FOUND;
        result.eco = selected;
        result.fallback = selected.size() == 3;
        result.detail = reason + "; selected " + selected;
    }
    return result;
}

Classification classify(const PgnGame& game, const OpeningBook& book,
                         const ReferenceHistoryCache& reference_histories,
                         int book_max_plies, bool mode4, bool include_debug,
                         bool audit_details, ClassificationTiming* timing)
{
    Classification result, best3, best4;
    if (include_debug) result.examined_fen = game.initial_position.to_fen();
    const std::string* variant = game.tag_value("Variant");
    if (variant && !variant->empty() && *variant != "Standard" && *variant != "Chess") {
        result.state = Classification::INVALID; result.detail = "non-standard chess variant"; return result;
    }
    Position position = game.initial_position;
    int ply = 0;
    std::string key = position_key(position,
        timing ? &timing->fen_ms : 0, timing ? &timing->key_ms : 0);
    std::vector<std::string> history;
    std::vector<Move> played_moves;
    std::vector<std::string> played_sans;
    std::vector<std::string> played_fens;
    history.push_back(key);
    if (audit_details) played_fens.push_back(position.to_fen());
    double phase_start = timing ? monotonic_ms() : 0.0;
    OpeningBook::const_iterator initial = book.find(key);
    if (timing) timing->lookup_ms += monotonic_ms() - phase_start;
    if (initial != book.end()) {
        const std::string initial_fen = include_debug ? position.to_fen() : std::string();
        phase_start = timing ? monotonic_ms() : 0.0;
        record_match(best3, book, reference_histories, initial->second, 3, 0, history, played_moves,
                     played_sans, played_fens, key, initial_fen, include_debug, audit_details, mode4);
        record_match(best4, book, reference_histories, initial->second, 4, 0, history, played_moves,
                     played_sans, played_fens, key, initial_fen, include_debug, audit_details, mode4);
        if (timing) timing->history_ms += monotonic_ms() - phase_start;
    }
    const MoveNode* parent = game.root;
    const int depth_limit = book_max_plies < MAX_CLASSIFY_PLIES ?
        book_max_plies : MAX_CLASSIFY_PLIES;
    while (!parent->children.empty() && ply < depth_limit) {
        const MoveNode* node = parent->children[0];
        if (node->san == "--") {
            result.state = Classification::INVALID; result.detail = "analytical null move on main line"; return result;
        }
        position = node->position_after;
        played_moves.push_back(node->move);
        if (audit_details) {
            played_sans.push_back(node->san);
            played_fens.push_back(position.to_fen());
        }
        ++ply;
        result.examined_plies = ply;
        if (include_debug) result.examined_fen = position.to_fen();
        key = position_key(position,
            timing ? &timing->fen_ms : 0, timing ? &timing->key_ms : 0);
        history.push_back(key);
        int pieces = 0;
        for (int square = 0; square < 64; ++square)
            if (position.piece_at(square) != 0) ++pieces;
        if (pieces < 20) break;
        phase_start = timing ? monotonic_ms() : 0.0;
        OpeningBook::const_iterator found = book.find(key);
        if (timing) timing->lookup_ms += monotonic_ms() - phase_start;
        if (found != book.end()) {
            const std::string fen = include_debug ? position.to_fen() : std::string();
            phase_start = timing ? monotonic_ms() : 0.0;
            record_match(best3, book, reference_histories, found->second, 3, ply, history, played_moves,
                         played_sans, played_fens, key, fen, include_debug, audit_details, mode4);
            record_match(best4, book, reference_histories, found->second, 4, ply, history, played_moves,
                         played_sans, played_fens, key, fen, include_debug, audit_details, mode4);
            if (timing) timing->history_ms += monotonic_ms() - phase_start;
        }
        parent = node;
    }
    const int examined_plies = result.examined_plies;
    const std::string examined_fen = result.examined_fen;
    if (mode4) result = select_mode4_match(best3, best4);
    else if (best3.ply >= 0) result = best3;
    result.examined_plies = examined_plies;
    result.examined_fen = examined_fen;
    if (audit_details) append_audit_timeline(result, book, history, played_moves,
                                             played_sans, played_fens);
    return result;
}

struct ParseResult {
    PgnGame* game;
    std::string error;
    PgnParseTiming timing;
    ParseResult() : game(0) {}
    ~ParseResult() { delete game; }
private:
    ParseResult(const ParseResult&);
    ParseResult& operator=(const ParseResult&);
};

void parse_game(const std::string& raw, ParseResult& result, int opening_horizon,
                bool measure_timing)
{
    result.game = 0;
    result.error.clear();
    PgnDocument document;
    const bool okay = PgnParser::parse_opening(raw, document, opening_horizon,
                                               true,
                                               measure_timing ? &result.timing : 0);
    if (!okay || document.games.size() != 1 || !document.errors.empty() ||
        !document.warnings.empty() || document.skipped_variation_count != 0) {
        result.error = document.errors.empty() ? "invalid, truncated, or structurally ambiguous PGN" :
            document.errors[0].message;
        return;
    }
    result.game = document.games[0];
    document.games[0] = 0;
}

enum EcoStatus { ECO_ABSENT, ECO_SINGLE, ECO_INVALID, ECO_DUPLICATE };
EcoStatus inspect_eco(const GameTags& tags, const TagSpan** found)
{
    *found = 0;
    unsigned int count = 0;
    for (std::size_t i = 0; i < tags.tags.size(); ++i)
        if (tags.tags[i].name == "ECO") {
            if (count == 0) *found = &tags.tags[i];
            ++count;
        }
    if (count == 0) return ECO_ABSENT;
    if (count != 1) return ECO_DUPLICATE;
    if (!valid_eco_any((*found)->value)) return ECO_INVALID;
    return ECO_SINGLE;
}

struct WorkItem {
    unsigned long game_number;
    unsigned long file_game_number;
    std::string input_path;
    std::string raw;
    GameTags headers;
    EcoStatus eco_status;
    std::size_t old_tag_index;
    bool has_old_tag;
    bool preserve_existing;
    std::string source_id;
    ParseResult parsed;
    Classification classification;
    ClassificationTiming classification_timing;
    double parse_ms;
    double classify_ms;
    bool done;
    bool worker_failed;
    char worker_error[256];
    WorkItem() : game_number(0), file_game_number(0), eco_status(ECO_ABSENT), old_tag_index(0),
        has_old_tag(false), preserve_existing(false), parse_ms(0.0),
        classify_ms(0.0), done(false), worker_failed(false)
    { worker_error[0] = '\0'; }
private:
    WorkItem(const WorkItem&);
    WorkItem& operator=(const WorkItem&);
};

/* Workers only read the loaded book and its precomputed immutable history
   cache. The coordinator owns PGN reading, reporting, statistics and writes. */
class ClassifierPool {
public:
    ClassifierPool(const OpeningBook& book,
                   const ReferenceHistoryCache& histories,
                   int book_max_plies, bool mode4, bool debug,
                   bool audit_details, bool timing)
        : book_(book), histories_(histories), book_max_plies_(book_max_plies),
          mode4_(mode4), debug_(debug), audit_details_(audit_details),
          timing_(timing), ready_(false), stopping_(false),
          mutex_ready_(false), work_ready_(false), done_ready_(false)
    {
        if (pthread_mutex_init(&mutex_, 0) != 0) return;
        mutex_ready_ = true;
        if (pthread_cond_init(&work_available_, 0) != 0) return;
        work_ready_ = true;
        if (pthread_cond_init(&result_available_, 0) != 0) return;
        done_ready_ = true;
        ready_ = true;
    }

    ~ClassifierPool()
    {
        stop();
        if (done_ready_) pthread_cond_destroy(&result_available_);
        if (work_ready_) pthread_cond_destroy(&work_available_);
        if (mutex_ready_) pthread_mutex_destroy(&mutex_);
    }

    bool start(unsigned long count, std::string& error)
    {
        if (!ready_) { error = "cannot initialize worker synchronization"; return false; }
        try {
            threads_.reserve(static_cast<std::size_t>(count));
            for (unsigned long i = 0; i < count; ++i) {
                pthread_t thread;
                const int rc = pthread_create(&thread, 0, worker_entry, this);
                if (rc != 0) {
                    std::ostringstream message;
                    message << "cannot create worker thread " << (i + 1) << ": "
                            << std::strerror(rc);
                    error = message.str();
                    stop();
                    return false;
                }
                threads_.push_back(thread);
            }
        } catch (const std::exception& exception) {
            error = std::string("cannot start worker threads: ") + exception.what();
            stop();
            return false;
        } catch (...) {
            error = "cannot start worker threads";
            stop();
            return false;
        }
        return true;
    }

    bool submit(WorkItem* item, std::string& error)
    {
        if (!ready_ || threads_.empty()) { error = "worker pool is not running"; return false; }
        const int rc = pthread_mutex_lock(&mutex_);
        if (rc != 0) { error = "cannot lock worker queue"; return false; }
        if (stopping_) {
            pthread_mutex_unlock(&mutex_);
            error = "worker pool is stopping";
            return false;
        }
        try {
            queue_.push_back(item);
        } catch (const std::exception& exception) {
            pthread_mutex_unlock(&mutex_);
            error = std::string("cannot queue game: ") + exception.what();
            return false;
        } catch (...) {
            pthread_mutex_unlock(&mutex_);
            error = "cannot queue game";
            return false;
        }
        pthread_cond_signal(&work_available_);
        pthread_mutex_unlock(&mutex_);
        return true;
    }

    bool wait(WorkItem* item, std::string& error)
    {
        const int rc = pthread_mutex_lock(&mutex_);
        if (rc != 0) { error = "cannot lock worker result"; return false; }
        while (!item->done) {
            const int wait_rc = pthread_cond_wait(&result_available_, &mutex_);
            if (wait_rc != 0) {
                pthread_mutex_unlock(&mutex_);
                error = "cannot wait for worker result";
                return false;
            }
        }
        pthread_mutex_unlock(&mutex_);
        if (item->worker_failed) {
            error = item->worker_error;
            return false;
        }
        return true;
    }

    void process_inline(WorkItem* item)
    {
        try {
            process(*item);
        } catch (const std::exception& exception) {
            item->worker_failed = true;
            std::snprintf(item->worker_error, sizeof(item->worker_error),
                "worker failed for game %lu: %s", item->game_number,
                exception.what());
        } catch (...) {
            item->worker_failed = true;
            std::snprintf(item->worker_error, sizeof(item->worker_error),
                "worker failed for game %lu", item->game_number);
        }
        item->done = true;
    }

    void stop()
    {
        if (!ready_) return;
        pthread_mutex_lock(&mutex_);
        stopping_ = true;
        pthread_cond_broadcast(&work_available_);
        pthread_mutex_unlock(&mutex_);
        for (std::size_t i = 0; i < threads_.size(); ++i)
            pthread_join(threads_[i], 0);
        threads_.clear();
        ready_ = false;
    }

private:
    const OpeningBook& book_;
    const ReferenceHistoryCache& histories_;
    int book_max_plies_;
    bool mode4_;
    bool debug_;
    bool audit_details_;
    bool timing_;
    bool ready_;
    bool stopping_;
    bool mutex_ready_;
    bool work_ready_;
    bool done_ready_;
    pthread_mutex_t mutex_;
    pthread_cond_t work_available_;
    pthread_cond_t result_available_;
    std::vector<pthread_t> threads_;
    std::deque<WorkItem*> queue_;

    static void* worker_entry(void* context)
    {
        static_cast<ClassifierPool*>(context)->worker_loop();
        return 0;
    }

    void process(WorkItem& item)
    {
        if (item.preserve_existing) {
            item.classification.state = Classification::NONE;
            return;
        }
        double phase_start = timing_ ? monotonic_ms() : 0.0;
        parse_game(item.raw, item.parsed,
                   book_max_plies_ < MAX_CLASSIFY_PLIES ?
                       (book_max_plies_ < 0 ? 0 : book_max_plies_) : MAX_CLASSIFY_PLIES,
                   timing_);
        if (timing_) item.parse_ms = monotonic_ms() - phase_start;
        phase_start = timing_ ? monotonic_ms() : 0.0;
        if (!item.parsed.game) {
            item.classification.state = Classification::INVALID;
            item.classification.detail = item.parsed.error;
        } else {
            item.classification = classify(*item.parsed.game, book_, histories_,
                book_max_plies_, mode4_, debug_, audit_details_,
                timing_ ? &item.classification_timing : 0);
        }
        if (timing_) item.classify_ms = monotonic_ms() - phase_start;
    }

    void worker_loop()
    {
        for (;;) {
            pthread_mutex_lock(&mutex_);
            while (queue_.empty() && !stopping_)
                pthread_cond_wait(&work_available_, &mutex_);
            if (queue_.empty() && stopping_) {
                pthread_mutex_unlock(&mutex_);
                return;
            }
            WorkItem* item = queue_.front();
            queue_.pop_front();
            pthread_mutex_unlock(&mutex_);

            try {
                process(*item);
            } catch (const std::exception& exception) {
                item->worker_failed = true;
                std::snprintf(item->worker_error, sizeof(item->worker_error),
                    "worker failed for game %lu: %s", item->game_number,
                    exception.what());
            } catch (...) {
                item->worker_failed = true;
                std::snprintf(item->worker_error, sizeof(item->worker_error),
                    "worker failed for game %lu", item->game_number);
            }

            pthread_mutex_lock(&mutex_);
            item->done = true;
            pthread_cond_broadcast(&result_available_);
            pthread_mutex_unlock(&mutex_);
        }
    }
};

bool write_all(FILE* output, const std::string& value)
{
#ifdef PGN_ECO_TEST_WRITE_FAILURE
    static std::size_t written = 0;
    const char* fail_after = std::getenv("PGN_ECO_TEST_WRITE_ERROR_AFTER");
    if (fail_after && *fail_after) {
        char* end = 0;
        const unsigned long parsed = std::strtoul(fail_after, &end, 10);
        if (end != fail_after && *end == '\0') {
            if (written >= static_cast<std::size_t>(parsed)) return false;
            const std::size_t remaining = static_cast<std::size_t>(parsed) - written;
            const std::size_t amount = std::min(value.size(), remaining);
            if (amount && std::fwrite(value.data(), 1, amount, output) != amount) return false;
            written += amount;
            return amount == value.size();
        }
    }
#endif
    return value.empty() || std::fwrite(value.data(), 1, value.size(), output) == value.size();
}

bool starts_with_utf8_bom(const std::string& value)
{
    return value.size() >= 3 && static_cast<unsigned char>(value[0]) == 0xef &&
        static_cast<unsigned char>(value[1]) == 0xbb &&
        static_cast<unsigned char>(value[2]) == 0xbf;
}

std::size_t leading_line_breaks(const std::string& value)
{
    std::size_t at = 0;
    std::size_t count = 0;
    while (at < value.size()) {
        if (value[at] == ' ' || value[at] == '\t') { ++at; continue; }
        if (value[at] == '\n') {
            ++at;
            ++count;
        } else if (value[at] == '\r' && at + 1 < value.size() && value[at + 1] == '\n') {
            at += 2;
            ++count;
        } else break;
    }
    return count;
}

std::size_t trailing_line_breaks(const std::string& value)
{
    std::size_t at = value.size();
    std::size_t count = 0;
    while (at > 0) {
        if (value[at - 1] == ' ' || value[at - 1] == '\t') { --at; continue; }
        if (value[at - 1] == '\n') {
            --at;
            if (at > 0 && value[at - 1] == '\r') --at;
            ++count;
        } else break;
    }
    return count;
}

bool contains_crlf(const std::string& value)
{
    return value.find("\r\n") != std::string::npos;
}

struct PatchEdit {
    std::size_t begin, end;
    std::string replacement;
};

struct PatchEditOrder {
    bool operator()(const PatchEdit& a, const PatchEdit& b) const
    { return a.begin > b.begin; }
};

bool patch_game(const std::string& raw, const GameTags& tags,
                EcoStatus status, const std::string& desired,
                std::string& output)
{
    output = raw;
    std::vector<const TagSpan*> eco_tags;
    for (std::size_t i = 0; i < tags.tags.size(); ++i)
        if (tags.tags[i].name == "ECO") eco_tags.push_back(&tags.tags[i]);
    if (status != ECO_ABSENT && eco_tags.empty()) return false;
    if (status == ECO_SINGLE || status == ECO_INVALID || status == ECO_DUPLICATE) {
        const TagSpan& kept = *eco_tags[0];
        for (std::size_t i = 0; i < eco_tags.size(); ++i) {
            const TagSpan& tag = *eco_tags[i];
            if (tag.line_begin > tag.value_begin || tag.value_begin > tag.value_end ||
                tag.value_end > tag.line_end || tag.line_end > tag.line_after ||
                tag.line_after > raw.size()) return false;
            const std::string line = raw.substr(tag.line_begin, tag.line_end - tag.line_begin);
            TagSpan verified;
            if (!parse_tag_line(line, tag.line_begin, verified) || verified.name != "ECO" ||
                verified.value_begin != tag.value_begin || verified.value_end != tag.value_end ||
                verified.value != tag.value) return false;
            if (tag.line_after != tag.line_end &&
                !(tag.line_after == tag.line_end + 1 && raw[tag.line_end] == '\n')) return false;
        }
        /* Apply edits from right to left so original byte offsets stay valid. */
        std::vector<PatchEdit> edits;
        PatchEdit value_edit;
        value_edit.begin = kept.value_begin;
        value_edit.end = kept.value_end;
        value_edit.replacement = desired;
        edits.push_back(value_edit);
        for (std::size_t i = 1; i < eco_tags.size(); ++i) {
            PatchEdit remove;
            remove.begin = eco_tags[i]->line_begin;
            remove.end = eco_tags[i]->line_after;
            remove.replacement.clear();
            edits.push_back(remove);
        }
        std::sort(edits.begin(), edits.end(), PatchEditOrder());
        for (std::size_t i = 0; i < edits.size(); ++i) {
            if (edits[i].begin > edits[i].end || edits[i].end > output.size()) return false;
            output.replace(edits[i].begin, edits[i].end - edits[i].begin, edits[i].replacement);
        }
        return true;
    }
    if (status != ECO_ABSENT) return false;
    std::size_t at = tags.insertion_point;
    std::string eol = "\n";
    std::size_t first_lf = raw.find('\n');
    if (first_lf != std::string::npos && first_lf > 0 && raw[first_lf - 1] == '\r') eol = "\r\n";
    output.insert(at, "[ECO \"" + desired + "\"]" + eol);
    return true;
}

struct InputFileInfo {
    std::string path;
    std::string canonical;
    struct stat status;
};

struct BookMetadata {
    struct stat status;
    bool available;
    std::string canonical;
    BookMetadata() : available(false) {}
};

std::string path_directory(const std::string& path)
{
    const std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

std::string path_basename(const std::string& path)
{
    const std::string::size_type slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool canonical_path(const std::string& path, std::string& result)
{
    char* resolved = ::realpath(path.c_str(), 0);
    if (!resolved) return false;
    result = resolved;
    std::free(resolved);
    return true;
}

bool same_identity(const struct stat& a, const struct stat& b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

bool unchanged_input(const struct stat& original, const struct stat& current)
{
    if (!same_identity(original, current) || original.st_size != current.st_size ||
        original.st_mode != current.st_mode || original.st_uid != current.st_uid ||
        original.st_gid != current.st_gid || original.st_mtime != current.st_mtime ||
        original.st_ctime != current.st_ctime)
        return false;
#if defined(__linux__)
    if (original.st_mtim.tv_nsec != current.st_mtim.tv_nsec ||
        original.st_ctim.tv_nsec != current.st_ctim.tv_nsec)
        return false;
#endif
    return true;
}

bool probe_output_directory(const std::string& directory, bool require_link,
                            std::string& error)
{
    std::string pattern = directory + "/.pgn-eco-probe.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int fd = ::mkstemp(&name[0]);
    if (fd < 0) {
        error = "cannot create a temporary file in output directory " + directory +
            ": " + std::strerror(errno);
        return false;
    }
    if (::close(fd) != 0) {
        const int saved_errno = errno;
        ::unlink(&name[0]);
        error = "cannot close output-directory test file: " +
            std::string(std::strerror(saved_errno));
        return false;
    }
    std::string linked_name;
    if (require_link) {
        linked_name = std::string(&name[0]) + ".publish-test";
        if (::link(&name[0], linked_name.c_str()) != 0) {
            const int saved_errno = errno;
            ::unlink(&name[0]);
            error = "output directory cannot safely publish new files " + directory +
                ": " + std::strerror(saved_errno);
            return false;
        }
    }
    bool removed = true;
    int remove_errno = 0;
    if (require_link && ::unlink(linked_name.c_str()) != 0) {
        removed = false;
        remove_errno = errno;
    }
    if (::unlink(&name[0]) != 0 && removed) {
        removed = false;
        remove_errno = errno;
    }
    if (!removed) {
        error = "cannot remove output-directory test file: " +
            std::string(std::strerror(remove_errno));
        return false;
    }
    return true;
}

bool validate_input_files(const std::vector<std::string>& paths, bool inplace,
                          const BookMetadata& book,
                          std::vector<InputFileInfo>& files,
                          std::set<std::string>& canonical_inputs,
                          std::string& error)
{
    std::set<std::string> inplace_ids;
    std::set<std::string> inplace_directories;
    std::map<std::string, std::string> resolved_directories;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        InputFileInfo info;
        info.path = paths[i];
        struct stat expected_status;
        if (::lstat(info.path.c_str(), &expected_status) != 0) {
            error = "cannot inspect input " + info.path + ": " + std::strerror(errno);
            return false;
        }
        const bool is_symlink = S_ISLNK(expected_status.st_mode);
        if (inplace && is_symlink) {
            error = "in-place processing does not accept symbolic links: " + info.path;
            return false;
        }
        if (is_symlink && ::stat(info.path.c_str(), &expected_status) != 0) {
            error = "cannot inspect input target " + info.path + ": " + std::strerror(errno);
            return false;
        }
        if (!S_ISREG(expected_status.st_mode)) {
            error = "input is not a regular file: " + info.path;
            return false;
        }
        int open_flags = O_RDONLY | O_NONBLOCK;
#ifdef O_NOFOLLOW
        if (inplace) open_flags |= O_NOFOLLOW;
#endif
        int fd = ::open(info.path.c_str(), open_flags);
        if (fd < 0) {
            error = "cannot open input " + info.path + ": " + std::strerror(errno);
            return false;
        }
        if (::fstat(fd, &info.status) != 0) {
            const int saved_errno = errno;
            ::close(fd);
            error = "cannot inspect input " + info.path + ": " + std::strerror(saved_errno);
            return false;
        }
        if (!S_ISREG(info.status.st_mode)) {
            ::close(fd);
            error = "input is not a regular file: " + info.path;
            return false;
        }
        if (!unchanged_input(expected_status, info.status)) {
            ::close(fd);
            error = "input changed during validation: " + info.path;
            return false;
        }
        if (::close(fd) != 0) {
            error = "cannot close input " + info.path + ": " + std::strerror(errno);
            return false;
        }
        if (!canonical_path(info.path, info.canonical)) {
            error = "cannot resolve input path " + info.path + ": " + std::strerror(errno);
            return false;
        }
        canonical_inputs.insert(info.canonical);
        if (book.available && same_identity(info.status, book.status)) {
            error = "input refers to the active ECO book: " + info.path;
            return false;
        }
        if (!book.canonical.empty() && info.canonical == book.canonical) {
            error = "input refers to the active ECO book: " + info.path;
            return false;
        }
        if (inplace) {
            if (info.status.st_nlink > 1) {
                error = "in-place processing does not accept hard-linked files: " + info.path;
                return false;
            }
            std::ostringstream id;
            id << info.status.st_dev << ':' << info.status.st_ino;
            if (!inplace_ids.insert(id.str()).second) {
                error = "duplicate or aliased in-place input: " + info.path;
                return false;
            }
            const std::string directory = path_directory(info.path);
            std::string canonical_directory;
            std::map<std::string, std::string>::const_iterator cached =
                resolved_directories.find(directory);
            if (cached != resolved_directories.end()) {
                canonical_directory = cached->second;
            } else {
                if (!canonical_path(directory, canonical_directory)) {
                    error = "cannot resolve in-place directory for " + info.path;
                    return false;
                }
                resolved_directories[directory] = canonical_directory;
            }
            if (inplace_directories.insert(canonical_directory).second &&
                !probe_output_directory(canonical_directory, false, error)) return false;
        }
        files.push_back(info);
    }
    return true;
}

bool validate_new_destination(const std::string& path,
                              const std::string& canonical_parent,
                              const std::set<std::string>& canonical_inputs,
                              const std::string& canonical_book,
                              std::string& error)
{
    const std::string base = path_basename(path);
    if (base.empty() || base == "." || base == "..") {
        error = "invalid output filename: " + path;
        return false;
    }
    const std::string key = canonical_parent == "/" ? canonical_parent + base :
        canonical_parent + "/" + base;
    if (canonical_inputs.find(key) != canonical_inputs.end()) {
        error = "output refers to an input file: " + path;
        return false;
    }
    if (!canonical_book.empty() && key == canonical_book) {
        error = "output refers to the active ECO book: " + path;
        return false;
    }
    struct stat st;
    if (::lstat(path.c_str(), &st) == 0) {
        error = "output already exists; choose a new path: " + path;
        return false;
    }
    if (errno != ENOENT) {
        error = "cannot inspect output path " + path + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

bool create_temp_output(const std::string& final_path, const struct stat* preserve,
                        std::string& temp_path, FILE*& output, std::string& error)
{
    temp_path = path_directory(final_path) + "/.pgn-eco.XXXXXX";
    std::vector<char> name(temp_path.begin(), temp_path.end());
    name.push_back('\0');
    const int fd = ::mkstemp(&name[0]);
    if (fd < 0) {
        error = "cannot create temporary output beside " + final_path + ": " +
            std::strerror(errno);
        return false;
    }
    temp_path = &name[0];
    if (preserve) {
        if (::fchown(fd, preserve->st_uid, preserve->st_gid) != 0 ||
            ::fchmod(fd, preserve->st_mode & 07777) != 0) {
            error = "cannot preserve input file ownership or permissions: " +
                std::string(std::strerror(errno));
            ::close(fd);
            ::unlink(temp_path.c_str());
            temp_path.clear();
            return false;
        }
    }
    output = ::fdopen(fd, "wb");
    if (!output) {
        error = "cannot open temporary output: " + std::string(std::strerror(errno));
        ::close(fd);
        ::unlink(temp_path.c_str());
        temp_path.clear();
        return false;
    }
    return true;
}

bool finish_temp_output(const std::string& temp_path, const std::string& final_path,
                        FILE*& output, bool inplace, const struct stat* original,
                        mode_t output_permissions, std::string& error)
{
    bool okay = true;
    if (!inplace && ::fchmod(::fileno(output), output_permissions & 0777) != 0) {
        error = "could not set output file permissions: " +
            std::string(std::strerror(errno));
        okay = false;
    }
    if (okay && (std::fflush(output) != 0 || ::fsync(::fileno(output)) != 0)) {
        error = "could not flush temporary output: " + std::string(std::strerror(errno));
        okay = false;
    }
    if (std::fclose(output) != 0) {
        if (okay) error = "could not close temporary output: " + std::string(std::strerror(errno));
        okay = false;
    }
    output = 0;
    if (okay && inplace) {
        struct stat current;
        if (!original || ::lstat(final_path.c_str(), &current) != 0 ||
            S_ISLNK(current.st_mode) || !unchanged_input(*original, current)) {
            error = "in-place input changed during processing: " + final_path;
            okay = false;
        } else if (::rename(temp_path.c_str(), final_path.c_str()) != 0) {
            error = "cannot atomically replace input " + final_path + ": " +
                std::strerror(errno);
            okay = false;
        }
    } else if (okay && ::link(temp_path.c_str(), final_path.c_str()) != 0) {
        error = "cannot publish output without overwrite at " + final_path + ": " +
            std::strerror(errno);
        okay = false;
    }
    ::unlink(temp_path.c_str());
    return okay;
}

std::string find_default_book(BookMetadata& metadata, bool mode4)
{
    metadata.available = false;
    metadata.canonical.clear();
    const char* env = std::getenv("PGN_ECO_BOOK");
    if (env && *env) return env;
    const std::string leaf = mode4 ? "scid.eco" : "lichess-openings.tsv";
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string candidate;
    if (xdg && *xdg && xdg[0] == '/') {
        candidate = std::string(xdg) + "/pgn-eco/" + leaf;
    } else {
        const char* home = std::getenv("HOME");
        if (home && *home) candidate = std::string(home) + "/.local/share/pgn-eco/" + leaf;
    }
    if (!candidate.empty()) {
        struct stat st;
        if (::stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            metadata.status = st;
            metadata.available = true;
            canonical_path(candidate, metadata.canonical);
            return candidate;
        }
    }
    const std::string paths[] = {std::string("share/pgn-eco/") + leaf,
        std::string("/usr/local/share/pgn-eco/") + leaf,
        std::string("/usr/share/pgn-eco/") + leaf};
    for (std::size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        struct stat st;
        if (::stat(paths[i].c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            metadata.status = st;
            metadata.available = true;
            canonical_path(paths[i], metadata.canonical);
            return paths[i];
        }
    }
    return "";
}

std::string xdg_data_dir()
{
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg && xdg[0] == '/') return std::string(xdg) + "/pgn-eco";
    const char* home = std::getenv("HOME");
    if (home && *home) return std::string(home) + "/.local/share/pgn-eco";
    return "";
}

bool make_directories(const std::string& path)
{
    if (path.empty() || path[0] != '/') return false;
    std::string part;
    for (std::size_t i = 1; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != '/') continue;
        part = path.substr(0, i);
        if (part.empty()) continue;
        if (::mkdir(part.c_str(), 0700) == 0) continue;
        if (errno != EEXIST) return false;
        struct stat st;
        if (::stat(part.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    }
    return true;
}

bool run_curl(const std::string& url, const std::string& destination)
{
    pid_t child = ::fork();
    if (child < 0) return false;
    if (child == 0) {
        ::execlp("curl", "curl", "--fail", "--location", "--silent", "--show-error",
                 "--proto", "=https", "--proto-redir", "=https",
                 "--tlsv1.2", "--max-time", "180",
                 "--output", destination.c_str(), url.c_str(), static_cast<char*>(0));
        _exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) if (errno != EINTR) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool file_sha256(const std::string& path, std::string& digest)
{
    int pipes[2];
    if (::pipe(pipes) != 0) return false;
    pid_t child = ::fork();
    if (child < 0) { ::close(pipes[0]); ::close(pipes[1]); return false; }
    if (child == 0) {
        ::dup2(pipes[1], STDOUT_FILENO);
        ::close(pipes[0]); ::close(pipes[1]);
        ::execlp("sha256sum", "sha256sum", path.c_str(), static_cast<char*>(0));
        _exit(127);
    }
    ::close(pipes[1]);
    char buffer[256];
    std::string output;
    ssize_t count;
    while ((count = ::read(pipes[0], buffer, sizeof(buffer))) > 0) output.append(buffer, static_cast<std::size_t>(count));
    ::close(pipes[0]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) if (errno != EINTR) return false;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || output.size() < 64) return false;
    digest = output.substr(0, 64);
    return true;
}

bool update_lichess_book(const std::string& directory, std::string& error)
{
    std::string stage_template = directory + "/.download.XXXXXX";
    std::vector<char> stage_name(stage_template.begin(), stage_template.end()); stage_name.push_back('\0');
    char* created = ::mkdtemp(&stage_name[0]);
    if (!created) { error = "cannot create a staging directory: " + std::string(std::strerror(errno)); return false; }
    const std::string stage(created);
    const char* volumes[] = {"a", "b", "c", "d", "e"};
    std::vector<std::string> files;
    bool okay = true;
    for (std::size_t i = 0; i < 5; ++i) {
        const std::string name = std::string(volumes[i]) + ".tsv";
        const std::string file = stage + "/" + name;
        const std::string url = std::string("https://raw.githubusercontent.com/lichess-org/chess-openings/") +
            LICHESS_REVISION + "/" + name;
        if (!run_curl(url, file)) { ::unlink(file.c_str()); error = "HTTPS download failed for " + url; okay = false; break; }
        struct stat st;
        if (::stat(file.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 64 || st.st_size > 16 * 1024 * 1024) {
            ::unlink(file.c_str()); error = "downloaded opening file has an implausible size: " + name; okay = false; break;
        }
        files.push_back(file);
    }
    std::string combined = directory + "/.lichess-openings.XXXXXX";
    int fd = -1;
    if (okay) {
        std::vector<char> name(combined.begin(), combined.end()); name.push_back('\0');
        fd = ::mkstemp(&name[0]);
        if (fd < 0) { error = "cannot create book staging file: " + std::string(std::strerror(errno)); okay = false; }
        else combined = &name[0];
    }
    FILE* output = fd >= 0 ? ::fdopen(fd, "wb") : 0;
    if (okay && !output) { ::close(fd); fd = -1; error = "cannot open staged ECO book"; okay = false; }
    if (okay) {
        std::string header = "# Source: lichess-org/chess-openings\n# Revision: " + std::string(LICHESS_REVISION) + "\n";
        if (!write_all(output, header)) okay = false;
        for (std::size_t i = 0; okay && i < files.size(); ++i) {
            std::string digest;
            if (!file_sha256(files[i], digest)) { error = "sha256sum is required to record downloaded data"; okay = false; break; }
            const std::string leaf = std::string(volumes[i]) + ".tsv";
            if (!write_all(output, "# SHA256 " + leaf + " " + digest + "\n")) { okay = false; break; }
            std::ifstream input(files[i].c_str(), std::ios::binary);
            if (!input) { error = "cannot read staged download " + leaf; okay = false; break; }
            char bytes[32768];
            while (input) {
                input.read(bytes, sizeof(bytes));
                std::streamsize got = input.gcount();
                if (got > 0 && std::fwrite(bytes, 1, static_cast<std::size_t>(got), output) != static_cast<std::size_t>(got)) { error = "cannot assemble staged ECO book"; okay = false; break; }
            }
            if (input.bad()) { error = "read error in staged download " + leaf; okay = false; }
            if (okay && !write_all(output, "\n")) okay = false;
        }
        if (std::fflush(output) != 0 || ::fsync(::fileno(output)) != 0) { error = "cannot flush staged ECO book"; okay = false; }
        if (std::fclose(output) != 0) { error = "cannot close staged ECO book"; okay = false; }
        output = 0;
    }
    if (okay) {
        OpeningBook validation;
        BookStats stats;
        if (!load_book(combined, validation, stats, error) ||
#ifndef PGN_ECO_TESTING
            stats.rows < 1000 || stats.positions < 500
#else
            stats.rows == 0 || stats.positions == 0
#endif
            ) {
            if (error.empty()) error = "downloaded Lichess book failed the completeness check";
            okay = false;
        }
    }
    const std::string target = directory + "/lichess-openings.tsv";
    if (okay && ::rename(combined.c_str(), target.c_str()) != 0) {
        error = "cannot atomically activate ECO book: " + std::string(std::strerror(errno)); okay = false;
    }
    if (!combined.empty()) ::unlink(combined.c_str());
    for (std::size_t i = 0; i < files.size(); ++i) ::unlink(files[i].c_str());
    ::rmdir(stage.c_str());
    if (!okay && error.empty()) error = "ECO book update failed";
    return okay;
}

std::string expected_scid_checksum()
{
#ifdef PGN_ECO_TESTING
    const char* test_value = std::getenv("PGN_ECO_TEST_SCID_SHA256");
    if (test_value && *test_value) return test_value;
#endif
    return SCID_SHA256;
}

bool scid_inventory_complete(const BookStats& stats)
{
#ifdef PGN_ECO_TESTING
    return stats.rows > 0 && stats.source_records == stats.rows &&
        stats.invalid_rows == 0;
#else
    unsigned long eco3 = 0, eco4 = 0;
    for (std::map<std::string, unsigned long>::const_iterator code =
             stats.code_counts.begin(); code != stats.code_counts.end(); ++code) {
        if (code->first.size() == 3) eco3 += code->second;
        else if (code->first.size() == 4) eco4 += code->second;
    }
    return stats.source_records == 10360UL && stats.rows == 10360UL &&
        stats.invalid_rows == 0 && stats.ignored_scid_depth == 0 &&
        eco3 == 2537UL && eco4 == 7823UL &&
        stats.code_counts.size() == 5860U && stats.max_plies == 36;
#endif
}

bool update_scid_book(const std::string& directory, std::string& error)
{
    std::string stage_template = directory + "/.scid-download.XXXXXX";
    std::vector<char> stage_name(stage_template.begin(), stage_template.end());
    stage_name.push_back('\0');
    char* created = ::mkdtemp(&stage_name[0]);
    if (!created) {
        error = "cannot create a Scid staging directory: " +
            std::string(std::strerror(errno));
        return false;
    }
    const std::string stage(created);
    const std::string downloaded = stage + "/scid.eco";
    const std::string url = std::string("https://raw.githubusercontent.com/benini/scid/") +
        SCID_REVISION + "/scid.eco";
    bool okay = run_curl(url, downloaded);
    if (!okay) error = "HTTPS download failed for Scid ECO4 book " + url;
    struct stat status;
    if (okay && (::stat(downloaded.c_str(), &status) != 0 ||
                 !S_ISREG(status.st_mode) || status.st_size < 64)) {
        error = "downloaded Scid ECO4 book is missing or has an implausible size";
        okay = false;
    }
#ifndef PGN_ECO_TESTING
    if (okay && static_cast<unsigned long>(status.st_size) != SCID_BYTES) {
        error = "downloaded Scid ECO4 book has an unexpected size";
        okay = false;
    }
#endif
    if (okay) {
        std::string digest;
        if (!file_sha256(downloaded, digest)) {
            error = "sha256sum is required to verify the Scid ECO4 book";
            okay = false;
        } else if (digest != expected_scid_checksum()) {
            error = "downloaded Scid ECO4 book SHA-256 does not match the pinned source";
            okay = false;
        }
    }
    BookStats stats;
    OpeningBook validation;
    if (okay && (!load_book(downloaded, validation, stats, error) ||
                 !scid_inventory_complete(stats))) {
        if (error.empty()) error = "downloaded Scid ECO4 book failed inventory validation";
        okay = false;
    }
    const std::string target = directory + "/scid.eco";
    if (okay && ::rename(downloaded.c_str(), target.c_str()) != 0) {
        error = "cannot atomically activate Scid ECO4 book: " +
            std::string(std::strerror(errno));
        okay = false;
    }
    ::unlink(downloaded.c_str());
    ::rmdir(stage.c_str());
    if (!okay && error.empty()) error = "Scid ECO4 book update failed";
    return okay;
}

bool update_books(std::string& error, std::string& report)
{
    const std::string directory = xdg_data_dir();
    if (directory.empty()) {
        error = "set XDG_DATA_HOME or HOME to install the ECO books";
        return false;
    }
    if (!make_directories(directory)) {
        error = "cannot create data directory " + directory;
        return false;
    }
    if (!update_lichess_book(directory, error)) {
        error = "Lichess ECO3 book update failed: " + error;
        return false;
    }
    report = "installed Lichess ECO3 book at revision " +
        std::string(LICHESS_REVISION) + "\n";
    if (!update_scid_book(directory, error)) {
        error = "Lichess ECO3 was installed, but the Scid ECO4 update failed: " + error;
        return false;
    }
    report += "installed Scid ECO4 book at revision " +
        std::string(SCID_REVISION) + " (SHA-256 verified)\n";
    return true;
}

void usage(std::ostream& out)
{
    out << "Usage:\n"
        << "  pgn-eco [OPTIONS] --check INPUT...\n"
        << "  pgn-eco [OPTIONS] --output FILE INPUT...\n"
        << "  pgn-eco [OPTIONS] --output-dir DIR INPUT...\n"
        << "  pgn-eco [OPTIONS] --in-place INPUT...\n"
        << "  pgn-eco --update-books\n"
        << "  pgn-eco --help\n\n"
        << "Operating modes (choose exactly one): --check, --output FILE,\n"
        << "--output-dir DIR, or --in-place. --fill-missing modifies output\n"
        << "classification; without it, --output modes update existing ECO tags.\n"
        << "Input files are read in command-line order; shell wildcards are expanded\n"
        << "by the shell. --output combines inputs, --output-dir writes one file per\n"
        << "input basename, and --in-place atomically replaces each input separately.\n"
        << "Use --output - for PGN on standard output; reports go to standard error,\n"
        << "and output already written cannot be rolled back after an error.\n"
        << "New output files preserve ordinary input permission bits; combined output\n"
        << "uses the intersection. Existing outputs are never overwritten.\n"
        << "--output-dir must already exist; basename collisions are rejected.\n"
        << "Output files must be new. --limit is only valid with --check and applies\n"
        << "globally across all input files. Use -- before filenames starting with '-'.\n\n"
        << "ECO is three characters (A00-E99) by default; --mode 3 selects this\n"
        << "explicitly. --mode 4 enables a Scid-style letter extension.\n"
        << "The deepest match wins across code lengths; a deeper 3-char match\n"
        << "is reported as a fallback. At equal depth, exact history and\n"
        << "historical distance are compared; unresolved cross-prefix ties\n"
        << "stay ambiguous, while a unique same-prefix 4-char code may refine\n"
        << "the 3-char code.\n"
        << "Matching uses legal positions, exact histories and move-history distances.\n"
        << "Complete reversible state cycles may be removed; ambiguous ties are preserved.\n"
        << "Existing ECO3 and ECO4 tags are syntactically valid in either mode.\n"
        << "--check reports exact, prefix-compatible, different, invalid, duplicate,\n"
        << "and missing tags separately; prefix-compatible is not an exact match.\n"
        << "Output modes recalculate tags and replace invalid or duplicate tags when\n"
        << "a unique result exists.\n"
        << "--fill-missing replaces one unsupported tag, but preserves valid tags\n"
        << "and duplicate tag groups. Slash values are never shortened.\n"
        << "Only main-line SAN through the book horizon (max 40 plies) is validated;\n"
        << "RAV SAN and later moves are skipped.\n"
        << "--details, --per-game and --debug select exclusive --check reports.\n"
        << "--limit N processes at most N complete games across all inputs; check only.\n"
        << "--threads N sets the maximum worker count. Omitted or 0 selects automatic\n"
        << "mode based on input sizes and file count, limited by available logical CPUs.\n"
        << "Explicit values override this choice; 1 is sequential, 2+ enables workers.\n"
        << "Workers start lazily when the first classification task is ready.\n"
        << "--eco-book accepts TSV, native pgn-extract eco.pgn, Scid .eco, and\n"
        << "the escaped Scid export used by Caissabase.\n"
        << "If --eco-book FILE is given, only that book is used. Without it, a\n"
        << "non-empty PGN_ECO_BOOK selects the default; otherwise mode 3 searches\n"
        << "for lichess-openings.tsv and mode 4 searches for Scid scid.eco.\n"
        << "PGN_ECO_TIMING=1 reports detailed phase timings on standard error.\n"
        << "--update-books installs pinned Lichess ECO3 (CC0) and Scid ECO4 books\n"
        << "for offline use. Scid source: commit " << SCID_REVISION << ".\n"
        << "Default mode and --mode 3 use Lichess; --mode 4 uses Scid. Each update is checked\n"
        << "before activation; a failure can leave the first book updated.\n"
        << "Exit status: 0 success/no findings, 1 differences or unresolved --check\n"
        << "findings, 2 processing or I/O error. An error takes precedence over status 1.\n"
        << "A multi-file output-dir or in-place run can leave earlier completed files\n"
        << "in place if a later input fails.\n";
}

enum Mode { MODE_NONE, MODE_CHECK, MODE_OUTPUT, MODE_OUTPUT_DIR, MODE_IN_PLACE, MODE_UPDATE_BOOKS };

double monotonic_ms()
{
    struct timespec value;
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0.0;
    return static_cast<double>(value.tv_sec) * 1000.0 +
        static_cast<double>(value.tv_nsec) / 1000000.0;
}

} // namespace

int main(int argc, char** argv)
{
    Mode mode = MODE_NONE;
    std::string book_path;
    bool book_selected = false;
    bool mode4 = false;
    bool fill_missing = false;
    ReportMode report_mode = REPORT_BRIEF;
    bool report_selected = false;
    bool limit_selected = false;
    unsigned long limit = 0;
    bool threads_selected = false;
    unsigned long thread_count = 0;
    bool end_options = false;
    std::string output_argument;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (!end_options && arg == "--") { end_options = true; continue; }
        if (!end_options && (arg == "--help" || arg == "-h")) { usage(std::cout); return 0; }
        if (end_options) { positional.push_back(arg); continue; }
        if (arg == "--check" || arg == "--output" || arg == "--output-dir" || arg == "--in-place") {
            if (mode != MODE_NONE) { std::cerr << "pgn-eco: operating modes are mutually exclusive\n"; return 2; }
            if (arg == "--output" || arg == "--output-dir") {
                if (++i >= argc) {
                    std::cerr << "pgn-eco: " << arg << " requires a path\n";
                    return 2;
                }
                output_argument = argv[i];
                mode = arg == "--output" ? MODE_OUTPUT : MODE_OUTPUT_DIR;
            } else mode = arg == "--check" ? MODE_CHECK : MODE_IN_PLACE;
        } else if (arg == "--fill-missing") {
            if (fill_missing) { std::cerr << "pgn-eco: --fill-missing may be specified only once\n"; return 2; }
            fill_missing = true;
        } else if (arg == "--update-books") {
            if (mode != MODE_NONE) { std::cerr << "pgn-eco: --update-books cannot be combined with another mode\n"; return 2; }
            mode = MODE_UPDATE_BOOKS;
        } else if (arg == "--mode") {
            if (++i >= argc) {
                std::cerr << "pgn-eco: --mode requires 3 or 4\n"; return 2;
            }
            const std::string mode_value(argv[i]);
            if (mode_value == "3") mode4 = false;
            else if (mode_value == "4") mode4 = true;
            else {
                std::cerr << "pgn-eco: --mode must be 3 or 4\n"; return 2;
            }
        } else if (arg == "--eco-book") {
            if (++i >= argc) { std::cerr << "pgn-eco: --eco-book requires a file\n"; return 2; }
            book_path = argv[i];
            book_selected = true;
        } else if (arg == "--details" || arg == "--per-game" || arg == "--debug") {
            if (report_selected) {
                std::cerr << "pgn-eco: --details, --per-game and --debug are mutually exclusive\n"; return 2;
            }
            report_selected = true;
            report_mode = arg == "--details" ? REPORT_DETAILS :
                (arg == "--per-game" ? REPORT_PER_GAME : REPORT_DEBUG);
        } else if (arg == "--limit") {
            if (limit_selected || ++i >= argc || !parse_limit(argv[i], limit)) {
                std::cerr << "pgn-eco: --limit requires a positive decimal integer\n"; return 2;
            }
            limit_selected = true;
        } else if (arg == "--threads") {
            if (threads_selected || ++i >= argc || !parse_threads(argv[i], thread_count)) {
                std::cerr << "pgn-eco: --threads requires a non-negative decimal integer\n"; return 2;
            }
            threads_selected = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "pgn-eco: unknown option: " << arg << '\n'; return 2;
        } else positional.push_back(arg);
    }
    if (mode == MODE_UPDATE_BOOKS) {
        if (!positional.empty() || book_selected || mode4 || report_selected || limit_selected ||
            threads_selected || fill_missing || !output_argument.empty()) {
            std::cerr << "pgn-eco: --update-books takes no other options or files\n"; return 2;
        }
        std::string error, report;
        if (!update_books(error, report)) { std::cerr << "pgn-eco: " << error << '\n'; return 2; }
        std::cout << report;
        return 0;
    }
    if (mode == MODE_NONE) {
        std::cerr << "pgn-eco: choose exactly one operating mode\n";
        usage(std::cerr); return 2;
    }
    if (positional.empty()) {
        std::cerr << "pgn-eco: provide at least one input file\n";
        usage(std::cerr); return 2;
    }
    if (mode == MODE_CHECK && fill_missing) {
        std::cerr << "pgn-eco: --fill-missing requires an output mode\n"; return 2;
    }
    if (mode != MODE_CHECK && report_selected) {
        std::cerr << "pgn-eco: report options apply only to --check\n"; return 2;
    }
    if (limit_selected && mode != MODE_CHECK) {
        std::cerr << "pgn-eco: --limit is only valid with --check\n"; return 2;
    }
    if (mode == MODE_CHECK && !output_argument.empty()) {
        std::cerr << "pgn-eco: --check cannot be combined with an output mode\n"; return 2;
    }
    const bool output_to_stdout = mode == MODE_OUTPUT && output_argument == "-";
    std::ostream& report = output_to_stdout ? std::cerr : std::cout;
    const char* audit_environment = std::getenv("PGN_ECO_AUDIT_JSON");
    const bool audit_details = report_mode == REPORT_DEBUG && audit_environment &&
        std::string(audit_environment) == "1";
    if (book_selected && book_path.empty()) {
        std::cerr << "pgn-eco: --eco-book requires a non-empty file path\n"; return 2;
    }
    BookMetadata active_book;
    if (!book_selected) book_path = find_default_book(active_book, mode4);
    if (book_path.empty()) {
        if (mode4)
            std::cerr << "pgn-eco: no local Scid ECO4 book found; run --update-books or pass --eco-book FILE\n";
        else
            std::cerr << "pgn-eco: no local Lichess ECO3 book found; run --update-books or pass --eco-book FILE\n";
        return 2;
    }
    if (!active_book.available) {
        if (::stat(book_path.c_str(), &active_book.status) == 0)
            active_book.available = true;
    }
    if (active_book.canonical.empty())
        canonical_path(book_path, active_book.canonical);

    std::vector<InputFileInfo> input_files;
    std::set<std::string> canonical_inputs;
    std::string error;
    if (!validate_input_files(positional, mode == MODE_IN_PLACE, active_book,
            input_files, canonical_inputs, error)) {
        std::cerr << "pgn-eco: " << error << '\n'; return 2;
    }
    if (!threads_selected || thread_count == 0) {
        bool large_enough = false;
        unsigned long aggregate_size = 0;
        for (std::size_t i = 0; i < input_files.size() && !large_enough; ++i) {
            if (input_files[i].status.st_size < 0) continue;
            if (input_files[i].status.st_size > 8192) {
                large_enough = true;
            } else {
                const unsigned long size =
                    static_cast<unsigned long>(input_files[i].status.st_size);
                if (size > 8192UL - aggregate_size) large_enough = true;
                else aggregate_size += size;
            }
        }
        thread_count = (large_enough || positional.size() >= 4) ?
            online_cpu_count() : 1UL;
        if (thread_count == 0) thread_count = 1;
    }
    std::vector<std::string> output_paths;
    if (mode == MODE_OUTPUT) {
        if (!output_to_stdout) {
            std::string output_parent;
            if (!canonical_path(path_directory(output_argument), output_parent)) {
                std::cerr << "pgn-eco: cannot resolve output directory for "
                          << output_argument << ": " << std::strerror(errno) << '\n';
                return 2;
            }
            if (!validate_new_destination(output_argument, output_parent,
                    canonical_inputs, active_book.canonical, error) ||
                !probe_output_directory(output_parent, true, error)) {
                std::cerr << "pgn-eco: " << error << '\n'; return 2;
            }
        }
        output_paths.push_back(output_argument);
    } else if (mode == MODE_OUTPUT_DIR) {
        struct stat directory_status;
        std::string directory;
        if (::stat(output_argument.c_str(), &directory_status) != 0 ||
            !S_ISDIR(directory_status.st_mode) || !canonical_path(output_argument, directory)) {
            std::cerr << "pgn-eco: output directory must already exist and be writable: "
                      << output_argument << '\n'; return 2;
        }
        if (!probe_output_directory(directory, true, error)) {
            std::cerr << "pgn-eco: " << error << '\n'; return 2;
        }
        std::set<std::string> basenames;
        for (std::size_t i = 0; i < input_files.size(); ++i) {
            const std::string base = path_basename(input_files[i].path);
            if (base.empty() || base == "." || base == ".." || !basenames.insert(base).second) {
                std::cerr << "pgn-eco: output basename collision for input " << input_files[i].path << '\n';
                return 2;
            }
            const std::string destination = directory + "/" + base;
            if (!validate_new_destination(destination, directory,
                    canonical_inputs, active_book.canonical, error)) {
                std::cerr << "pgn-eco: " << error << '\n'; return 2;
            }
            output_paths.push_back(destination);
        }
    }
    OpeningBook book;
    BookStats book_stats;
    const double book_start = monotonic_ms();
    if (!load_book(book_path, book, book_stats, error)) {
        std::cerr << "pgn-eco: " << error << '\n'; return 2;
    }
    ReferenceHistoryCache reference_histories;
    prepare_reference_histories(book, reference_histories);
    const double book_load_ms = monotonic_ms() - book_start;
    for (std::size_t i = 0; i < book_stats.rejected_details.size() && i < 10; ++i)
        std::cerr << "pgn-eco: warning: " << book_stats.rejected_details[i] << '\n';
    if (book_stats.rejected_details.size() > 10)
        std::cerr << "pgn-eco: warning: " << (book_stats.rejected_details.size() - 10)
                  << " additional invalid ECO book records skipped\n";
    for (std::size_t i = 0; i < book_stats.conflict_details.size() && i < 10; ++i)
        std::cerr << "pgn-eco: book conflict: " << book_stats.conflict_details[i] << '\n';
    if (book_stats.conflicts > book_stats.conflict_details.size())
        std::cerr << "pgn-eco: " << (book_stats.conflicts - book_stats.conflict_details.size())
                  << " additional conflicting book positions\n";
    std::string temp_path;
    FILE* output = 0;
    mode_t combined_output_permissions = 0777;
    if (mode == MODE_OUTPUT) {
        for (std::size_t i = 0; i < input_files.size(); ++i)
            combined_output_permissions &= input_files[i].status.st_mode & 0777;
        if (output_to_stdout) {
            ::signal(SIGPIPE, SIG_IGN);
            output = stdout;
        } else if (!create_temp_output(output_paths[0], 0, temp_path, output, error)) {
            std::cerr << "pgn-eco: " << error << '\n'; return 2;
        }
    }

    unsigned long games = 0, correct = 0, changed = 0, missing = 0,
        incorrect = 0, invalid = 0, unclassifiable = 0, ambiguous = 0, skipped = 0,
        fallbacks = 0,
        book_conflict_ambiguous = 0, prefix_compatible = 0, invalid_openings = 0,
        invalid_eco_values_found = 0, duplicate_eco_groups_found = 0,
        invalid_eco_values_repaired = 0, invalid_eco_values_left = 0,
        invalid_eco_values_no_unique = 0, invalid_eco_values_duplicate_protected = 0,
        duplicate_eco_groups_repaired = 0, duplicate_eco_groups_left = 0,
        duplicate_groups_no_unique = 0, duplicate_groups_fill_protected = 0;
    std::map<std::string, unsigned long> check_outcomes;
    unsigned long validated_mainline_plies = 0, skipped_rav_san_tokens = 0,
        skipped_late_mainline_san_tokens = 0;
    std::map<std::string, CodeStats> code_stats;
    std::map<std::string, unsigned long> shifts;
    double read_split_ms = 0.0, parse_ms = 0.0, tokenize_ms = 0.0,
        semantic_ms = 0.0, san_ms = 0.0, header_ms = 0.0,
        classify_ms = 0.0, fen_ms = 0.0, key_ms = 0.0,
        lookup_ms = 0.0, history_ms = 0.0, report_ms = 0.0;
    const char* timing_setting = std::getenv("PGN_ECO_TIMING");
    const bool detailed_timing = timing_setting && std::string(timing_setting) == "1";
    const int opening_horizon = book_stats.max_match_plies < MAX_CLASSIFY_PLIES ?
        (book_stats.max_match_plies < 0 ? 0 : book_stats.max_match_plies) :
        MAX_CLASSIFY_PLIES;
    std::string raw;
    bool fatal = false;
    ClassifierPool pool(book, reference_histories, book_stats.max_match_plies,
                        mode4, report_mode == REPORT_DEBUG, audit_details,
                        detailed_timing);
    const std::size_t batch_capacity = thread_count >= MAX_BATCH_GAMES / 2 ?
        MAX_BATCH_GAMES : static_cast<std::size_t>(thread_count * 2UL);
    std::vector<WorkItem*> batch;
            try { batch.reserve(batch_capacity); }
    catch (const std::exception& exception) {
        std::cerr << "pgn-eco: cannot allocate bounded work batch: "
                  << exception.what() << '\n';
        if (output && !output_to_stdout) {
            std::fclose(output); output = 0; ::unlink(temp_path.c_str());
        }
        return 2;
    }
    std::size_t batch_bytes = 0;
    bool pool_started = false;
    bool combined_has_content = false;
    std::size_t combined_trailing_breaks = 0;
    bool combined_crlf = false;
    for (std::size_t file_index = 0; file_index < input_files.size() && !fatal; ++file_index) {
        if (mode == MODE_CHECK && limit && games >= limit) break;
        const std::string input_path = input_files[file_index].path;
        const std::string current_output_path = mode == MODE_OUTPUT ? output_paths[0] :
            (mode == MODE_OUTPUT_DIR ? output_paths[file_index] :
             (mode == MODE_IN_PLACE ? input_path : std::string()));
        const bool per_file_output = mode == MODE_OUTPUT_DIR || mode == MODE_IN_PLACE;
        if (per_file_output && !create_temp_output(current_output_path,
                mode == MODE_IN_PLACE ? &input_files[file_index].status : 0,
                temp_path, output, error)) {
            std::cerr << "pgn-eco: " << error << '\n';
            fatal = true;
            break;
        }
        int input_flags = O_RDONLY | O_NONBLOCK;
#ifdef O_NOFOLLOW
        if (mode == MODE_IN_PLACE) input_flags |= O_NOFOLLOW;
#endif
        const int input_fd = ::open(input_path.c_str(), input_flags);
        if (input_fd < 0) {
            std::cerr << "pgn-eco: cannot open input " << input_path << ": "
                      << std::strerror(errno) << '\n';
            fatal = true;
            if (per_file_output) { std::fclose(output); output = 0; ::unlink(temp_path.c_str()); }
            break;
        }
        struct stat opened_status;
        if (::fstat(input_fd, &opened_status) != 0 ||
            !S_ISREG(opened_status.st_mode) ||
            !unchanged_input(input_files[file_index].status, opened_status)) {
            std::cerr << "pgn-eco: input changed after validation: " << input_path << '\n';
            fatal = true;
            ::close(input_fd);
            if (per_file_output) { std::fclose(output); output = 0; ::unlink(temp_path.c_str()); }
            break;
        }
        DescriptorInputBuffer input_buffer(input_fd);
        std::istream input(&input_buffer);
        GameReader reader(input);
        unsigned long file_games = 0;
        bool file_contributed = false;
        bool input_done = false;
        std::string read_error;
        while ((!input_done || !batch.empty()) && !fatal) {
        while (!input_done && batch.size() < batch_capacity &&
               batch_bytes < MAX_BATCH_BYTES && (!limit || games < limit)) {
            error.clear();
            const double read_start = detailed_timing ? monotonic_ms() : 0.0;
            const bool got_game = reader.next(raw, error);
            if (detailed_timing) read_split_ms += monotonic_ms() - read_start;
            if (input_buffer.failed()) {
                input_done = true;
                read_error = "input read error";
                break;
            }
            if (!got_game) {
                input_done = true;
                read_error = error;
                break;
            }
            if (trim(raw).empty()) continue;
            ++games;
            ++file_games;
            WorkItem* item = 0;
            try {
                item = new WorkItem();
                item->game_number = games;
                item->file_game_number = file_games;
                item->input_path = input_path;
                item->raw.swap(raw);
                const double header_start = detailed_timing ? monotonic_ms() : 0.0;
                item->headers = scan_header(item->raw);
                const TagSpan* old_tag = 0;
                item->eco_status = inspect_eco(item->headers, &old_tag);
                if (old_tag) {
                    item->old_tag_index = static_cast<std::size_t>(
                        old_tag - &item->headers.tags[0]);
                    item->has_old_tag = true;
                }
                item->source_id = source_game_id(item->headers, file_games);
                if (detailed_timing) header_ms += monotonic_ms() - header_start;
                /* Valid codes and duplicate tag groups are protected in fill
                   mode. A single unsupported value may be repaired. */
                item->preserve_existing = fill_missing &&
                    (item->eco_status == ECO_SINGLE || item->eco_status == ECO_DUPLICATE);
            } catch (const std::exception& exception) {
                delete item;
                std::cerr << "pgn-eco: " << input_path << ": cannot prepare game " << file_games << ": "
                          << exception.what() << '\n';
                fatal = true;
                break;
            } catch (...) {
                delete item;
                std::cerr << "pgn-eco: " << input_path << ": cannot prepare game " << file_games << '\n';
                fatal = true;
                break;
            }
            const std::size_t item_bytes = item->raw.size();
            if (!pool_started && thread_count > 1 && !item->preserve_existing) {
                if (!pool.start(thread_count, error)) {
                    std::cerr << "pgn-eco: " << input_path << ": " << error << '\n';
                    delete item;
                    fatal = true;
                    break;
                }
                pool_started = true;
            }
            if (pool_started) {
                if (!pool.submit(item, error)) {
                    std::cerr << "pgn-eco: " << input_path << ": " << error << '\n';
                    delete item;
                    fatal = true;
                    break;
                }
            } else {
                pool.process_inline(item);
            }
            batch.push_back(item);
            batch_bytes += item_bytes;
        }

        for (std::size_t batch_index = 0; batch_index < batch.size(); ++batch_index) {
            WorkItem* item = batch[batch_index];
            if (!pool.wait(item, error)) {
                std::cerr << "pgn-eco: " << item->input_path << ": " << error << '\n';
                fatal = true;
                break;
            }
            const std::string& raw = item->raw;
            const GameTags& headers = item->headers;
            const TagSpan* old_tag = item->has_old_tag ?
                &headers.tags[item->old_tag_index] : 0;
            const EcoStatus eco_status = item->eco_status;
            const std::string& source_id = item->source_id;
            Classification& classification = item->classification;
            if (!item->preserve_existing) {
                parse_ms += item->parse_ms;
                if (detailed_timing) {
                    tokenize_ms += item->parsed.timing.tokenize_ms;
                    semantic_ms += item->parsed.timing.semantic_ms;
                    san_ms += item->parsed.timing.san_ms;
                    validated_mainline_plies += item->parsed.timing.validated_mainline_plies;
                    skipped_rav_san_tokens += item->parsed.timing.skipped_rav_san_tokens;
                    skipped_late_mainline_san_tokens +=
                        item->parsed.timing.skipped_late_mainline_san_tokens;
                }
            }
            if (classification.state == Classification::AMBIGUOUS &&
                classification.book_conflict) ++book_conflict_ambiguous;
            classify_ms += item->classify_ms;
            if (detailed_timing) {
                fen_ms += item->classification_timing.fen_ms;
                key_ms += item->classification_timing.key_ms;
                lookup_ms += item->classification_timing.lookup_ms;
                history_ms += item->classification_timing.history_ms;
            }
            const unsigned long file_game_number = item->file_game_number;
            const double report_start = detailed_timing ? monotonic_ms() : 0.0;
            if (classification.fallback && classification.state == Classification::FOUND)
                ++fallbacks;

            bool should_write = false;
            std::string desired;
            std::string status;
            const bool classification_found = classification.state == Classification::FOUND;
            const bool classification_ambiguous = classification.state == Classification::AMBIGUOUS;
            const bool classification_invalid = classification.state == Classification::INVALID;
            const bool classification_none = classification.state == Classification::NONE;
            unsigned int invalid_values_this_game = 0;
            for (std::size_t i = 0; i < headers.tags.size(); ++i) {
                if (headers.tags[i].name != "ECO") continue;
                if (!valid_eco_any(headers.tags[i].value)) ++invalid_values_this_game;
            }
            invalid_eco_values_found += invalid_values_this_game;
            if (eco_status == ECO_DUPLICATE) ++duplicate_eco_groups_found;
            if (eco_status == ECO_INVALID || eco_status == ECO_DUPLICATE) ++invalid;
            if (classification_invalid) ++invalid_openings;
            else if (classification_ambiguous) ++ambiguous;
            else if (classification_none && !item->preserve_existing) ++unclassifiable;

            std::string classification_status = classification_found ? "classified" :
                (classification_invalid ? "invalid PGN" :
                 (classification_ambiguous ? "ambiguous" : "unclassifiable"));
            std::string tag_status = eco_status == ECO_ABSENT ? "missing" :
                (eco_status == ECO_DUPLICATE ? "duplicate" :
                 (eco_status == ECO_INVALID ? "invalid-notation" :
                  (valid_eco(old_tag->value) ? "valid-ECO3" : "valid-ECO4")));

            if (mode == MODE_CHECK) {
                if (eco_status == ECO_ABSENT) {
                    ++missing;
                    if (classification_found) {
                        status = "missing";
                        CodeStats& stats = code_stats[classification.eco];
                        ++stats.total; ++stats.missing;
                    } else if (classification_ambiguous) {
                        status = "missing / ambiguous";
                    } else if (classification_invalid) {
                        status = "missing / invalid PGN";
                    } else {
                        status = "missing / unclassifiable";
                    }
                } else if (eco_status == ECO_INVALID || eco_status == ECO_DUPLICATE) {
                    status = eco_status == ECO_INVALID ? "invalid notation" : "duplicate tags";
                    if (classification_ambiguous) status += " / ambiguous";
                    else if (classification_invalid) status += " / invalid PGN";
                    else if (classification_none) status += " / unclassifiable";
                    if (classification_found) {
                        CodeStats& stats = code_stats[classification.eco];
                        ++stats.total; ++stats.invalid;
                    }
                } else if (classification_found) {
                    CodeStats& stats = code_stats[classification.eco];
                    ++stats.total;
                    if (old_tag->value == classification.eco) {
                        status = "exact"; ++correct; ++stats.equal;
                    } else if (prefix_compatible_eco(old_tag->value, classification.eco)) {
                        status = "prefix-compatible"; ++prefix_compatible; ++stats.prefix_compatible;
                        ++shifts[old_tag->value + " -> " + classification.eco];
                    } else {
                        status = "different"; ++incorrect; ++stats.different;
                        ++shifts[old_tag->value + " -> " + classification.eco];
                    }
                } else {
                    status = classification_status;
                    CodeStats& stats = code_stats[old_tag->value];
                    ++stats.total;
                    if (classification_ambiguous) ++stats.ambiguous;
                    else if (classification_invalid) ++stats.invalid_opening;
                    else ++stats.unclassifiable;
                }
                ++check_outcomes[status];
            } else {
                if (eco_status == ECO_ABSENT) ++missing;
                if (fill_missing && item->preserve_existing) ++skipped;
                if (classification_found) {
                    desired = classification.eco;
                    if (!fill_missing) {
                        should_write = eco_status != ECO_SINGLE || old_tag->value != desired;
                    } else if (fill_missing) {
                        should_write = eco_status == ECO_ABSENT || eco_status == ECO_INVALID;
                    }
                    if (eco_status == ECO_SINGLE && old_tag->value == desired) ++correct;
                    else if (eco_status == ECO_SINGLE) ++incorrect;
                }
                if (should_write) {
                    invalid_eco_values_repaired += invalid_values_this_game;
                    if (eco_status == ECO_DUPLICATE) ++duplicate_eco_groups_repaired;
                } else {
                    invalid_eco_values_left += invalid_values_this_game;
                    if (eco_status == ECO_INVALID) {
                        invalid_eco_values_no_unique += invalid_values_this_game;
                    } else if (eco_status == ECO_DUPLICATE) {
                        ++duplicate_eco_groups_left;
                        if (fill_missing) {
                            ++duplicate_groups_fill_protected;
                            invalid_eco_values_duplicate_protected += invalid_values_this_game;
                        } else {
                            ++duplicate_groups_no_unique;
                            invalid_eco_values_no_unique += invalid_values_this_game;
                        }
                    }
                }
            }
            if (mode == MODE_CHECK) {
                if (report_mode == REPORT_PER_GAME) {
                    if (input_files.size() > 1) std::cout << "file=" << input_path << " | ";
                    std::cout << "game " << file_game_number << " | existing="
                              << (eco_status == ECO_SINGLE ? old_tag->value : "-")
                              << " | calculated=" << (classification.state == Classification::FOUND ? classification.eco : "-")
                              << " | status=" << status << " | source-id=" << source_id
                              << " | fallback=" << (classification.fallback ? "yes" : "no")
                              << " | tag_status=" << tag_status
                              << " | classification=" << classification_status
                              << " | existing_tags=" << diagnostic_tag_values(raw, headers);
                    if (classification.state == Classification::AMBIGUOUS)
                        std::cout << " | ambiguity=" <<
                            (classification.book_conflict ? "book-data-contradiction" : "history-tie");
                    std::cout << '\n';
                } else if (report_mode == REPORT_DEBUG) {
                    if (input_files.size() > 1) std::cout << "file=" << input_path << " | ";
                    std::cout << "game " << file_game_number << " | existing="
                              << (eco_status == ECO_SINGLE ? old_tag->value : "-")
                              << " | calculated=" << (classification.state == Classification::FOUND ? classification.eco : "-")
                              << " | status=" << status << " | source-id=" << source_id
                              << " | tag_status=" << tag_status
                              << " | classification=" << classification_status
                              << " | existing_tags=" << diagnostic_tag_values(raw, headers) << '\n'
                              << "  reference_ply=" << classification.ply
                              << " reference_position=\"" << (classification.position_key.empty() ? "-" : classification.position_key)
                              << "\" FEN=\"" << (classification.fen.empty() ? "-" : classification.fen)
                              << "\" examined_halfmoves=" << classification.examined_plies
                              << " examined_FEN=\"" << (classification.examined_fen.empty() ? "-" : classification.examined_fen)
                              << "\" candidates=" << code_list(classification.alternatives)
                              << " exact_candidates=" << code_list(classification.exact_alternatives)
                              << " book_conflict=" << (classification.book_conflict ? "yes" : "no")
                              << " ambiguity_source="
                              << (classification.state != Classification::AMBIGUOUS ? "none" :
                                  (classification.book_conflict ? "book-data" : "history-tie"))
                              << " mode4_priority=\""
                              << (classification.mode4_priority.empty() ? "-" : classification.mode4_priority) << '\"'
                              << " history=\"" << (classification.history_evidence.empty() ? "-" : classification.history_evidence) << '\"'
                              << " fallback=" << (classification.fallback ? "yes" : "no");
                    if (!classification.detail.empty()) std::cout << " detail=\"" << classification.detail << '\"';
                    std::cout << '\n';
                    if (audit_details && !classification.audit_json.empty())
                        std::cout << "AUDIT_JSON\t" << classification.audit_json << '\n';
                }
            }

            if (mode != MODE_CHECK) {
                std::string bytes = raw;
                if (should_write) {
                    if (!patch_game(raw, headers, eco_status, desired, bytes)) {
                        std::cerr << "pgn-eco: " << input_path
                                  << ": internal ECO patch error in game " << file_game_number << '\n';
                        fatal = true;
                        break;
                    }
                    ++changed;
                }
                if (mode == MODE_OUTPUT) {
                    /* Keep the BOM only on the first actual contribution. Empty
                       files and whitespace-only records do not consume that slot. */
                    if (combined_has_content && starts_with_utf8_bom(bytes))
                        bytes.erase(0, 3);
                    if (!bytes.empty() && !file_contributed && combined_has_content &&
                        combined_trailing_breaks == 0 && leading_line_breaks(bytes) == 0) {
                        const char* separator = (combined_crlf || contains_crlf(bytes)) ?
                            "\r\n" : "\n";
                        if (!write_all(output, separator)) {
                            std::cerr << "pgn-eco: write failed for "
                                      << (output_to_stdout ? "standard output" : current_output_path) << '\n';
                            fatal = true;
                            break;
                        }
                        combined_trailing_breaks = 1;
                    }
                }
                if (!bytes.empty() && !write_all(output, bytes)) {
                    std::cerr << "pgn-eco: write failed for "
                              << (output_to_stdout ? "standard output" : current_output_path) << '\n';
                    fatal = true;
                    break;
                }
                if (mode == MODE_OUTPUT && !bytes.empty()) {
                    combined_has_content = true;
                    file_contributed = true;
                    combined_trailing_breaks = trailing_line_breaks(bytes);
                    const std::string::size_type last_break = bytes.find_last_of("\r\n");
                    if (last_break != std::string::npos)
                        combined_crlf = bytes[last_break] == '\n' && last_break > 0 &&
                            bytes[last_break - 1] == '\r';
                }
            }
            if (detailed_timing) report_ms += monotonic_ms() - report_start;
            delete item;
            batch[batch_index] = 0;
        }
        if (!fatal && !read_error.empty()) {
            std::cerr << "pgn-eco: " << input_path << ": " << read_error << '\n';
            fatal = true;
        }
        if (!fatal) {
            batch.clear();
            batch_bytes = 0;
            if (limit && games >= limit) input_done = true;
        }
        }
        const bool input_close_ok = input_buffer.close();
        if ((input_buffer.failed() || input.bad() || !input_close_ok) && !fatal) {
            std::cerr << "pgn-eco: " << input_path << ": input close/read failure\n";
            fatal = true;
        }
        if (per_file_output) {
            if (!fatal && !finish_temp_output(temp_path, current_output_path, output,
                    mode == MODE_IN_PLACE, mode == MODE_IN_PLACE ?
                        &input_files[file_index].status : 0,
                    input_files[file_index].status.st_mode & 0777, error)) {
                std::cerr << "pgn-eco: " << error << '\n';
                fatal = true;
            }
            if (fatal) {
                if (output) { std::fclose(output); output = 0; }
                if (!temp_path.empty()) ::unlink(temp_path.c_str());
            }
            temp_path.clear();
        }
    }
    pool.stop();
    for (std::size_t i = 0; i < batch.size(); ++i) delete batch[i];
    if (mode == MODE_OUTPUT && output) {
        if (output_to_stdout) {
            if (std::fflush(stdout) != 0) {
                std::cerr << "pgn-eco: write or flush failed for standard output: "
                          << std::strerror(errno) << '\n';
                fatal = true;
            }
            output = 0; /* stdout belongs to the caller; never close it. */
        } else {
            if (!fatal && !finish_temp_output(temp_path, output_paths[0], output,
                    false, 0, combined_output_permissions, error)) {
                std::cerr << "pgn-eco: " << error << '\n';
                fatal = true;
            }
            if (fatal) {
                if (output) { std::fclose(output); output = 0; }
                if (!temp_path.empty()) ::unlink(temp_path.c_str());
            }
        }
    }
    if (fatal) return 2;
    if (mode == MODE_CHECK && report_mode == REPORT_DETAILS) {
        report << "ECO | total | exact | prefix-compatible | different | missing | invalid-tag | ambiguous | unclassifiable | invalid-PGN | exact% | prefix-compatible% | different%\n";
        for (std::map<std::string, CodeStats>::const_iterator i = code_stats.begin(); i != code_stats.end(); ++i) {
            const CodeStats& stats = i->second;
            report << i->first << " | " << stats.total << " | " << stats.equal << " | "
                      << stats.prefix_compatible << " | " << stats.different << " | "
                      << stats.missing << " | " << stats.invalid << " | "
                      << stats.ambiguous << " | " << stats.unclassifiable << " | "
                      << stats.invalid_opening << " | " << percent(stats.equal, stats.total) << " | "
                      << percent(stats.prefix_compatible, stats.total) << " | "
                      << percent(stats.different, stats.total) << '\n';
        }
        std::vector<std::pair<std::string, unsigned long> > ordered_shifts(shifts.begin(), shifts.end());
        std::sort(ordered_shifts.begin(), ordered_shifts.end(), ShiftOrder());
        report << "Most common code shifts:\n";
        for (std::size_t i = 0; i < ordered_shifts.size() && i < 10; ++i)
            report << ordered_shifts[i].first << " | " << ordered_shifts[i].second << '\n';
    }
    if (mode == MODE_CHECK) {
        unsigned long exclusive_total = 0;
        for (std::map<std::string, unsigned long>::const_iterator i = check_outcomes.begin();
             i != check_outcomes.end(); ++i) exclusive_total += i->second;
        report << "Check summary\n"
                  << "games processed: " << games;
        if (limit) report << " (limit " << limit << ')';
        report << '\n' << "exclusive outcomes (sum " << exclusive_total << "): ";
        bool first_outcome = true;
        for (std::map<std::string, unsigned long>::const_iterator i = check_outcomes.begin();
             i != check_outcomes.end(); ++i) {
            if (!first_outcome) report << ", ";
            first_outcome = false;
            report << i->first << ' ' << i->second;
        }
        report << '\n'
                  << "missing tags " << missing << ", unclassifiable " << unclassifiable
                  << ", ambiguous " << ambiguous << ", invalid opening PGNs " << invalid_openings << '\n'
                  << "invalid ECO values " << invalid_eco_values_found
                  << ", duplicate-tag groups " << duplicate_eco_groups_found << '\n'
                  << "book-conflict ambiguities " << book_conflict_ambiguous
                  << ", four-character fallbacks " << fallbacks << '\n'
                  << "changed or added tags: " << changed << ", preserved existing: " << skipped << '\n'
                  << "book references: " << book_stats.rows << " accepted / " << book_stats.source_records
                  << " records; invalid " << book_stats.invalid_rows
                  << ", stale '+' suffixes corrected " << book_stats.corrected_check_suffixes
                  << ", missing Scid capture markers corrected "
                  << book_stats.corrected_missing_capture_markers << '\n'
                  << "book unique positions: " << book_stats.positions << ", exact-line conflicts: " << book_stats.conflicts
                  << ", multi-reference: " << book_stats.multi_reference_positions << '\n'
                  << "book max line depth: " << book_stats.max_plies << " ply; load "
                  << static_cast<unsigned long>(book_load_ms) << " ms; parse "
                  << static_cast<unsigned long>(parse_ms) << " ms; classify "
                  << static_cast<unsigned long>(classify_ms) << " ms\n";
    } else {
        report << "games " << games << ", correct " << correct << ", differing " << incorrect
                  << ", missing " << missing << ", invalid " << invalid << ", ambiguous " << ambiguous
                  << ", not classifiable " << unclassifiable
                  << ", invalid opening PGNs " << invalid_openings << ", changed " << changed
                  << ", four-char fallbacks " << fallbacks;
        if (limit) report << ", limit " << limit;
        report << '\n';
    }
    if (mode != MODE_CHECK) {
        report << "ECO tag repair: invalid values found " << invalid_eco_values_found
                  << ", replaced " << invalid_eco_values_repaired
                  << ", left " << invalid_eco_values_left << " (no unique result "
                  << invalid_eco_values_no_unique << "; protected duplicate groups "
                  << invalid_eco_values_duplicate_protected << ')'
                  << "; duplicate groups found " << duplicate_eco_groups_found
                  << ", collapsed " << duplicate_eco_groups_repaired
                  << ", left " << duplicate_eco_groups_left << " (no unique result "
                  << duplicate_groups_no_unique << "; protected by fill policy "
                  << duplicate_groups_fill_protected << ')' << '\n';
    }
    if (detailed_timing) {
        const double flush_start = monotonic_ms();
        report.flush();
        const double flush_ms = monotonic_ms() - flush_start;
        const double structure_ms = semantic_ms > san_ms ? semantic_ms - san_ms : 0.0;
        const double classify_other_ms = classify_ms > fen_ms + key_ms + lookup_ms + history_ms ?
            classify_ms - fen_ms - key_ms - lookup_ms - history_ms : 0.0;
        std::cerr << "PGN_ECO_TIMING read_split=" << static_cast<unsigned long>(read_split_ms)
                  << "ms tokenize=" << static_cast<unsigned long>(tokenize_ms)
                  << "ms structure_and_bookkeeping=" << static_cast<unsigned long>(structure_ms)
                  << "ms SAN_and_legal_moves=" << static_cast<unsigned long>(san_ms)
                  << "ms FEN_format=" << static_cast<unsigned long>(fen_ms)
                  << "ms position_key=" << static_cast<unsigned long>(key_ms)
                  << "ms book_lookup=" << static_cast<unsigned long>(lookup_ms)
                  << "ms history_comparison=" << static_cast<unsigned long>(history_ms)
                  << "ms classify_other=" << static_cast<unsigned long>(classify_other_ms)
                  << "ms header_and_tag_inspection=" << static_cast<unsigned long>(header_ms)
                  << "ms report_formatting=" << static_cast<unsigned long>(report_ms)
                  << "ms final_output_flush=" << static_cast<unsigned long>(flush_ms) << "ms"
                  << "; worker_pool_started=" << (pool_started ? "yes" : "no")
                  << "; worker_limit=" << thread_count
                  << "; validated_mainline_plies=" << validated_mainline_plies
                  << "; skipped_rav_SAN_tokens=" << skipped_rav_san_tokens
                  << "; skipped_late_mainline_SAN_tokens=" << skipped_late_mainline_san_tokens
                  << "; opening_horizon=" << opening_horizon << " ply\n";
    }
    return mode == MODE_CHECK && (incorrect || prefix_compatible || missing || invalid ||
                                  ambiguous || unclassifiable || invalid_openings) ? 1 : 0;
}

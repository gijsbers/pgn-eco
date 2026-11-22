# Technical notes

This file explains how pgn-eco chooses an opening code. The basic commands
and safety rules are in the [README](../README.md).

## Position matching

The program records the legal main-line positions that it analyzes. A
position key includes:

- The piece on every square.
- Which side moves next.
- Each side's remaining castling rights.
- An en-passant capture, but only when a legal capture is possible.

Move counters are not part of position identity. The six-field FEN
(Forsyth-Edwards Notation) is useful for display; the lookup key is a compact
position identity, not necessarily a full FEN. A difference in castling
rights or a genuinely legal en-passant capture means the positions differ.

The deepest matching position within the effective opening horizon is
preferred. This supports transpositions: two different legal move orders can
reach the same position.

## Comparing histories at a shared position

If references with different codes reach the same candidate position, the
classifier uses their legal move histories:

1. An exact ordered move history is strongest.
2. It represents each move by its source square, destination square, and
   promotion piece. White's and Black's move sequences are compared
   separately.
3. It may remove a complete cycle only when the cycle returns to the same
   full position state, including castling and legal en-passant rights.
4. If the two per-side move multisets are the same, fewer removed plies rank
   first, followed by fewer adjacent same-side swaps. The swap count is a
   Kendall inversion distance. Repeated moves use a stable minimum
   occurrence mapping.
5. If the move multisets differ, a longest common subsequence (LCS) measures
   unmatched move occurrences. A smaller LCS insertion/deletion distance
   ranks first, followed by fewer removed plies and fewer swaps. Unmatched
   moves are evidence for ranking; they are not removed from the game.

These distances compare historical similarity. They do not claim that
reordering moves creates a legal intermediate game. Only complete verified
position cycles are removed. If candidates remain equally supported, the
result is ambiguous rather than dependent on book order.

## Comparison with pgn-extract

The same ECO book does not guarantee the same result in two classifiers.
They can differ in which reference match they keep and how they decide
between codes when a position has more than one opening history. Position
identity and move history answer different questions: two games can have the
same board state and legal rights while reaching it through different moves.

### How pgn-eco ranks matching histories

The classifier first prefers the deepest legal reference position within the
effective opening horizon. If different ECO codes reach that position, it
compares the played history with each reference history:

1. A unique exact ordered move history is the strongest evidence.
2. Before comparing distances, it may remove a complete reversible cycle
   from a history. A cycle is removable only when replaying it returns to the
   same full legal state: the same pieces, side to move, castling rights, and
   legally available en-passant capture. This reduction is only for comparing
   histories; it does not change the game or make arbitrary moves disappear.
3. Moves are compared by their chess move (from-square, to-square, and
   promotion), not by their SAN spelling. White's sequence and Black's
   sequence are compared separately.
4. When the two histories contain the same move occurrences for each side,
   the classifier counts inversions in the stable occurrence matching. This
   is the Kendall inversion distance: the number of pairs whose order differs,
   or equivalently the minimum adjacent same-side swaps needed to match the
   sequences. White and Black distances are added. Repeated identical moves
   are matched in stable order.
5. When the move sets differ, a longest common subsequence (LCS) finds the
   largest order-preserving set of matching moves for each side. The
   insertion/deletion distance is the total number of move occurrences left
   unmatched by those LCS matches. The classifier then compares removed
   plies and the order of the moves that still match.

These are ranking measures, not permission to shuffle a game into a different
legal line. Every played opening move and every reference move was parsed as
a legal move in its actual position. An inversion score does not claim that
the swapped intermediate moves would themselves be legal. A sequence edit
does not remove an arbitrary move from the played game. If the best candidates
still have the same score, the result remains ambiguous.

The position key is deliberately strict. The pieces on all squares, the side
to move, castling rights, and a genuinely legal en-passant capture must match.
A rook that moves and returns does not restore castling rights. A nominal
en-passant target that cannot be captured legally is not a legal en-passant
right. The half-move and full-move counters do not define position identity.

### Urusov transposition example

These two reference lines reach the same position:

```text
C24: 1.e4 e5 2.Bc4 Nf6 3.d4 exd4 4.Nf3
C43: 1.e4 e5 2.Nf3 Nf6 3.d4 exd4 4.Bc4
```

The pieces, player to move, and legal rights are the same, but the move order
is different. When either reference line is played exactly, pgn-eco can use
that exact history to distinguish C24 from C43. A transposed game that follows
neither complete route needs a history comparison. In the million-game audit,
the line `1.e4 e5 2.d4 exd4 3.Nf3 Nf6 4.Bc4` had two same-side move-order
inversions against C43 and three against C24, so the current ranking selected
C43. That is a consistent historical heuristic; it does not prove that C43
is the only objectively correct ECO label for every such game.

### What the million-game audit found

The reproducible audit compared the first 1,000,000 games in Mega using the
same pgn-extract book for both programs. The book was `pgn-extract/eco.pgn`,
SHA-256 `058ad9ffa6b393305520c5324f27598eae2ba8c3df966af790695a8ffcf66af3`.
The pgn-extract version was v26-06.

| Audit result | Games |
|---|---:|
| Input games examined | 1,000,000 |
| Reliably paired by `Mega2PGNRecordId` | 999,964 |
| Same calculated code | 999,690 |
| Different calculated code | 274 |
| Records pgn-extract skipped | 36 |
| Not classifiable by both programs | 30,715 |

The 274 different codes fall into two technical groups:

| Cause | Games | What differed |
|---|---:|---|
| Same strict position, different reference histories | 237 | Both codes matched the same legal position. pgn-eco ranked the competing references using move-history evidence. |
| Legal en-passant right changes position identity | 37 | The compared position candidates differed by a genuinely legal en-passant right. |

The audit traced pgn-extract v26-06's ECO matching through its position and
cumulative hashes, half-move count, and ordered reference scan. Its source
code and small controlled tests showed that, for the examined conflicting
transposition lines, reversing the lines in the book could reverse the chosen
code. This was demonstrated for the D90/D15, D43/D30, and C43/C24 conflicts.
It is evidence about those tested cases, not a claim that every pgn-extract
result depends on book order. pgn-eco's choices in the 237 cases were based
on a unique history-distance minimum, not reference order.

For the 37 en-passant cases, the audit found that the relevant candidate
positions matched in piece placement, side to move, and castling rights but
not in legal en-passant rights. pgn-eco keeps this distinction. Relaxing the
position key to imitate the other result would incorrectly call these states
identical under the rules used by pgn-eco.

Among the 274 disagreements, pgn-eco's calculated code matched the original
Mega ECO tag in 151 games; pgn-extract matched it in 37; neither matched it in
86. Those stored tags are useful context, but they are not an independent
ground truth. This comparison does not establish that either program
reproduces Mega's original classifier, nor does a smaller disagreement count
prove a more correct ECO taxonomy.

The audit covers one ECO book, the first million games of one database, and
the version-specific behavior of pgn-extract v26-06. Its conclusions should
not be generalized to other books, databases, or pgn-extract versions without
new measurements.

## Three- and four-character results

The default mode uses three-character codes. Mode 4 considers both
three-character and four-character matches. It prefers the deeper match,
even when that result is a three-character fallback. At equal depth, exact
history and the history comparison above are used. A unique four-character
code can refine a three-character result only when the candidates have the
same three-character prefix. An unresolved tie between different prefixes
remains ambiguous. The program never creates a suffix that is not in the
book and never shortens a five-character code.

An explicit `--eco-book FILE` is exclusive in both modes: only references
from that file are loaded. This also applies in mode 4. A three-character
fallback can come from a three-character reference in that same selected book;
no separate default book is added. Without `--eco-book`, a non-empty
`PGN_ECO_BOOK` selects the default. If it is unset or empty, the program
searches the configured per-user location and then installed locations.

## Input and analysis limits

pgn-eco is an ECO classifier, not a complete chess validator. It reads PGN
tags and checks the structure needed to identify complete games safely. It
legally parses relevant main-line moves from the start of the game. It skips
move-legality checks inside parenthesized variations and stops checking later
main-line move text when those moves cannot affect the opening result.

The normal maximum is 40 half-moves. Analysis can stop earlier at the
deepest useful position in the loaded book or when fewer than 20 pieces
remain. A single PGN record is limited to 64 MiB. An illegal main-line move
inside the analyzed opening prevents that opening line from being used.
Later illegal moves and illegal moves inside variations do not invalidate an
otherwise valid opening classification.

With --fill-missing, a syntactically supported ECO3 or ECO4 tag is copied byte
for byte and receives no chess classification. A game with multiple ECO tags
is also copied unchanged. A single unsupported value (including an empty
value, `?`, or a slash form such as `D37/22`) is classified and replaced only
if the result is unique. Otherwise its original bytes are retained.

Tag syntax is independent of the selected classification mode: both ECO3
(`B32`) and ECO4 (`A41e`) are supported existing values in either mode. In
`--check`, two supported values of different lengths but the same first three
characters are reported as prefix-compatible, not exact. `--update` follows
the selected mode when writing a new value and can replace unsupported values
or collapse duplicate tag lines when classification is unique. It leaves
existing ECO bytes unchanged when no unique classification is available.
Slash forms are never shortened or normalized implicitly.

## Book readers

The loader accepts Lichess TSV, generated TSV with checked move/position
columns, Scid .eco text, the escaped Scid export used by Caïssabase, and
pgn-extract's eco.pgn format. A book line is parsed as legal standard-chess
moves before it is indexed.

Some Scid book rows contain an incorrect check marker. When reading an
opening book only, the loader may retry a line after removing one terminal
plus sign from a move. The retry is accepted only if all moves then parse
legally and unambiguously. Stored PGN moves do not get this tolerance.

The pinned original Scid book used for the ECO4 default also contains two
French Tarrasch lines (C08l and C08m) where `Nbd7` omits the capture marker.
The Scid-book reader may try inserting one `x` into one SAN token, but accepts
the retry only when exactly one corrected full line passes the ordinary legal
PGN parser. This leaves the downloaded file unchanged and does not change SAN
handling for party PGNs or other book formats. The installed Scid file is
downloaded byte for byte from commit
`fc26337165346192b5399780357561401930dff4` and checked against SHA-256
`acd73837668a0791aa4d1b174fdfe8b19efa361ac2437b45735b397b0e20c4a7`.
Its header credits Shane Hudson (1999–2003). The upstream Scid `COPYING`
states that files other than named third-party contributions are GPL version
2; `scid.eco` is not named among those exclusions. The updater preserves its
header and installs the external data only in the user's data directory; the
project source archive does not bundle the book.

Book selection is deterministic: `--eco-book FILE` loads only `FILE`; without
it, a non-empty `PGN_ECO_BOOK` is used. Otherwise mode 3 searches for the
installed `lichess-openings.tsv`, while mode 4 searches for installed `scid.eco`
in the configured user directory and then system locations. The selected book
determines the references and codes available to classification. When
interpreting an audit result, record the exact selected book and its hash; ECO
books can differ in coverage and code assignments. `--update-books` installs
both defaults. Each book is staged, verified and parsed before its own atomic
activation. If the second activation fails after the first succeeds, the
diagnostic reports partial completion; the first book remains updated and the
second keeps its previous file.

## Output safety

Use `--check` for read-only operation. For file output, `--output FILE` combines
the selected inputs, while `--output-dir DIR` writes one file per input using
its basename. Both refuse existing destinations and publish completed output
from temporary files in the destination filesystem. `--output-dir` requires
an existing directory, and basename collisions are rejected before processing.
Bytes outside ECO tags being added or replaced are copied from the source games.

New PGN files use ordinary creation ownership and group rules. `--output-dir`
preserves each input's ordinary permission bits. A single-input `--output`
does the same; combined output from several inputs uses the intersection of
their ordinary permission bits. Special permission bits are not copied.

`--in-place` is explicit. It writes a temporary file beside each input and
replaces that file only after processing and checked writes, flush and close
succeed. The replacement is atomic per file where supported, and the existing
ownership and permissions are preserved. Unsafe aliases and symbolic or hard
links are rejected. A multi-file run is not transactional as a whole: files
completed before a later failure remain replaced.

`--output -` writes PGN directly to standard output. Reports and diagnostics
go to standard error, and no output file is created. Standard output cannot be
rolled back if a later read or write fails; a downstream pipe that closes early
is reported as an output error. Shell redirection controls the permissions of
any file opened by the shell.

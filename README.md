# pgn-eco

`pgn-eco` adds, checks and updates opening codes in chess game files. These
files use PGN, a text format for recording chess games. An ECO code, such as
`B32`, identifies a group of openings.

The program reads the opening moves and compares them with an ECO opening
book. It can work on one file or a collection of files, and it keeps the
original PGN content outside the ECO tags it changes.

## Is pgn-eco right for you?

Use it when you want to check or maintain ECO codes in PGN collections while
keeping comments, variations and other game text intact. It is designed for
large files as well as small ones: processing is streamed, and worker threads
are selected automatically for larger workloads.

Different opening books and classification programs can assign different
codes to the same game. `pgn-eco` uses the book you select, handles openings
played through different move orders, and reports unresolved choices instead
of guessing. Its tests and comparisons with other classifiers provide
evidence about compatibility and consistent behavior; they cannot establish
one universally correct ECO label for every game.

An exact move-order match is strongest. If two lines use the same moves in a
different order, pgn-eco favors the reference needing the fewest move-order
swaps. It also treats positions differently when an en-passant capture is
actually legal in one but not the other. If these clues do not separate the
candidates, it reports the result as ambiguous.

In a full-database comparison, pgn-eco matched 6,914,075 of 6,916,303
existing ECO4 tags (99.9678%) in one Scid-based collection. It also matched
99.9726% of 999,964 paired ECO3 results from pgn-extract when both programs
used the same book. These figures measure compatibility with existing labels
and another program, not independent proof of a single correct answer. See
the [validation summary](docs/validation-summary.md) for the scope and limits.

`pgn-eco` is an opening classifier, not a full game validator. It checks
relevant legal moves near the start of the main line. It does not check every
later move or the moves inside variations. If you need complete game
validation, use a PGN validation tool as well.

## Choose what to do

| Task | What it does |
| --- | --- |
| `--check` | Reports how existing ECO tags compare with the selected book. It never changes the PGN files. |
| `--output FILE` | Recalculates tags and writes all input games to a new combined file. |
| `--fill-missing --output FILE` | Adds a missing tag or replaces one unsupported value when a unique result is available. Supported ECO3/ECO4 tags and games with duplicate ECO tags are left unchanged. |

For output modes, `--output-dir DIR` writes one new file per input. Use
`--in-place` only when you want to replace the input files themselves. A
unique classification is not always possible, so some games may remain
unchanged or unclassified.

New output files are not allowed to overwrite existing files. The original
inputs are unchanged unless you explicitly choose `--in-place`. Outside ECO
tag lines that are deliberately changed, the original PGN bytes are
preserved.

## Try it

Open a terminal, go to the pgn-eco project directory, and build the program:

```sh
make
```

From that directory, use `./pgn-eco`. If it is installed on your system, use
`pgn-eco` instead. To run the regression tests, use `make test`.

Install the standard opening books once. This needs an internet connection;
checking and classifying games works offline afterward.

```sh
./pgn-eco --update-books
```

Check a file without changing it:

```sh
./pgn-eco --check games.pgn
```

Create a newly classified copy. Choose an output name that does not already
exist:

```sh
./pgn-eco --output games-classified.pgn games.pgn
```

Fill missing or unsupported tags in a separate copy:

```sh
./pgn-eco --fill-missing --output games-completed.pgn games.pgn
```

For more than one input, list the files in the order you want them processed.
The shell expands wildcards, for example `games-*.pgn`. See the manpage for
separate output files, standard output, in-place processing and their safety
rules.

## ECO3 and ECO4

The normal mode uses three-character codes such as `B32` and the Lichess
opening book. With `--mode 4`, the program can use Scid's four-character
codes, such as `A41e`. `--update-books` installs both books. You can also
provide your own book:

```sh
./pgn-eco --mode 4 --eco-book /path/to/scid.eco --check games.pgn
```

An explicit `--eco-book FILE` is the only book used for that run. Without
it, `PGN_ECO_BOOK` or the installed default is used. Books can differ in the
openings they cover and in the codes they assign. Mode 4 may return a
three-character code when that deeper opening match is better supported than
a shorter four-character match. Five-character ECO codes are not supported.

## Learn more

- Run `./pgn-eco --help` for a quick option list.
- Read [`man/pgn-eco.1`](man/pgn-eco.1) or `man pgn-eco` for full command
  syntax, output modes, safety details, environment settings and exit codes.
- Read [`docs/technical-notes.md`](docs/technical-notes.md) for how position
  matching, transpositions and classification comparisons work.
- See `LICENSE` and `THIRD_PARTY_NOTICES.md` for software and data terms.

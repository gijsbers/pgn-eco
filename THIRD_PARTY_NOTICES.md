# Third-party data and notices

The source package does not include a complete ECO opening book or chess-game
database. pgn-eco can download opening books separately with
`pgn-eco --update-books`.

## Lichess opening data

The Lichess opening dataset is not bundled in this package. The upstream
project describes the dataset as public domain and releases it under CC0
where copyright or database rights apply:

- <https://github.com/lichess-org/chess-openings>
- License: CC0-1.0

## Scid ECO book

The original Scid ECO book is not bundled in this package. The updater
downloads it from the Scid source repository at commit
`fc26337165346192b5399780357561401930dff4`.

The COPYING file at that revision states that Scid files are generally under
GNU GPL version 2, subject to specifically listed exceptions. The ECO book is
not named in that exception list. The downloaded file retains its upstream
notice. See:

- <https://github.com/benini/scid/blob/fc26337165346192b5399780357561401930dff4/scid.eco>
- <https://github.com/benini/scid/blob/fc26337165346192b5399780357561401930dff4/COPYING>

The Scid book has separate terms from the pgn-eco software. This project does
not redistribute it in this source package.

## Tests

The included position-key test and small smoke-test PGN/book are project test
fixtures. The smoke-test files are intentionally small and contain only
handwritten sample tags and opening moves.

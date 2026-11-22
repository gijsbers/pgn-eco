# Validation summary

These results measure agreement with stored ECO tags or another classifier.
They are compatibility evidence, not an independent proof that one ECO label
is objectively correct.

| Comparison | Result | Scope |
| --- | ---: | --- |
| ECO3: pgn-eco and pgn-extract v26-06, using the same `eco.pgn` book | 999,690 / 999,964 paired games (99.9726%) | First 1,000,000 games of a Mega collection; 36 pgn-extract records could not be paired. |
| ECO4: pgn-eco and stored tags, using the original Scid book | 6,914,075 / 6,916,303 valid ECO4 tags (99.967786%) | Complete Lumbra GB OTB Complete edition dated 11 November 2026. |

For the ECO3 comparison, pgn-eco investigated all 274 different calculated
codes. Most came from competing reference histories to the same position;
others involved a genuinely legal en-passant right. The stored Mega tags were
not treated as ground truth.

For the ECO4 comparison, the stored tags use a Scid-family scheme. This is a
close compatibility test, but the stored labels and reference book may share
their origin. The result therefore does not constitute independent
validation of ECO taxonomy. The non-exact ECO4 results were retained and
diagnosed in the local research study; the underlying game databases and
per-game research tables are not part of this source package.

The results apply only to the named editions, books and program versions.
They should not be assumed for other databases, books, or later releases.

# Clipboard song-import fixtures

These `.txt` files are the stimulus for the clipboard song import
(`SongImportPage`). Copy a whole file to the normal clipboard and press
**Paste from clipboard** on the import page.

They are deliberately **synthetic or public-domain test data**, not real
song lyrics and not the operator sample included in the task description
(which is copyrighted and must not enter this repository). The CCLI
numbers `9000001` and `9000002` are fictional.

What each file exercises:

| File | Shape |
|---|---|
| `morning-light.txt` | A title line, bracketed `[Verse 1]`/`[Chorus]`/`[Bridge]`/`[Ending]` labels, and the full footer: writers, `CCLI Song #…`, © line, permission line, licence line |
| `grace-abounding.txt` | Bare `Verse 1`/`Chorus`/`Bridge 1`/`Vamp` labels and a footer whose author line is separated from the markers by a blank line |
| `simple-praise.txt` | No labels at all; blank lines are the stanza boundaries |
| `long-verse.txt` | One long verse that must split across several slides at authored line boundaries, with a chorus that must **not** share the verse's last slide |

Expected import behaviour:

- A lone title before the first explicit section becomes editable metadata.
  Unlabelled songs and multi-line preambles retain all lyric lines. Prepared
  songs begin with one intentional blank cue, outside the catalog stanzas.
- Every section starts on a new slide. A section that does not fit is
  split at line boundaries (then at words for a single over-long line)
  and its continuation stays in that section — a later section never fills
  a previous section's leftover space.
- The footer never becomes slide text. The writers, CCLI number and © /
  licence block are stored in the song record for reporting.

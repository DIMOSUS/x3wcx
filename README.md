# x3wcx — X3 catalogs and .pck files in Total Commander

[Русский](README.ru.md)

A packer plugin (WCX) for Total Commander. It opens these as ordinary archives:

- **X3 catalogs** (X2, X3: Reunion, Terran Conflict, Albion Prelude): an index `NN.cat` and its paired data file
  `NN.dat`;
- **`.pck` files** (gzip-compressed XML and TXT): an archive with one file inside.

You can view (F3), unpack (F5), edit (F4), add, replace and delete files right in TC. No external libraries are
needed: deflate/inflate are built in (`src/flate.cpp`).

## Installation

1. Put `x3wcx.wcx` (32-bit TC) and `x3wcx.wcx64` (64-bit TC) in one folder, e.g.
   `%COMMANDER_PATH%\Plugins\wcx\x3wcx\`. Easier: open `x3wcx.zip` in TC. TC offers to install the plugin
   (`pluginst.inf`) and binds it to `cat`.
2. **Configuration → Options → Plugins → Packer plugins (WCX) → Configure**:
   - type `cat` as the extension, press "New type" and pick `x3wcx.wcx` (64-bit TC takes the `.wcx64` next to it);
   - repeat for `pck`.
3. If the extension is already taken by another plugin, TC asks which one to use. Do not bind `.dat`.

An archive with another extension opens with Ctrl+PgDn. The plugin recognizes a catalog by the first index line
(`*.dat` once decrypted) and a `.pck` by the gzip signature, masked or not. By content, a `.pck` is recognized only
when its gzip header stores no file name, as in the game's files, so ordinary `.gz` files from gzip are left alone.

## Features

| | `.cat` + `.dat` | `.pck` |
|---|---|---|
| List, F3, F5, test | yes | yes, one file |
| New archive (Alt+F5) | yes, `NN.cat` + `NN.dat` | yes, unmasked, like the game's `addon\scripts` |
| Add | appended to the end of `.dat` in place; `.dat` is not rewritten | — |
| Replace (same path, any case) | the file keeps its place in the index and its name | only a file with the same name (any case, any extension); a foreign file is refused (`E_NOT_SUPPORTED`); mask and key kept |
| Delete, including folders | yes | no (`E_NOT_SUPPORTED`) |
| F4 — edit a file inside | yes | yes |

- **Paths inside a catalog** are stored with forward slashes in latin1, as the game does. A name with characters
  outside latin1 is refused (`E_BAD_DATA`).
- **The first index line** (a `.dat` name the game does not use) is kept as it was when a catalog is changed. In a new
  catalog it names the paired `.dat`. A new catalog is byte-identical to what `x3kc cat --pack` writes.
- **The extension of the file inside a `.pck`** comes from its folder: `scripts`, `t`, `director`, `maps` → `.xml`,
  `types` → `.txt`. In any other folder it comes from the content: `.xml` if the text starts with `<`, else `.txt`.
  This matters for a `.pck` opened from inside a catalog: TC unpacks it to a temp folder, and the original folder name
  is lost.
- **Progress and Cancel** work for unpacking and writing. Cancel leaves the catalog as it was.
- **Text search** (Alt+F7, "search archives") looks inside catalogs and `.pck` files.
- **A `.pck` made of several gzip members** is read whole, as `unpck.py` does, and written back as one member.

## How writes stay safe

- **Add** appends to `.dat`, then replaces `.cat` atomically (a `*.x3wcx-tmp` file and a rename). On failure `.dat`
  is cut back to its old length. The old index never points into the new tail, so a crash at any step leaves a working
  catalog.
- **Deleting trailing files** writes the new index first, then cuts `.dat`.
- **Replacing or deleting in the middle** writes a new `.dat` and `.cat` next to the old ones (`*.x3wcx-tmp`), then
  swaps the pair by renames. Meanwhile the old pair is kept as `*.x3wcx-bak` and removed after success; on error it is
  put back. If Windows crashes exactly between the two renames, restore both files from `*.x3wcx-bak` by hand.
  While a `*.x3wcx-bak` lies next to a catalog, the plugin refuses to change it (`E_ECREATE`), so as not to overwrite
  what may be the only whole copy: restore the pair or remove the extra files first.
- **Disk space.** Such a rewrite needs free space the size of the `.dat`. For the stock `01.dat` (1.3 GB) and `02.dat`
  (2.1 GB) it is slow and needs that space.
- **A catalog without its `.dat`.** If the `.dat` is gone while the index lists files, adding is refused and no empty
  `.dat` is created.
- **`.pck`** is written through a temp file and a rename.

## Warning: editing stock catalogs changes the game

Catalogs `01.cat`–`13.cat` and `addon\01.cat`–`addon\04.cat` are the game's own data. Any change to them changes the
game at once, may break save compatibility, and is undone by Steam's file verification. **Put mods into a new catalog
with the next number**: the game reads catalogs in order, and a file in a higher-numbered catalog overrides the same
path in a lower one (this is how `addon\05.cat`, for example, works). To make a new catalog in TC, select files, press
Alt+F5, name it `addon\06.cat` and choose the `cat` packer.

Back up stock catalogs before experimenting with them.

## Nested archives: `.pck` inside `.cat`

TC can enter an archive inside an archive: Enter on a `.pck` in a catalog unpacks it to a temp folder and opens it
with the plugin. According to TC's help ("Packed files"), the temp copy is deleted when you leave the nested archive.
Neither the help nor the TC 11.56 history says that changes to a nested archive are packed back into the outer one.
This needs a manual check (see below). **Until then, assume that edits inside a nested `.pck` do not reach the outer
`.cat`.** The reliable way:

1. copy the `.pck` out of the catalog to disk (F5);
2. open it there and edit it as an archive (F4 on the `.xml` inside);
3. copy the edited `.pck` back into the catalog (F5 into the panel with the `.cat` open); it replaces the old one in
   place.

## Limitations

- Catalogs store no file times: every file shows the `.cat` date.
- Catalogs store no empty folders: folders come only from file paths.
- A `.pck` is unpacked whole in memory (the game's files are a few MB at most).
- Background operation (`GetBackgroundFlags`) is not declared: TC runs operations in its main thread.
- `.pbd`/`.pbb` (compressed models) show as plain files and are not unpacked.

## Build and tests

Needs Visual Studio 2019+ (or Build Tools) with the C++ workload:

```
build.cmd
```

This produces `build\x3wcx.wcx`, `build\x3wcx.wcx64`, `build\x3wcx.zip` and the test drivers
`build\wcxdrv32.exe`/`wcxdrv64.exe`. A driver loads the plugin and calls its exports the way TC does.

The tests compare the plugin with the Python reference, `catx.py` and `unpck.py` from
[x3kc](https://github.com/DIMOSUS/x3kc):

```
python tests\test_wcx.py --game "D:\GAMES\X3 Terran Conflict" --x3kc D:\GAMES\x3kc [--quick]
```

- Game catalogs are only read: the list and the contents of every file in every catalog are checked.
- Changes are made on copies in a temp folder and compared byte for byte with `catx.write_catalog`.
- `.pck` files go round unpack → pack → unpack; Python's `gzip` checks the result as well.
- `--quick` skips the two big catalogs (01, 02).

# Third-party notices

EROS itself (everything under `src/`, `apps/music/` except `third_party/`,
`tools/`, `mk/`, and the config/res/scripts written for this project) is
licensed **0BSD** — see `LICENSE`.

A shipped EROS SD card (`out/sd/`) also redistributes third-party software that
keeps its own license. This file lists those components and their terms. It is
copied onto the card as `eros/THIRD-PARTY-LICENSES.md` by `mk/payload.sh`.

Two components carry **non-commercial** licenses (snes9x, PicoDrive). Because a
built payload links and ships them, **an EROS card that includes those cores may
not be sold.** Ship without them, or replace them, to distribute commercially.

---

## Frontend

### minarch (the in-game libretro host)
- **Origin:** built from **NextUI** source (LoveRetro/NextUI), which EROS
  patches and rebuilds (`minarch/`, `minarch/build.sh`).
- **License:** **GPL-3.0**. https://github.com/LoveRetro/NextUI
- The three files under `minarch/overrides/all/` (`common/api.c`,
  `minarch/ma_menu.c`, `minarch/ma_frontend_opts.c`) are copied-and-patched
  NextUI GPL-3.0 source and remain GPL-3.0.
- **Source availability (GPL §6):** the corresponding source for the shipped
  `minarch.elf`, cores, and NextUI-derived runtime libraries is the upstream
  repositories listed here, at the release tag pulled by `mk/fetch-vendor.sh`
  (currently NextUI `v6.11.2`), plus the EROS patches in `minarch/`.

### NextUI runtime libraries and assets
Pulled by `mk/fetch-vendor.sh` from a NextUI release into `vendor/`:
- `vendor/lib/libmsettings.so`, `libgametimedb.so`, `libbatmondb.so` — NextUI/MinUI
  device libraries.
- `vendor/system/res/` — NextUI UI image assets, loaded by minarch from its
  hardcoded `/.system/res` path: `assets@Nx.png` (the in-game menu glyph sheet)
  and `line-N.png` / `grid-N.png` (CRT scanline/grid overlays). EROS does **not**
  use the NextUI fonts (`font1.ttf`, `font2.ttf`, `BPreplayBold-unhinted.otf`) —
  the in-game menu is forced to EROS's own `menu.ttf` — nor the MinUI launcher
  assets, since EROS ships its own launcher.
- These follow their NextUI upstream licensing (GPL-3.0 / MinUI MIT, and the
  individual asset/font licenses). See https://github.com/LoveRetro/NextUI.

---

## libretro cores (`vendor/cores/`, shipped as `eros/cores/`)

| Core | System | License | Notes |
|------|--------|---------|-------|
| `fceumm_libretro.so` | NES | GPL-2.0-or-later | |
| `mgba_libretro.so` | GBA | MPL-2.0 | |
| `gambatte_libretro.so` | Game Boy / Game Boy Color | GPL-2.0 | |
| `picodrive_libretro.so` | Genesis / Master System / Game Gear | PicoDrive license | **Non-commercial** (MAME-style) |
| `snes9x_libretro.so` | SNES | Snes9x license | **Non-commercial** — personal use only |
| `mednafen_pce_fast_libretro.so` | TG-16 / PC Engine | GPL-2.0-or-later | Mednafen-derived |
| `pcsx_rearmed_libretro.so` | PlayStation | GPL-2.0 | |

Core source: the libretro organization / each core's upstream repository
(https://github.com/libretro).

---

## Runtime libraries (`vendor/lib/`, shipped as `eros/lib/`)

Pulled from the NextUI release; standard shared libraries linked by minarch/cores:

| Library | Project | License |
|---------|---------|---------|
| `libcrypto.so.1.1` | OpenSSL 1.1 | OpenSSL + SSLeay (dual) |
| `liblzma.so.5` | xz-utils | Public domain / BSD-0 |
| `libbz2.so.1.0` | bzip2 | bzip2 (BSD-style) |
| `libzstd.so.1` | Zstandard | BSD-3-Clause (or GPL-2.0) |
| `liblz4.so.1` | LZ4 | BSD-2-Clause |
| `libzip.so.5` | libzip | BSD-3-Clause |
| `libchdr.so.0` | libchdr | BSD-3-Clause |
| `libsamplerate.so.0` | libsamplerate | BSD-2-Clause |

---

## Muse (music player) — bundled decoders

Single-header audio decoders vendored under `apps/music/third_party/`, compiled
into the `muse` binary:

- **dr_wav / dr_mp3 / dr_flac** — David Reid (mackron). Choice of **public domain
  (Unlicense) or MIT-0**. https://github.com/mackron/dr_libs
- **stb_vorbis** — Sean Barrett. **Public domain (or MIT)**.
  https://github.com/nothings/stb

Muse's own code (`audio.c`, `library.c`, `player.c`, `music.c`) is 0BSD, part of
EROS.

---

## Fonts (EROS-original UI)

- `res/fonts/menu.ttf` — **Josefin Sans**, SIL Open Font License 1.1. Full text
  in `res/fonts/OFL.txt`.

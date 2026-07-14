# minarch overrides — NextUI-derived (GPL-3.0)

Every source file under `minarch/overrides/` is a copy of a file from
[NextUI](https://github.com/LoveRetro/NextUI) (© the NextUI / MinUI authors),
patched for EROS and overlaid onto a pristine NextUI checkout by
`minarch/build.sh`. NextUI is licensed **GPL-3.0**, so these files — and the
`minarch.elf` (shipped as `vendor/minarch.elf` / `eros/minarch.elf`) built from
them — are **GPL-3.0**, not EROS's 0BSD. Each file carries an
`SPDX-License-Identifier: GPL-3.0-only` header saying the same.

Files:

- `all/common/api.c`
- `all/common/notification.c`
- `all/minarch/ma_frontend_opts.c`
- `all/minarch/ma_input.c`
- `all/minarch/ma_menu.c`

**Source availability (GPL §6):** the unmodified upstream is the NextUI tag
pinned in `mk/fetch-vendor.sh`; the EROS modifications are exactly the diffs in
this directory against that tag. See `THIRD-PARTY-LICENSES.md` at the repo root
for the full third-party notice.

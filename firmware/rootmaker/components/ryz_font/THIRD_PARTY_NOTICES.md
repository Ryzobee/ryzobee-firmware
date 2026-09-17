# Third-party notices

## Embedded font subsets

The firmware embeds modified static TTF subsets (printable ASCII for all faces,
plus the original U+00B0 degree sign for Teko) of the
following font software:

- Noto Sans Regular 400, Medium 500 and SemiBold 600 — Copyright 2022 The Noto Project Authors
  (https://github.com/notofonts/latin-greek-cyrillic).
- Teko SemiBold 600 — Copyright 2023 The Teko Project Authors
  (https://www.github.com/googlefonts/teko).
- Roboto Mono Regular 400, Medium 500 and SemiBold 600 — Copyright 2015 The Roboto Mono
  Project Authors (https://github.com/googlefonts/robotomono).

These font families and generated subsets are distributed under the SIL
Open Font License, Version 1.1. The license text is archived at
`LICENSES/OFL-1.1.txt`. The source and output hashes for each generated subset
are recorded in `assets/manifest.json`. Roboto Mono's original copyright and
complete license are also preserved verbatim in `LICENSES/RobotoMono-OFL.txt`;
the fixed official source revision is recorded in `sources/roboto_mono/README.md`.

## FreeType

This component links the FreeType library through Espressif's managed FreeType
component, pinned in `idf_component.yml`.

This software is based in part on the work of the FreeType Team
(https://freetype.org).

Ryzobee uses FreeType under the FreeType License. The complete license text is
archived at `LICENSES/FTL.txt`; FreeType source files supplied by the managed
component retain their original notices and any file-specific licenses.

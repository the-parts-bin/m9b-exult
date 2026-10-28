# Ultima VII's data

Commercial games, so you need your own copies of the DOS versions. Either
game can be left out. Symbolic links work for all of these.

| Here | What | Tested version |
|---|---|---|
| `data/ULTIMA7/` | The Black Gate's install directory (holds `STATIC/`) | with The Forge of Virtue: `STATIC/USECODE`, 1,521,368 bytes, MD5 `07ea98fe080ea7dc720492856ebf0fb8` |
| `data/SERPENT/` | Serpent Isle's install directory (holds `STATIC/`) | with The Silver Seed: `STATIC/USECODE`, 2,103,075 bytes, MD5 `3a9a735b83817c7ad3a94b6f5f88e675` |
| `data/exult-audio/` (optional) | Exult's Roland MT-32 packs from Exult's download page, each unpacked into its own directory: `U7MusicOGG_1of2/`, `U7MusicOGG_2of2/` (the music, 76 `.ogg` files), `sqsfxbg/`, `sqsfxsi/` (`sqsfxbg.flx`, `sqsfxsi.flx`) | Music Pack 1.0, SFX packs 1.1 |

With `exult-audio/`, both games get the MT-32 music and sound effects;
without it, The Black Gate's AdLib music is rendered from `ULTIMA7/` and
Serpent Isle has no music and neither game has sound effects.

`data/exult/` is made by `tools/get_data.sh` (the SD card build in
m9b-system runs it): Exult's own data files (fonts, menus), fetched from
Exult 1.12.1's source on GitHub and packed.

The original engine (m9b-u7, in ../u7) uses the same data on the card, so it
needs nothing more here.

# Weather assets — third-party attribution

The 26 `icon_*.c` files in this directory are LVGL-formatted C bitmap arrays
generated from the Google Weather icon set, redistributed via:

  https://github.com/mrdarrengriffin/google-weather-icons

The icons originate from Google. mrdarrengriffin's repo carries no formal
license; per their README: "I do not own these icons. All rights belong to
Google." Used here in a personal-use, non-commercial home-automation context
with attribution.

The `lv_font_montserrat_latin_*.c` files are derivatives of Montserrat
(SIL Open Font License), generated as LVGL bitmap fonts with extended Latin
range (covers ASCII + Latin-1 supplemental, including £, °, etc.).

Both sets of files were copied verbatim from:

  https://github.com/Surrey-Homeware/Aura

The Aura project's `weather.ino` source is GPL-3.0; that licence does NOT
extend to these asset files (icons and fonts are governed by the licences
above). No `.ino` code was copied from Aura — the weather fetch and display
code in this project is original work, written from scratch following the
Open-Meteo API documentation directly.

# The wrapper UI: third-party notices

The start screen, in-game menu and settings (`port/ui`, docs/32) are built from the software and fonts below. The
About tab of the settings lists the same.

| What | Used for | Licence | Where |
| --- | --- | --- | --- |
| [Dusklight](https://github.com/TwilitRealm/dusklight) | The interface code (`src/dusk/ui`) and stylesheets (`res/rml`), adapted by way of FFCCEOT-Recomp. Each adapted file says so in its header. | CC0 1.0 (public domain) | `port/ui/src`, `port/ui/res/rml` |
| FFCCEOT-Recomp | The same interface as adapted there (D3D/SDL host, settings, embedding); by the author of this port | The author's own, or Dusklight's (CC0) | `port/ui/src`, `port/cmake/EmbedFiles.cmake` |
| [RmlUi](https://github.com/mikke89/RmlUi) 6.3 | The HTML/CSS interface library, with its D3D11 renderer and Win32 helpers (`Backends/`) | MIT (below) | Downloaded at configure time (FetchContent), compiled in |
| [FreeType](https://freetype.org) 2.14.3 | Font rendering for RmlUi | FreeType Licence (FTL) | Downloaded at configure time (FetchContent), compiled in |
| [Cinzel](https://github.com/NDISCOVER/Cinzel) | Headings | SIL Open Font Licence 1.1 | `res/fonts/Cinzel.ttf`, licence in `res/fonts/OFL-Cinzel.txt` |
| [Alegreya Sans](https://github.com/huertatipografica/Alegreya-Sans) | Body text | SIL Open Font Licence 1.1 | `res/fonts/AlegreyaSans-*.ttf`, licence in `res/fonts/OFL-AlegreyaSans.txt` |
| [Material Symbols Rounded](https://github.com/google/material-design-icons) | Icons | Apache-2.0 | `res/fonts/MaterialSymbolsRounded-Regular.ttf`, licence in `res/fonts/LICENSE-Apache-2.0.txt` |

SDL is not used: the gamepads are read with XInput and the runtime's own Sony HID reader, the keyboard and mouse
through the game window's messages.

No art from Dusklight, FFCCEOT-Recomp or the game is used. The fonts and stylesheets are compiled into the executable.

## FreeType

Portions of this software are copyright © 2025 The FreeType Project (www.freetype.org). All rights reserved.

## RmlUi

```
MIT License

Copyright (c) 2008-2014 CodePoint Ltd, Shift Technology Ltd, and contributors
Copyright (c) 2019-2026 The RmlUi Team, and contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

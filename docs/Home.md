# XLT developer docs

These pages are for anyone who wants to read or change XLT's code: how each part works, the firmware
calls it relies on, what was measured on hardware, and how it is tested. For what XLT is and how to use it,
see the [README](../README.md).

They are written to work as a GitHub wiki too: copy the `.md` files into the wiki unchanged.

| Page | What it covers |
|---|---|
| [Look and sound](Look-and-Sound.md) | The logo, the XMB icon, background and preview (and the PMF muxer), themes, interface sounds |
| [Start-up](Startup.md) | The boot screen and library check, safe start |
| [How XLT is built](Architecture.md) | Threads, the atlas, drawing, memory, the VRAM layout |
| [Library and games](Library-and-Games.md) | Scanning and the index, launching through ARK, the options sheet, folders, removed entries, the UMD drive |
| [System screens](System.md) | Settings (ARK, plugins, the registry, free space), the control center, volume and AVLS, USB mode, screenshots in games (the xltshot plugin), the screen recorder, the screensaver |
| [Music, video and pictures](Media.md) | The music player (MP3, AAC, FLAC, levelling), the video player (MP4, PMF, subtitles, the weighted-prediction stripper), pictures, live previews |
| [Files, save data, reader, web and search](Files-and-Apps.md) | The file manager, ZIP, save data, the reader (text, PDF), the web browser, search |
| [Building and testing](Building-and-Tests.md) | Building, the host tests and screen previews, the demo reel |
| [License and credits](Credits.md) | License, code adapted from other projects, libraries and references |

# mdview

A small Markdown viewer that runs on everything from Windows 95 to Windows 11. It is a single
self-contained `.exe` with no dependencies, and it draws everything itself with plain GDI (no RichEdit,
no browser control), so it looks like a classic Windows help file.

## Features

- Headings, bold, italic, inline code, fenced and indented code blocks, nested lists, block quotes,
  horizontal rules, links, and tables (borders, padding, alignment, wrapping)
- Code blocks scroll horizontally instead of wrapping
- Find, zoom (Ctrl+wheel, Ctrl +/-), light and dark themes, smooth scrolling, drag a file onto the window
- Middle-click autoscroll (Windows NT family)
- Emoji and other symbols on Windows 10/11 (monochrome); older Windows shows `?`
- Remembers its window size, position and settings (`HKCU\Software\mdview`)

## Install

Run `mdview-setup-<version>.exe`. It installs mdview, adds a Start Menu entry and registers the `.md`
file type. `setup.exe /S` installs silently (close any running mdview first).

## Build

Needs [Open Watcom 2](https://github.com/open-watcom/open-watcom-v2) and PowerShell. Set `WATCOM` to its
install folder (default `C:\watcom`), then:

```powershell
.\build.ps1
```

This builds `mdview.exe` and `setup.exe` (which embeds it) and copies the installer to
`dist\mdview-setup-<version>.exe`. `.\build.ps1 mdview` builds only the viewer. `make_icon.py` regenerates
`mdview.ico`.

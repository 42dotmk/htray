# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

htray is a single-file X11 program (`htray.c`): a toggleable overlay bar that acts as the system tray (owns the `_NET_SYSTEM_TRAY_S<n>` selection, docks applet icons via XEmbed) and shows a status readout (workspace, CPU, memory, volume/mic via `wpctl`, battery, clock). Suckless-style: configuration lives in `config.h` (included by `htray.c`), not a runtime config file — changing defaults means editing it and recompiling. The first block of settings (height, colors, font, font size, paddings, clock format, ...) can additionally be overridden at startup by `HTRAY_*` environment variables; each setting's comment names its variable, and `loadconfig()` in `htray.c` applies them. The icon-dimension/template block below it is compile-time only (the `draw*` functions hardcode strokes to those sizes).

## Build

```sh
make            # builds ./htray (needs libX11 + libXrandr headers)
make install    # symlinks it into ~/.local/bin
make clean
```

There are no tests or lint targets; the compiler flags (`-std=c99 -pedantic -Wall -Wextra`) are the lint. Keep the build warning-free.

`vendor/stb_ds.h` is the only dependency beyond Xlib/Xrandr; it provides the dynamic array (`arrput`/`arrdel`/`arrlen`) used for the docked-icon list.

## Architecture

Everything lives in `htray.c`, structured as:

- `setup()` acquires the tray selection (exits if another tray owns it), creates the override-redirect bar window, and announces the tray via a MANAGER client message so waiting applets dock.
- `run()` is a `select()` loop over the X connection fd with a timeout: 1s while visible (so volume/CPU readouts stay fresh), sleep-to-the-next-minute while hidden. `SIGUSR1` (sent e.g. by `pkill -USR1 -x htray` from a WM keybind) sets a flag that toggles bar visibility, `SIGUSR2` likewise toggles input-box focus (showing the bar first if hidden); the signal handlers are installed without `SA_RESTART` on purpose so they interrupt `select`.
- `handle()` processes X events: dock requests (`_NET_SYSTEM_TRAY_OPCODE`), icon destruction/reparenting (undock), Expose (redraw), `_NET_CURRENT_DESKTOP` changes on the root window, SelectionClear (another tray took over → exit), and Configure/Map of other top-level windows, which re-raises the bar to keep it always on top.
- `updatestatus()` polls `/proc/stat`, `/proc/meminfo`, `/sys/class/power_supply/BAT*`, and `wpctl`; it snapshots all values into `stattxt` and returns whether anything changed, so redraws only happen on change.
- External commands must go through `readcmd()` (fork/exec + pipe), which kills the child after `cmdtimeout` ms — never `popen`, which once let a hung `wpctl` block the bar from ever mapping.
- The input box next to the workspace (click to focus via `XGrabKeyboard`, Enter runs the line through `sh -c`, Escape cancels) runs its command asynchronously: `runinput()` forks with a pipe whose fd joins `run()`'s `select`, `cmdread()` accumulates the output, and on EOF `notifycmd()` shows it as a desktop notification (`notify-send`, command line as the title).
- `layout()` recomputes bar width/position (right-aligned on the second monitor when RandR reports one, bottom or top per `atbottom`) and slots each icon; `drawbar()` renders text plus small hand-drawn Xlib icons (`drawbattery`, `drawcpu`, `drawmem`, `drawspeaker`, `drawmic`).

Readout widths are reserved via the `*tmpl` template strings so the bar doesn't resize as values fluctuate — keep a template in sync when changing a readout's format.

Tray icons come and go asynchronously; the X error handler deliberately ignores all errors so operations on vanished icon windows don't kill the process.

## Style

Follows suckless/OpenBSD C style: C99, tabs, return type on its own line, no dynamic allocation beyond stb_ds, fixed-size buffers with `snprintf`. Match it.

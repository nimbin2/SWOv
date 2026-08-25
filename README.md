# swov

A window and workspace overview for [Sway](https://swaywm.org), drawn with SDL3.
Each workspace is a tile; inside it, windows sit where they sit on the real
screen. Click one to focus it, or drag it somewhere else.

![swov](screenshot.png)

100% vibecode, but tested.

## Build

Debian trixie and newer:

```sh
sudo apt install build-essential pkg-config libsdl3-dev libsdl3-image-dev libsdl3-ttf-dev
make
make install          # ~/.local/bin, or PREFIX=/usr/local
make config           # optional: config.example -> ~/.config/swov/config
```

`make debug` builds `swov-debug` with the address and UB sanitizers.

On bookworm the SDL3 packages do not exist yet; build SDL, SDL_image and
SDL_ttf 3.2+ from source and set `PKG_CONFIG_PATH`.

Needs a running sway session (`$SWAYSOCK`) and any TTF font.

## Use

```
bindsym $mod+Tab exec swov
for_window [app_id="swov"] floating enable, border none
```

| key | action |
| --- | --- |
| `0`–`9` | switch to that workspace |
| `ctrl+0`–`9` | move the selection there |
| arrows, `hjkl` | move the selection; it walks through tile borders and wraps around the grid |
| `tab` / `shift+tab` | previous / next workspace |
| `ctrl+tab` (`+shift`) | one row down / up in the grid |
| `w` | window selection ⇄ whole-workspace selection |
| `enter`, click | focus |
| `space`, right click | mark or unmark a window (marks are what the next action applies to) |
| `shift+space`, `a` | mark or unmark the whole workspace |
| `c` | clear all marks |
| `x`, `del` | close marked or selected windows, `enter` confirms |
| middle click | close that window straight away |
| `f` | find windows by app id, title or workspace name, highlighting the hits |
| `/` | filter: same search, but hides everything else |
| `r` | reload |
| `esc` | cancel a drag, else quit |

Mouse and keyboard share one cursor: pointing at a window selects it, so
`space`, `x`, `ctrl`+digit and the rest act on whatever is under the pointer.
Click a workspace name in the header to rename it; `enter` keeps it, `esc`
drops it. The name is what `f` and `/` search, next to app ids and titles.
swov opens with the current workspace selected as a whole, no window picked.
Orange is only ever the selection cursor; the workspace sway is showing and the
window it has focused are marked in teal (`current`), search hits in violet
(`match`).

## Drag and drop

Press, move, release. `Esc` cancels.

A floating or fullscreen window is grabbed by its name plate — the rest of it is
click-through, so the windows underneath stay selectable.

**A window** onto a tile moves it there. Onto the left or right edge of another
window it lands beside it, splitting horizontally; top or bottom splits
vertically. A bar shows the edge before you let go.

**A workspace** (grab the header strip) onto another swaps the two. Onto the
left or right quarter of another it inserts there, pushing the occupied run up
by one.

**Ghost slots** are the free numbers 0–10. They appear when a workspace drag
starts, or when a dragged window leaves its own workspace, and the tiles glide
aside to make room. Drop on one to give a workspace that number, or to create a
workspace from a single window.

## Without opening the overview

```sh
swov -g 3        # switch to workspace 3 and exit
swov -g 3:code   # by name works too
swov -b          # switch to the workspace you came from
```

Both talk to the IPC socket and quit — no window, no font, about 5 ms. Good for
keybindings.

## Workspace usage

Every switch goes through swov, so it stamps the time as it goes: the workspace
you leave is credited with the seconds since the last switch, and the new one is
noted in `~/.cache/swov/usage`. No background process.

The overview draws it as a dot scale down the left edge of each tile — fourteen
dots filling from the bottom, relative to the busiest workspace — and prints the time
next to the window count. A workspace that falls empty is forgotten and starts
from zero.

```sh
swov --usage     # where you are, and how long each workspace has had you
swov --info      # every path and setting in use: binary, config, usage file,
                 # sway socket, fonts, icon theme, colours
```

`track=0` in the config turns the recording off.

## Config

`${XDG_CONFIG_HOME:-~/.config}/swov/config`, `key=value`, `#` comments. Every
key is also a command line option:

```sh
swov ui_scale=1.2 hl=ff8800
swov --ssaa=1 --header_pos=top-left
swov --shot /tmp/o.png     # one frame to a PNG, for tuning colours
```

`config.example` lists everything with defaults. The ones worth knowing:

| key | |
| --- | --- |
| `ui_scale` | scales all text at once |
| `ssaa` | supersampling 1–4; drops to 1 by itself on very large screens |
| `bg` | the scrim over the desktop; `0d111700` for none |
| `float_alpha` | how see-through floating and fullscreen windows are |
| `win_gap`, `screen_pad` | space between windows, and around them |
| `header_pos`, `hints_pos` | `none`, or `top`/`bottom` + `left`/`center`/`right` |
| `anim_ms` | tile glide duration; `0` disables |
| `track`, `usage_dots`, `dot_count`, `dot_px` | usage recording and its dot scale |
| `start_selection` | `workspace`, `none` or `window` |
| `cols`, `rows` | force the grid; default picks the largest tiles |

## Notes

- Talks to the sway IPC socket directly: no `swaymsg`, no `jq`, no `/bin/sh`.
- Subscribes to sway's events, so a reload waits for sway to finish a move
  instead of reading a half-applied tree.
- Windows whose `app_id` says nothing (`GTK Application` and friends) are named
  from `/proc/<pid>/cmdline` and the window title.
- Repaints only on change; an idle overlay costs nothing.
- Reordering workspaces renames them, which is all sway offers. Swaps go through
  a temporary name.
- Scratchpad windows are not shown.
- Tabbed and stacked containers are drawn the way sway draws them: a tab strip
  across the top, the visible window's contents below. Each tab is clickable.
- Only the name plate of a floating or fullscreen window takes the mouse; clicks
  on its body go to whatever is beneath it.

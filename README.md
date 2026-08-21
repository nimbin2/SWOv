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
cc -std=c11 -O2 -Wall -Wextra -o swov swov.c $(pkg-config --cflags --libs sdl3 sdl3-image sdl3-ttf)
install -Dm755 swov ~/.local/bin/swov
```

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
| arrows, `hjkl` | move the selection, across tiles too |
| `tab` / `shift+tab` | previous / next workspace |
| `w` | window selection ⇄ whole-workspace selection |
| `enter`, click | focus |
| `space`, right click | mark (marks are what the next action applies to) |
| `a` / `c` | mark all in the workspace / clear marks |
| `x`, `del`, middle click | close |
| `/` | filter by app, title or workspace |
| `r` | reload |
| `esc` | cancel a drag, else quit |

swov opens with the current workspace selected as a whole, no window picked.

## Drag and drop

Press, move, release. `Esc` cancels.

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

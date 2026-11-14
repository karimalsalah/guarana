# Guarana Launcher UI

The web UI of the Guarana Launcher (Tauri 2): first run, game launcher, updater, doctor and problem
reports. Vite 5 + TypeScript, no UI framework. It talks to the Rust backend only through the commands
and events in [`../CONTRACT.md`](../CONTRACT.md), and runs on sample data in a plain browser.

## Run it

```sh
npm install          # no lockfile is committed yet, so CI needs `npm install`, not `npm ci`
npm run dev          # Vite on http://localhost:5173 (the port tauri.conf.json's devUrl should use)
npm run build        # production build into dist/ (Tauri's frontendDist: ../ui/dist)
npm run typecheck    # tsc --noEmit; kept out of `build` so a type slip can't block a Tauri build
npm run preview      # serves dist/
```

Outside Tauri (no `window.__TAURI_INTERNALS__`) the UI loads `src/mock.ts` instead of
`@tauri-apps/api`, so `npm run dev` in any browser shows the full app. A built `dist/` works from any
static server too, for example `python -m http.server -d dist 8080`. Opening `index.html` straight
from disk does not work: the source is TypeScript and needs Vite.

### Sample-data scenarios

Add query parameters to the URL, for example `http://localhost:5173/?mock=ready&pad=xbox`.

| Parameter | Values | Default |
|---|---|---|
| `mock` | `first-run` (Arc 140T laptop, Ocarina of Time on disk, keys missing), `ready` (everything set up), `no-oot`, `failure` (launch, install, update check and report fail) | `first-run` |
| `keys`, `update`, `first` | `0` or `1`: keys imported, update available, first run | from `mock` |
| `pad` | `playstation`, `xbox`, `switch`, `generic`, `none`, `late` (connects after 3.5 s) | `playstation` |
| `crash` | `1`: the game exits with 0xC0000005 right after it starts | off |
| `warm` | `1`: an automatic shader warm-up (as after a GPU driver change) runs 2.5 s after start | off |
| `session` | seconds until `game://exited` | `7` |
| `speed` | multiplies every mock delay (`0.25` is fast, `20` lets you watch each stage) | `1` |
| `screen` | `welcome`, `scan`, `profile`, `home`, `updates`, `doctor`, `report` | from `get_state` |
| `motion` | `reduce`: the reduced-motion version | system setting |

The footer shows a "Sample data" badge whenever the mock is in use.

## Structure

```
index.html            boot splash (plain markup, dark), #app, module entry
public/favicon.svg    brand favicon (copy of assets/brand/favicon.svg)
src/
  main.ts             entry: theme, motion policy, shell, input, controllers, first screen
  api.ts              the only module with command/event names; Tauri or mock backend
  mock.ts             stateful sample backend with progress events (lazy chunk, browser only)
  types.ts            CONTRACT.md types, plus Controller and the proposed Toggle bounds
  state.ts            cached reads (detect_system, list_profiles, get_state, list_games), signals
  router.ts           lazy screens, back stack, tabs, focus and announcements per navigation
  shell.ts            top bar (brand, tabs, theme), main outlet, footer, toasts, live region
  input.ts            intent layer (keyboard + pad), spatial navigation, moving focus halo
  gamepad.ts          Gamepad API polling, per-pad layouts, button glyphs, rumble
  motion.ts           reduced motion, view transitions, pause switch, idle scheduling
  theme.ts            dark by default, light on request, circular reveal
  markdown.ts         minimal safe Markdown -> DOM (no innerHTML of data)
  sessions.ts         last-played times and last applied profile (localStorage)
  format.ts, dom.ts, icons.ts
  ui/                 controls, toast, progress ring, checks, controller card, key art, library list
  screens/            welcome, scan, profile, home, launch, updates, doctor, report (one chunk each)
  styles/             tokens.css, base.css, components.css, screens.css, intro.css (app.css imports them)
```

## Screens and the contract

| Screen | Calls | Listens |
|---|---|---|
| Boot | `get_state` (first run decides Welcome or Home), Tauri `getVersion()` for the footer | `install://progress` while `get_state` installs the bundled build, `game://exited` (safety net), `controller://changed`, `warm://progress` |
| Welcome | nothing; preloads Scan | |
| Scan | `detect_system`, `list_profiles`, `locate_files {scope:"quick"}` (or `"deep"`), `run_doctor`, `import_keys`, `add_game_folder`, `pick_folder`, `set_mode` (setup ends with Auto, no profile step), `apply_fix`, `open_path`, `list_controllers`; preloads Home and Profile | `locate://progress`, `doctor://progress`, `controller://changed` |
| Profile (under Advanced) | `list_profiles`, `apply_profile {profile_id, overrides}` (only values that differ from `Toggle.default`; sets the mode to custom) | |
| Home | `get_mode`, `set_mode` (the Auto / Smooth / Quality / Battery switch), `autopilot_undo`, `autopilot_enable`, `get_booster`, `set_booster_level`, `booster_undo_all` (the Game Booster card), `stop_warm_shaders`, `get_state`, `list_games`, `list_profiles`, `prefetch_caches {title_id}` once per session on idle, `warm_shaders {title_id}` ("Prepare shaders"), `check_updates` once per session | `autopilot://changed`, `booster://changed`, `warm://progress` (Play waits while the title's shaders build, also for the automatic run after a driver change) |
| Launch | `launch_game {title_id, profile_id}` | `game://exited` (registered before launching) |
| Updates | `check_updates {channel}`, `install_version`, `set_current_version` | `install://progress` |
| Doctor | `run_doctor`, `apply_fix`, `ask_ai` (the `install_update` fix shows its `install://progress` on the button) | `doctor://progress`, `install://progress` |
| Report | `ask_ai`, `collect_report {kind, note}`, `open_path`, `open_url` | |

Home has one ember button, Play. Below the hero come the mode card (when keys are imported), the
library, then "This PC": the Game Booster as a folded card that shows its current level, and the
controllers and setup cards. The share prompt, when offered, sits under the hero with a secondary
Share button. First run is Scan and then Home in Auto mode, so the top bar says "Set up" without a
step count; Scan's Continue unlocks once the device and the files are known, and Health finishes in
the background. The launch stage shows the title, one status line with a spinner (launch_game sends
no progress, so there is no bar), then the game's title followed by "is running" on black: the launcher knows the process
started, not that the game has drawn a frame.

Every listener is registered before the command that streams, and removed when its screen goes.
Links never navigate the webview: `https://` links go through `open_url`, nothing else opens.

Argument keys are sent in snake_case, exactly as CONTRACT.md writes them (the Rust commands use
`rename_all = "snake_case"`; `CAMEL_CASE_ARGS` in `src/api.ts` flips it if that ever changes).
Rejections are shown as the sentence the backend sends. The Stable/Nightly switch passes
`{ channel }` to `check_updates`.

`list_profiles` returns the applied profile with the user's saved values merged in, and
`apply_profile` replaces the saved overrides, so Profile's Apply sends every toggle's value and
"Restore defaults" applies the profile without overrides. On a first start `get_state` waits for
the install of the bundled build; its `install://progress` shows under the boot mark.

The mock mirrors the backend: the same profiles and toggles as `src-tauri/src/profiles.rs`, the
contract's check and fix ids in order, and the stage names of every progress event.

## Design tokens

All in `src/styles/tokens.css`. Brand values come from `assets/brand` and `site/assets/site.css`.

| Token | Dark (default) | Light | Use |
|---|---|---|---|
| `--ember-1` / `--ember-2` | `#FFC94A` / `#F2851A` | same | brand gradient (`--grad-ember`): primary buttons, Play, selection |
| `--leaf`, `--echo` | `#3CCF7E`, `#F59E0B` | same | logo leaf, echo arcs |
| `--bg` | `#070B14` night blue | `#F7F5F0` paper | page |
| `--surface` / `-2` / `-3` | `#0E1527` / `#141D33` / `#1B2640` | `#FFF` / `#F4F2ED` / `#EBE7DF` | cards, controls |
| `--text` / `--text-2` / `--muted` | `#EEF1F6` / `#C2C9D6` / `#8F99AD` | `#16181D` / `#434954` / `#5C626D` | text |
| `--accent` | `#FBBF24` | `#A8510A` | links, focus, kickers (the site's accents, 4.5:1 on their backgrounds) |
| `--ok` `--warn` `--fail` `--info` | green, orange, red, violet tints | darker shades | status, always with an icon and a word |

The night stages (Welcome, the game hero, Launch) stay dark in both themes.

- Type: system fonts only (`Segoe UI Variable` on Windows 11), like the site. Sizes `--fs-xs` 12 px to `--fs-3xl` 44 px; headings use the display cut, weight 750-800, tight tracking.
- Space: 4 px grid (`--sp-1` 4 px … `--sp-12` 48 px). Radius: `--r-sm` 8, `--r-md` 14, `--r-lg` 20, `--r-xl` 28, `--r-pill`.
- Motion: `--dur-1/2/3` 150/200/250 ms for micro-interactions, `--dur-screen` 420 ms, `--dur-hero` 700 ms. `--ease-spring` (3 % overshoot) and `--ease-bounce` (12 %) are sampled damped springs in CSS `linear()`, with cubic-bezier fallbacks; `--ease-emph` for large morphs.

## Motion and performance rules

- Everything that loops or covers a large area animates `transform` and `opacity` only: the intro's layers, beam, embers and grain, the key art's echo arcs and glow, screen transitions, skeleton shimmer.
- Nothing decorative loops while the player is only looking. The intro's stars, haze and grain run a few cycles and come to rest on their base frame while Set up waits. The hero's echo arcs pulse three times when Home opens, then rest. The launch stage's art holds still, so the spinner next to the status line is the only thing that moves while a game starts. Spinners, skeletons and progress bars run only while something is loading. Small one-off transitions also touch other properties (the focus halo's size, the segmented thumb's width, the progress ring's stroke, button shadows and colours), and the intro paints three small things once (the drop's path, the streak masks, the wordmark's light sweep). Bloom and grain are pre-made textures; nothing uses a live `filter: blur` or `backdrop-filter`.
- One pause switch: while a game runs or the window is hidden, `html[data-paused]` stops every CSS animation and the pad loop stops; while the window is unfocused, ambient loops (`.ambient`) and pad polling stop.
- Gamepad polling runs only with a pad connected and the window focused, and is suspended from `launch_game` until `game://exited`, so in-game presses never reach the launcher.
- Screens are separate chunks loaded with `import()`; Home is preloaded while Scan runs; the library list and the Markdown renderer load when needed; the mock is its own chunk.
- Screen changes use the View Transitions API: slides for forward and back, a short slide for tabs, the hero card's art and title (or only the pressed library row's title) morph into the full-window launch stage, then the stage dims to black over one second as the game window takes over. The intro's logo flies into the top bar's mark when you press Set up.
- Reduced motion (the system setting or `?motion=reduce`): the intro shows its final frame, transitions are instant, nothing loops.

## Controllers and keyboard

Keyboard and pads feed one intent layer (`src/input.ts`); focus moves by geometry, and a single halo
follows it. The halo shows for keyboard and pad use, never for the mouse. A folded card's heading
(a `<details>` summary, such as Game Booster or Advanced) is in the focus order, and Select opens or
closes it.

| Intent | Keyboard | Xbox | PlayStation | Switch Pro | Generic |
|---|---|---|---|---|---|
| Move | arrows | D-pad / left stick | D-pad / left stick | D-pad / left stick | D-pad / left stick |
| Select | Enter / Space | A | Cross | A (right button) | bottom button |
| Back | Esc | B | Circle | B (bottom button) | right button |
| Switch tabs | Ctrl+Tab / Ctrl+Shift+Tab | LB / RB | L1 / R1 | L / R | L / R |
| Screen's main action | | Menu | Options | + | Start |
| Scroll | | right stick | right stick | right stick | right stick |

The Gamepad API's standard mapping is positional (button 0 is the bottom face button), so Switch
layouts select with the right button, where A sits. Glyphs follow the pad used last (else the first
pad `list_controllers` reports) and are drawn here: letters or shapes in circles, no platform logos.

## Content rules

- No Nintendo artwork, logos or names in imagery. The key art is brand-abstract (the drop, its streaks and echo arcs); game titles appear only as text from the user's own files.
- Numbers come from the site: "Measured on an Arc 140T: Hyrule Field 14.75 → 22.83 game fps, frame generation off".
- The Files card always carries: "Guarana never downloads keys, firmware or games. Use your own, dumped from your console." Nothing is imported until the user presses Import or Add folder.
- Diagnostics copied from Doctor are scrubbed (`C:\Users\<name>` becomes `%USERPROFILE%`); reports are built and scrubbed by the backend and only leave the PC when the user submits the GitHub issue.

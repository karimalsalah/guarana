# Guarana Launcher UI

The web UI of the Guarana Launcher (Tauri 2): first run, game launcher, updater, doctor and problem
reports. Vite 8 + TypeScript, no UI framework. It talks to the Rust backend only through the commands
and events in [`../CONTRACT.md`](../CONTRACT.md), and runs on sample data in a plain browser.

## Run it

```sh
npm ci               # package-lock.json is committed; launcher's `ui:install` runs `npm ci` too
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
| `mock` | `first-run` (Arc 140T laptop, Ocarina of Time on disk, keys missing), `ready` (everything set up), `no-oot`, `failure` (launch, install, update check and report fail), `contaminated` (`ready`, and the doctor's settings-hygiene rows find something) | `first-run` |
| `keys`, `update`, `first` | `0` or `1`: keys imported, update available, first run | from `mock` |
| `pad` | `playstation`, `xbox`, `switch`, `generic`, `none`, `late` (connects after 3.5 s) | `playstation` |
| `crash` | `1`: the game exits with 0xC0000005 right after it starts | off |
| `warm` | `1`: an automatic shader warm-up (as after a GPU driver change) runs 2.5 s after start | off |
| `session` | seconds the game runs once it is on screen, then `game://exited` | `7` |
| `boot` | the `game://boot` events a launch plays: `session` (a 20.3+ emulator: phases, shader counts, its window taking the foreground, then the game shown after about 5 s), `old` (only its window taking the foreground), `none` (nothing: the stage hands over after 20 s) | `session` |
| `share` | `0` or `1`: a finished session's numbers wait in the "Share my numbers" offer (`0`: the sample PC never finished a session, and numbers the UI kept from an earlier visit are forgotten). Each clean exit of a sample game offers a new session | `1`, `0` for `first-run` |
| `speed` | multiplies every mock delay (`0.25` is fast, `20` lets you watch each stage) | `1` |
| `screen` | `welcome`, `scan`, `profile`, `home`, `updates`, `doctor`, `report` | from `get_state` |
| `motion` | `reduce`: the reduced-motion version | system setting |

The hint bar shows a "Sample data" badge whenever the mock is in use.

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
  shell.ts            rail (the seed's mark, tabs, theme), main outlet, hint bar, toasts, live region
  input.ts            intent layer (keyboard + pad), spatial navigation, liquid focus halo and comet tail
  gamepad.ts          Gamepad API polling, per-pad layouts, button glyphs, rumble
  motion.ts           reduced motion, view transitions, pause switch, idle scheduling
  theme.ts            dark by default, light on request, circular reveal
  markdown.ts         minimal safe Markdown -> DOM (no innerHTML of data)
  sessions.ts         last-played times and last applied profile (localStorage)
  format.ts, dom.ts, icons.ts
  ui/                 controls, toast, progress ring, checks, controller card, the seed (and its launch orbit), the session's numbers (rolling numerals, pacing strip, readouts), library list
  screens/            welcome, scan, profile, home, launch, updates, doctor, report (one chunk each)
  styles/             tokens.css, base.css, components.css, seed.css, numbers.css, screens.css, intro.css (app.css imports them)
```

## Screens and the contract

| Screen | Calls | Listens |
|---|---|---|
| Boot | `get_state` (first run decides Welcome or Home), Tauri `getVersion()` for the footer | `install://progress` while `get_state` installs the bundled build, `game://exited` (safety net), `controller://changed`, `warm://progress` |
| Welcome | nothing; preloads Scan | |
| Scan | `detect_system`, `list_profiles`, `locate_files {scope:"quick"}` (or `"deep"`), `run_doctor`, `import_keys`, `add_game_folder`, `pick_folder`, `set_mode` (setup ends with Auto, no profile step), `apply_fix`, `open_path`, `list_controllers`; preloads Home and Profile | `locate://progress`, `doctor://progress`, `controller://changed` |
| Profile (under Advanced) | `list_profiles`, `apply_profile {profile_id, overrides}` (only values that differ from `Toggle.default`; sets the mode to custom) | |
| Home | `get_mode`, `set_mode` (the Auto / Smooth / Quality / Battery switch), `autopilot_undo`, `autopilot_enable`, `get_booster`, `set_booster_level`, `booster_undo_all` (the Game Booster card), `stop_warm_shaders`, `get_state`, `list_games`, `list_profiles`, `prefetch_caches {title_id}` once per session on idle, `warm_shaders {title_id}` ("Prepare shaders"), `check_updates` once per session | `autopilot://changed`, `booster://changed`, `warm://progress` (Play waits while the title's shaders build, also for the automatic run after a driver change) |
| Launch | `launch_game {title_id, profile_id}` | `game://exited` and `game://boot` (both registered before launching; the stage shows the boot phase and the real shader count, and holds until the game is on screen, see CONTRACT.md "Boot progress and the hand-over") |
| Updates | `check_updates {channel}`, `install_version`, `set_current_version` | `install://progress` |
| Doctor | `run_doctor`, `apply_fix`, `ask_ai` (the `install_update` fix shows its `install://progress` on the button) | `doctor://progress`, `install://progress` |
| Report | `ask_ai`, `collect_report {kind, note}`, `open_path`, `open_url` | |

Home is a full-bleed stage: the seed on the right, the game's title, Play (the one ember button)
and Prepare shaders on the left, with the mode switch (when keys are imported) under them. Under the
seed sit the last measured session's numbers ("Your last measured session"): frames on time as a large
rolling numeral, the frame-pacing strip (one stem per frame, modelled from the session's percentiles
and captioned so; late stems are red and drawn taller), and game fps, frame time (median, with the 95th
percentile), shown fps and when it was measured (the session's own date and length of play). The
"Last played" line by the title stays: it is the launcher's own record and can be newer.
Their only source is the "Share my numbers" data (`get_share_status`, `share://changed`); the latest per
game is kept in `localStorage` (`sessions.ts`) because the backend drops the offer after Share or Not
now, and it is shown with its date. The seed wears the frames-on-time share as one leaf arc around the
fruit (the session ring). With no finished session the seed stands alone with one line that says the
numbers appear after the first session (or, with the share prompt off, how to get them back). The seed
stays closed until the keys are in, half-opens while the library loads or when Ocarina is missing,
opens when the game is ready, and shuts on an error. The share prompt, when offered, is a quiet
line along the bottom of the stage with a secondary Share button. Below the stage come the rest of
the library (a typographic list) and "This PC": the Game Booster as a folded pane that shows its
current level, and the controllers and setup panes. First run is Scan and then Home in Auto mode,
so the rail says "Set up" without a step count; Scan's Continue unlocks once the device and the
files are known, and Health finishes in the background. On Play the interface folds into the seed
and the seed flies to the centre of the launch stage. Each boot phase from `game://boot` blooms as
a ring around it (start, load, shaders, the game), and while shaders load their real count settles
onto the shader ring as sparks and rolls as a number beside the rings, with the time since Play (it
settles in leaf when the shaders are in, and the clock stops at how long they took); one status line says the phase in words, with the count after it,
and a row of phase names under it says the same in text. It holds until the game is on screen; then
the rings fall into the seed, the iris opens to black, and the stage shows the game's title followed
by "is running". When it hands over after a hold time without news, the launcher knows only that
the process started, not that the game has drawn a frame.

Every listener is registered before the command that streams, and removed when its screen goes.
Links never navigate the webview: `https://` links go through `open_url`, nothing else opens.

Argument keys are sent in snake_case, exactly as CONTRACT.md writes them (the Rust commands use
`rename_all = "snake_case"`; `CAMEL_CASE_ARGS` in `src/api.ts` flips it if that ever changes).
Rejections are shown as the sentence the backend sends. The Stable/Nightly switch passes
`{ channel }` to `check_updates`.

`list_profiles` returns the applied profile with the user's saved values merged in, and
`apply_profile` replaces the saved overrides, so Profile's Apply sends only the toggles that differ
from `Toggle.default` and "Restore defaults" applies the profile without overrides. On a first
start `get_state` waits for the install of the bundled build; its `install://progress` shows under
the boot mark.

The mock mirrors the backend: the same profiles and toggles as `src-tauri/src/profiles.rs`, the
contract's check and fix ids in order, and the stage names of every progress event.

## Design tokens

All in `src/styles/tokens.css`. Brand values come from `assets/brand` and `site/assets/site.css`.
The direction is "the living seed": a ripe guarana fruit split open looks like an eye (red shell,
white flesh, black seed), and that seed (`src/ui/seed.ts`) is the heart of the interface. Dark is
"Rainforest night", light is "Aril" (the fruit's white flesh).

| Token | Dark (default) | Light | Use |
|---|---|---|---|
| `--ember-1` / `--ember-2` | `#FFC94A` / `#F2851A` | same | brand gradient (`--grad-ember`): primary buttons, Play, the focus halo, progress |
| `--leaf`, `--leaf-ink` | `#3CCF7E`, `#6FE3A0` | `#3CCF7E`, `#0B6B39` | the one green accent: what is on, chosen or done (rail marker, switch, segmented drop, settled rings) |
| `--shell`, `--flesh`, `--seed`, `--echo` | `#D8321F`, `#F6F1E6`, `#060504`, `#F59E0B` | same | the seed's art, echo arcs |
| `--bg` | `#000000` true OLED black | `#F5F0E6` aril | page |
| `--surface` / `-2` / `-3` | `#06120C` / `#0A1B12` / `#10271A` canopy greens | `#FBF8F1` / `#EFE9DC` / `#E6DFCF` | panes, controls |
| `--text` / `--text-2` / `--muted` | `#F3EFE6` / `#C8C9BB` / `#8E9C90` | `#0F1A12` / `#3B4A3F` / `#546155` | text (all at least 4.9:1 on every surface) |
| `--accent` | `#FFBF5A` | `#974407` | links, kickers |
| `--halo` | `#FFB547` | `#B4530A` | focus (at least 3.7:1 on every surface) |
| `--ok` `--warn` `--fail` `--info` | leaf, amber, shell-red, aril tints | darker shades | status, always with an icon and a word |

The night stages (Welcome, Launch) stay dark in both themes; Home's stage follows the theme.

- Shape: the leaf. Two opposite corners round, two nearly sharp (`--r-leaf-sm` 14/4, `--r-leaf` 24/7, `--r-leaf-lg` 34/10) on buttons, panes, fields and toasts. The focus halo copies each corner of what it surrounds.
- Type: system fonts only (`Segoe UI Variable` on Windows 11), like the site. Sizes `--fs-xs` 12 px to `--fs-3xl` 44 px and `--fs-stage` for the game's title; headings use the display cut, weight 700, tight tracking. Kickers (`.eyebrow`) start with a tiny seed.
- Space: 4 px grid (`--sp-1` 4 px … `--sp-12` 48 px). Chrome: `--rail-w` 84 px, `--hint-h` 38 px.
- Motion: `--dur-1/2/3` 140/200/260 ms for micro-interactions, `--dur-screen` 380 ms, `--dur-hero` 760 ms, `--dur-fold` 560 ms, `--dur-iris` 760 ms. `--ease-spring` (3 % overshoot), `--ease-bounce` (12 %) and `--ease-jelly` (25 %) are sampled damped springs in CSS `linear()`, with cubic-bezier fallbacks; `--ease-fold` for things being swallowed, `--ease-bloom` for things opening, `--ease-emph` for large morphs.

## Motion and performance rules

- Everything that loops or covers a large area animates `transform` and `opacity` only: the seed's breath, the orbit's spin, the intro's canopy layers and fireflies, screen transitions, the launch fold and the iris, skeleton shimmer.
- The seed breathes and blinks only on Home's stage and the intro, stepped to 24 fps (`steps()` on a composited transform). The breath is on `.ambient` so it stops while the window is unfocused, and is stopped by `html[data-paused]`. The blink is not a loop: a timer starts one 312 ms blink about every ten seconds, re-reads focus each time (a webview does not always get `blur`), and is not set at all while the window is unfocused, hidden or paused, or the seed is asleep. After 45 s without input it dozes off (half-closed, no breath): nothing moves at all on an idle Home.
- The session's numbers roll by `translateY` on each digit's reel (a transition per change; none while paused or under reduced motion), the strip's stems grow in once by `scaleY`, and the session ring draws in once (a transition of its dash). The launch's elapsed time is one text update a second, only while the count shows and nothing is paused. Its gaze follows focus (keyboard and pad) or the pointer (mouse) through transitions fired by those events, never a loop.
- The focus halo is liquid ember: it springs to the next control, stretches along its path (a 460 ms one-off scale on the ring), and a comet tail streaks from where it was (one reused element, 340 ms). Both are skipped under reduced motion and hidden for the mouse.
- The launch orbit spins only while a game starts and only before the emulator's window takes the foreground (`game://boot` "window" sets the pause switch). The shader count lights ticks through short transitions as events arrive, and up to four sparks fly per update (skipped while paused). The iris is one black disc scaled from the pupil past the corners: a transition, so it completes even after the pause switch flips.
- Nothing decorative loops while the player is only looking: the intro's haze and fireflies drift once and rest, the echo arcs slide out only while Play has focus or the pointer, spinners, skeletons and progress bars run only while something is loading. Small one-off transitions also touch other properties (the halo's size and corners, the segmented drop's width, the progress ring's stroke, button shadows and colours), and the intro paints the seed's leaf once. Glows are pre-made gradients; nothing uses a live `filter` or `backdrop-filter`.
- One pause switch: while a game runs or the window is hidden, `html[data-paused]` stops every CSS animation and the pad loop stops; while the window is unfocused, ambient loops (`.ambient`) and pad polling stop.
- Gamepad polling runs only with a pad connected and the window focused, and is suspended from `launch_game` until `game://exited`, so in-game presses never reach the launcher.
- Screens are separate chunks loaded with `import()`; Home is preloaded while Scan runs; the library list and the Markdown renderer load when needed; the mock is its own chunk.
- Screen changes use the View Transitions API: forward and back grow in from the side, tabs (on a vertical rail) lift the next screen from below or above. On Play the old screen spirals down into the seed's eye while the rail and the hint bar slip away, the seed flies to the launch stage and the title follows (from a library row: the seed if it is on screen, and the row's title). Back from a game the stage rises out of the black and the seed wakes up. The theme switch is one composited cross-fade. The intro's seed flies into the rail's mark when you press Set up.
- Reduced motion (the system setting or `?motion=reduce`): the intro shows its final frame, the seed neither breathes nor blinks, the orbit does not spin, the halo has no tail or stretch, transitions and the iris are instant, nothing loops.
- Forced colours (Windows contrast themes): the halo and tail are hidden and a real `Highlight` outline shows focus.

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

- No Nintendo artwork, logos or names in imagery. The seed and the intro's canopy are procedural and brand-abstract (the drop with its streaks, leaf and echo arcs, split open like a guarana fruit; leaves drawn from a seeded random); game titles appear only as text from the user's own files.
- Numbers come from the site: "Measured on an Arc 140T: Hyrule Field 14.75 → 22.83 game fps, frame generation off".
- The Files card always carries: "Guarana never downloads keys, firmware or games. Use your own, dumped from your console." Nothing is imported until the user presses Import or Add folder.
- Diagnostics copied from Doctor are scrubbed (`C:\Users\<name>` becomes `%USERPROFILE%`); reports are built and scrubbed by the backend and only leave the PC when the user submits the GitHub issue.

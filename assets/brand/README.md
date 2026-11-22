# Guarana brand assets

Original artwork for Guarana. Nothing here is derived from Nintendo, upstream Lichii or any other project's art.

| File | What it is | Size |
| --- | --- | --- |
| `logo-mark.svg` | The mark alone, transparent background | 64 x 64 viewBox |
| `logo.svg` | Mark plus the `Guarana` wordmark (live text) | 320 x 64 viewBox |
| `favicon.svg` | The mark on a dark rounded tile, wider streak gaps for 16 px | 64 x 64 viewBox |
| `social-preview.svg` | Open Graph / Twitter card and GitHub social preview | 1280 x 640 |

The website serves copies of `favicon.svg` and `logo-mark.svg` from `site/assets/` (GitHub Pages publishes only `site/`). Edit the masters here and copy them over; the two pairs must stay identical.

## The idea

- **A fast drop**: the real frame, the one the game rendered. Round like a fruit, a nod to the fruit names in the project's lineage, without reusing anyone's artwork.
- **Three speed streaks** cut into its tail at equal spacing: motion, and evenly paced frames.
- **A leaf** on top: the fruit.
- **An echo arc** ahead of the drop: the predicted frame that frame generation extrapolates forward.

Rules: no letterforms in the mark, no shapes that read as a console or controller, and never a red and blue pair.

## Palette

| Token | Value | Use |
| --- | --- | --- |
| Ember light | `#FFC94A` | Gradient start (top left of the drop) |
| Ember deep | `#F2851A` | Gradient end |
| Echo | `#F59E0B` at 60 % opacity | Echo arc |
| Leaf | `#2FBF71` (on dark tiles `#3CCF7E`) | Leaf |
| Ink | `#14171D` / `#0F1218` | Favicon tile, social background |
| Paper | `#FBFAF7` | Light page background |
| Site accent | `#A8510A` light, `#FBBF24` dark | Links and buttons on the site (meets 4.5:1 on its background) |

Keep clear space around the mark of at least a quarter of its height. Below 24 px use `favicon.svg`, whose streak gaps survive downscaling.

## Placeholders

Text in these files and in `site/` uses three placeholders, substituted before publishing:

| Placeholder | Meaning | Example |
| --- | --- | --- |
| `Guarana` | Display name | `Example` |
| `guarana` | Repository slug, lowercase | `example` |
| `karimalsalah` | GitHub owner | `karimalsalah` |

The commands below skip this README, and build the tokens from pieces so that a repository-wide replace can't break them. Run them from the repository root.

PowerShell (case-sensitive on purpose; a hashtable would not work, its keys ignore case):

```powershell
$o = '{' * 2; $c = '}' * 2
$pairs = @(@("${o}NAME$c", 'Example'), @("${o}name$c", 'example'), @("${o}OWNER$c", 'karimalsalah'))
Get-ChildItem site, assets/brand -Recurse -File -Include *.html, *.css, *.js, *.json, *.txt, *.xml, *.svg -Exclude README.md | ForEach-Object {
  $text = [IO.File]::ReadAllText($_.FullName); $new = $text
  foreach ($p in $pairs) { $new = $new.Replace($p[0], $p[1]) }
  if ($new -ne $text) { [IO.File]::WriteAllText($_.FullName, $new) }   # writes UTF-8 without BOM
}
```

bash:

```sh
o='{{'; c='}}'
grep -rlE --exclude=README.md '\{\{(NAME|name|OWNER)\}\}' site assets/brand |
  xargs sed -i -e "s/${o}NAME${c}/Example/g" -e "s/${o}name${c}/example/g" -e "s/${o}OWNER${c}/karimalsalah/g"
```

Then check that nothing is left: `grep -rn --exclude=README.md '{{' site assets/brand` should print nothing.

## After substituting the name

1. **Fit `logo.svg`.** The wordmark is live text, so the 320-wide viewBox may be too wide or too narrow for the real name. Open it in Inkscape, select all, *Edit > Resize Page to Selection*, add a little margin. For a final logo that doesn't depend on installed fonts, also run *Path > Object to Path* on the text.
2. **Check `social-preview.svg`.** The name is set at 104 px starting at x = 420; a name longer than about 12 characters needs a smaller font size there.
3. **Export the PNGs** (below). Social networks and GitHub don't accept SVG for previews.

## Exporting PNGs

Every page's `og:image` and `twitter:image` point at `site/assets/social-preview.png`, which does not exist until you export it. Export at exactly 1280 x 640.

Inkscape 1.x:

```sh
inkscape assets/brand/social-preview.svg --export-type=png --export-filename=site/assets/social-preview.png -w 1280 -h 640
```

librsvg:

```sh
rsvg-convert -w 1280 -h 640 assets/brand/social-preview.svg -o site/assets/social-preview.png
```

Microsoft Edge or Chrome, headless (uses the browser's fonts; on Windows, `msedge` is in `C:\Program Files (x86)\Microsoft\Edge\Application\`):

```powershell
msedge --headless=new --disable-gpu --hide-scrollbars --window-size=1280,640 --screenshot="$PWD\site\assets\social-preview.png" "file:///$($PWD -replace '\\','/')/assets/brand/social-preview.svg"
```

Optional extras, same tools:

- `apple-touch-icon.png`, 180 x 180, from `favicon.svg` (iOS home screen). Add `<link rel="apple-touch-icon" href="assets/apple-touch-icon.png">` to the pages if you ship it.
- `logo-mark-512.png`, 512 x 512, from `logo-mark.svg`, for places that want a square raster avatar.

Look at the result before publishing: text set in a fallback font can shift, and the browser export reflects your system's light or dark setting only in `logo.svg` (below).

## Light and dark

`logo.svg` switches its wordmark from ink to near-white with `@media (prefers-color-scheme: dark)`. In an `<img>`, that media query follows the viewer's system or browser setting, not the colour of the page around it, so on a light page viewed with a dark system theme the wordmark can turn pale. Where that matters (a fixed light or dark background), delete the `@media` rule and set the fill you need, or use the mark alone. `logo-mark.svg`, `favicon.svg` and `social-preview.svg` use fixed colours that work on both.

## GitHub social preview

Repository *Settings > General > Social preview > Edit > Upload an image*, and pick the exported `social-preview.png`. GitHub recommends 1280 x 640 and keeps a margin around the edge; the artwork keeps its content 80 px inside.

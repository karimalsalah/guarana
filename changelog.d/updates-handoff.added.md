- Help > Check for Updates in the emulator (Windows) opens the Guarana launcher's Updates screen,
  which checks for an update and installs it, so there is one updater, the launcher's
  ([#44](https://github.com/karimalsalah/guarana/issues/44)). The menu item goes through a new
  `guarana://updates` link (optionally `?channel=stable` or `?channel=nightly`), which only shows
  the screen and never installs by itself. Without the launcher installed it opens the releases
  page.

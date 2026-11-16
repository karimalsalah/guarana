- `EDEN_NX2_HOT_PAGES` (experimental, off): guest pages the CPU rewrites every frame stay
  unprotected and are uploaded at most once a frame, instead of being write-protected after each
  upload and faulting on the next write. Ported from Eglantine (`f27522bfb9`), where Ocarina of
  Time's frame went from 25.9 to 24.7 ms.

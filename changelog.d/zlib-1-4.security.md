- Windows: zlib 1.4, up from 1.3.2. It fixes an overflow in non-blocking `gzwrite()`
  (CVE-2026-85091), an `inflateGetDictionary()` write past the caller's buffer present since
  1.2.8, and other inflate, deflate and minizip memory-safety bugs. Linux and macOS builds use the
  system's zlib.

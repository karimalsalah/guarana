- Build: MSVC can fill a solid NCZ prefix and search storage-buffer addresses,
  and GCC can compile the query-report lambda and the tests that shadowed
  Catch2, dynarmic, and ring-buffer names (Lichii #1768, #1763, #1762, #1761, #1760).
- Fire Emblem 1.1.1's captured prepass opens the same replacement chain as the
  original prepass (Lichii #1752).
- Dump RomFS reads a loose .nup from its loader instead of looking for an
  installed NCA (Lichii #1764).

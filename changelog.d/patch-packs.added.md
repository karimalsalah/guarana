- Built-in patches can be data: a patch pack (`dist/patch-packs/*.json`, see
  [docs/patch-packs.md](docs/patch-packs.md)) adds built-in add-ons for any game without C++.
  They are checked as the compiled-in ones are: build id, the game's original bytes (by
  SHA-256), code caves in executable padding, zero-filled state, user mods first. Ocarina of
  Time's three Text Speed add-ons are the first pack and work as before. The packs ship in a
  `patch-packs` folder next to the program and are compiled into it as well, so Android and a
  program without the folder keep them; a file in the folder replaces the compiled-in copy of its
  name. The unit tests reject a pack with a field the format does not have, a patch without a
  description, or bytes another built-in writes.

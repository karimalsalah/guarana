- Upstream: merged zortos293/Lichii `nx2` from `7c7f723b68` to `0a15f2ce07` (9 commits).
  Images are not scaled past the device's image and framebuffer limits (Lichii #1890, which
  we carried; now upstream's); a staging buffer without memory no longer submits in the middle
  of a draw (Lichii #1997); a present fenced by a service thread is composited on the scheduler
  owner, in order (Lichii #1996); Switch 1 add-ons show on Switch 2 Edition titles in the game
  list (Lichii #2001); keyblob CMACs and NCZBLOCK section tables are checked (Lichii #1966,
  #1892). With Nextendo online play on: friends, friend requests and presence (Lichii #1983),
  and an opt-in sync of the account's nickname and picture to the current user (Lichii #1990).

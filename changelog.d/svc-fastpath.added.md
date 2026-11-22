- Kernel: `EDEN_NX2_SVC_FASTPATH=1` (off by default) lets a supervisor call that
  leaves its thread runnable continue the JIT loop in place instead of
  round-tripping the scheduler, the top measured JIT/HLE exit cost on the
  Ocarina of Time Switch 2 remake (~2084 SVC exits/frame). Blocking, yielding,
  terminating and migrating calls still schedule as before.
- Ocarina FSR: on AMD the jitter offset stays non-negative so the integer viewport
  scissor does not drop the last column and row (Lichii #1744).
- NVIDIA: routed global memory picks its storage buffer with OpSelect instead of
  one branch per buffer, which hung the driver (Lichii #1757, #619). Other
  drivers keep the per-buffer chain.

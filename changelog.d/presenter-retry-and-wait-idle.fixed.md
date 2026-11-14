- Vulkan: avoid submitting a frame twice after a failed present, and exclude queue activity
  during the blit screen's device idle wait, including pending copy-ahead presents (#136).

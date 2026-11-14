- Vulkan: drain background GPL optimization builds before the teardown driver pipeline cache save
  in `~PipelineCache`, and add unit-tested shared/exclusive gating, reload drain, and unchanged
  save skipping behind opt-in `EDEN_NX2_CACHE_SAFE=1` on Intel Windows drivers
  ([#7](https://github.com/karimalsalah/guarana/issues/7)).

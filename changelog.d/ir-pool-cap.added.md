- `EDEN_NX2_IR_POOL_CAP_MIB` (experimental, off): the shader recompiler's object pools free
  their memory after a shader once it passes the cap instead of keeping their largest shader's
  worth, which can return host RAM an integrated GPU shares. Ported from Fritchii (`c99790b878`).

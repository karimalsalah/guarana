# GPU trace corpus

GPU traces (`.lxtrace`, see `docs/nx2/gpu-trace.md`) replay a scene without the game, so they make regression tests for rendering. Traces from games hold the game's shaders, textures and buffers, so they never go into this repository or a public place. The repository keeps only what checks them:

The `draws` and `motion` rows come from the sample's `--gpu-trace-sample-draws` and `--gpu-trace-sample-motion` modes; `draws-render-follower` and `draws-unlend` are the draws trace with `EDEN_NX2_RENDER_FOLLOWER=1` and `EDEN_NX2_RENDER_UNLEND=1`, which must leave its frames bit-identical; `sample-no-presents` is the sample recorded without its presents, which the replay must refuse.
- `expected/<id>/`: when a frame's look matters for review, a small PNG of it (only frames whose content may be public).

Check the corpus (CI runs this on lavapipe with the generated traces):

```sh
python3 -I tools/nx2/trace_replay.py check --cli build/bin/guarana-cli \
  --corpus tests/gpu_traces/corpus.json --traces <directory with the private traces>
```

Compare two builds on one trace (an HTML page with the frames side by side, and the largest difference, mean difference and PSNR of every frame that differs):

```sh
python3 -I tools/nx2/trace_replay.py compare --base old/guarana-cli --head new/guarana-cli trace.lxtrace --out cmp
```

To add a trace: record it at 1x resolution, put it in the private store, replay it on lavapipe with `trace_replay.py replay` and add its entry with the hashes it printed. Hashes are only comparable on the same device class.

# NPU benchmark

Measures what a laptop's neural processor (NPU) can do inside a game frame, next to the iGPU and the CPU, with
OpenVINO. Guarana's first run on a Core Ultra 9 285H (Intel AI Boost) is in [ROADMAP.md](../../ROADMAP.md#a1-npu-feasibility):
the iGPU was about twice as fast as the NPU for per-frame image work, and moving a 1440p frame to the NPU and back
took about 30 ms. Newer NPUs (Lunar Lake, Panther Lake, AMD Ryzen AI) may change that verdict: **run it on yours and
send the numbers.**

## Run it

```powershell
python -m venv npu-venv
npu-venv\Scripts\pip install openvino numpy
npu-venv\Scripts\python tools\npu\npu_bench.py > npu-results.jsonl
```

It builds small networks with random weights (no models are downloaded): super resolution 720p → 1440p and
540p → 1080p, an image-to-image network at 1440p, and a tiny policy network, then times each on every device
OpenVINO finds (`NPU`, `GPU`, `CPU`). Each line of the output is one workload and device with its p50 and p99 latency.

## Send the numbers

Open a [device report](https://github.com/karimalsalah/guarana/issues/new?template=device-report.yml), name your
CPU and NPU driver version, and paste `npu-results.jsonl`. Questions: [Discord](https://discord.gg/urz7vMdRM).

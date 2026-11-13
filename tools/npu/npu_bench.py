# SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""What an NPU can do inside a game frame: latency of small networks on the NPU, iGPU and CPU (OpenVINO).

Without arguments it runs the standard matrix, one subprocess per case, and prints one JSON line per
workload and device. With arguments (kind dev height width extra iterations) it runs one case."""
import sys, time, json, numpy as np, openvino as ov
from openvino import opset13 as ops
rng = np.random.default_rng(0)

def conv(x, cin, cout, k, relu=True, int8=False):
    w = (rng.standard_normal((cout, cin, k, k)) * (2.0/(cin*k*k))**0.5).astype(np.float32)
    if int8:
        # weights int8 symmetric via FakeQuantize on const
        m = float(np.abs(w).max())
        wc = ops.constant(w)
        w = ops.fake_quantize(wc, ops.constant(-m), ops.constant(m), ops.constant(-m), ops.constant(m), 255)
    else:
        w = ops.constant(w)
    y = ops.convolution(x, w, [1,1], [k//2]*2, [k//2]*2, [1,1])
    b = ops.constant(np.zeros((1,cout,1,1),np.float32))
    y = ops.add(y, b)
    if relu: y = ops.relu(y)
    if int8:
        y = ops.fake_quantize(y, ops.constant(0.0 if relu else -8.0), ops.constant(8.0), ops.constant(0.0 if relu else -8.0), ops.constant(8.0), 256)
    return y

def sr(h, w, int8=False):
    x = ops.parameter([1,3,h,w], np.float32, "x")
    y = conv(x,3,56,5,int8=int8); y = conv(y,56,16,1,int8=int8)
    for _ in range(4): y = conv(y,16,16,3,int8=int8)
    y = conv(y,16,56,1,int8=int8); y = conv(y,56,12,3,relu=False,int8=int8)
    y = ops.depth_to_space(y, "blocks_first", 2)
    return ov.Model([y],[x],"sr")

def aa(h, w, ch, int8=False):
    x = ops.parameter([1,3,h,w], np.float32, "x")
    y = conv(x,3,ch,3,int8=int8)
    for _ in range(2): y = conv(y,ch,ch,3,int8=int8)
    y = conv(y,ch,3,3,relu=False,int8=int8)
    return ov.Model([ops.add(y,x)],[x],"aa")

def mlp():
    x = ops.parameter([1,64], np.float32, "x")
    y = x
    for i,(a,b) in enumerate([(64,64),(64,64),(64,8)]):
        y = ops.matmul(y, ops.constant((rng.standard_normal((a,b))*0.1).astype(np.float32)), False, False)
        if i<2: y = ops.relu(y)
    return ov.Model([y],[x],"mlp")

def ident(h, w):
    x = ops.parameter([1,4,h,w], np.float16, "x")
    wt = np.eye(4,dtype=np.float16).reshape(4,4,1,1)
    y = ops.convolution(x, ops.constant(wt), [1,1],[0,0],[0,0],[1,1])
    return ov.Model([y],[x],"ident")

def pct(a,p): return float(np.percentile(a,p))

def run(kind, dev, h, w, extra, n):
    core = ov.Core()
    int8 = (extra == "int8")
    if kind=="sr": m = sr(h,w,int8)
    elif kind=="aa": m = aa(h,w,int(extra.split("c")[1]) if extra.startswith("c") else 24, False)
    elif kind=="aa8": m = aa(h,w,24,True)
    elif kind=="mlp": m = mlp()
    elif kind=="ident": m = ident(h,w)
    cfg = {"PERFORMANCE_HINT":"LATENCY"}
    if dev in ("GPU","NPU") and kind!="ident": cfg["INFERENCE_PRECISION_HINT"]=ov.Type.f16
    if dev=="CPU": cfg["INFERENCE_PRECISION_HINT"]=ov.Type.f32
    t=time.perf_counter(); cm = core.compile_model(m, dev, cfg); tc=time.perf_counter()-t
    ir = cm.create_infer_request()
    inp = cm.input(0)
    dt = np.float16 if kind=="ident" else np.float32
    data = rng.random(list(inp.shape)).astype(dt)
    t=time.perf_counter(); ir.infer({0:data}); tfirst=time.perf_counter()-t
    for _ in range(5): ir.infer({0:data})
    lat=[]
    for _ in range(n):
        t=time.perf_counter(); ir.infer({0:data}); lat.append((time.perf_counter()-t)*1000)
    # async throughput, 4 requests
    q = ov.AsyncInferQueue(cm, 4); N=max(20,n//2)
    t=time.perf_counter()
    for _ in range(N): q.start_async({0:data})
    q.wait_all(); thr = N/(time.perf_counter()-t)
    r = dict(kind=kind,dev=dev,h=h,w=w,extra=extra,compile_s=round(tc,3),first_infer_ms=round(tfirst*1000,2),
             p50=round(pct(lat,50),3),p99=round(pct(lat,99),3),mean=round(float(np.mean(lat)),3),fps_async=round(thr,1))
    if kind=="ident" and dev=="NPU":
        # zero-copy host tensor attempt
        try:
            ctx = ov.Core().get_default_context("NPU")
            ti = ctx.create_host_tensor(ov.Type.f16, ov.Shape(list(inp.shape)))
            to = ctx.create_host_tensor(ov.Type.f16, ov.Shape(list(cm.output(0).shape)))
            ti.data[...] = data
            ir.set_tensor(cm.input(0), ti); ir.set_tensor(cm.output(0), to)
            for _ in range(5): ir.infer()
            l2=[]
            for _ in range(n):
                t=time.perf_counter(); ir.infer(); l2.append((time.perf_counter()-t)*1000)
            r["hosttensor_p50"]=round(pct(l2,50),3); r["hosttensor_p99"]=round(pct(l2,99),3)
        except Exception as e:
            r["hosttensor_err"]=str(e)[:200]
    return r

MATRIX = [("sr", 720, 1280, "fp16", 50), ("sr", 540, 960, "fp16", 50), ("aa", 1440, 2560, "c16", 30),
          ("mlp", 1, 1, "fp16", 500)]

def main_matrix():
    import subprocess
    for device in ov.Core().available_devices:
        dev = device.split(".")[0]
        for kind, h, w, extra, n in MATRIX:
            out = subprocess.run([sys.executable, "-I", __file__, kind, dev, str(h), str(w), extra, str(n)],
                                 capture_output=True, text=True, timeout=900).stdout
            for line in out.splitlines():
                if line.startswith("RESULT "):
                    print(line[7:], flush=True)

if __name__=="__main__":
    if len(sys.argv) < 7:
        main_matrix()
        sys.exit(0)
    kind,dev,h,w,extra,n = sys.argv[1],sys.argv[2],int(sys.argv[3]),int(sys.argv[4]),sys.argv[5],int(sys.argv[6])
    try: print("RESULT "+json.dumps(run(kind,dev,h,w,extra,n)))
    except Exception as e: print("RESULT "+json.dumps(dict(kind=kind,dev=dev,h=h,w=w,extra=extra,error=str(e)[:300])))

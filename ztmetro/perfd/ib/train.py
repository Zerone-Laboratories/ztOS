#!/usr/bin/env python3
"""Train ztOSIntelligentBoost (V2) from zrnperfd's log (perfd/iblog.c).

    train.py LOG.csv [LOG.csv ...] -o model.bin      train on real logs
    train.py --synthetic -o model.bin                 a made-up log, to test the pipeline

The label of each sample is the highest rung the ladder (RapidBoost) itself
reached in the next two seconds. A model that predicts it is there when the
ladder would get there, without the ladder's lag (its hold times: 1-2 s of
load before each step up), and never higher than the ladder went: the
efficiency of the ladder, the response of a predictor.

(The first label tried, the rung the next two seconds of *load* asked for by
the ladder's marks, said rapid 45 % of the time where the ladder ran it 2.4 %:
Android's load spreads over many cores and passes a core's 0.95 easily.)

The features must match ibnet.c's ibnet_features() exactly, in order and units.
The network runs in Q16 on SCORE: this script checks the quantised integer
model, the same sums the C code does, before writing it.
"""
import argparse
import csv
import struct
import sys

import numpy as np

RUNGS = ["idle0", "nominal", "interactive", "big", "rapid"]
MARKS = [0.25, 0.50, 0.85, 0.95]
HISTORY = 8          # samples (250 ms): two seconds
AHEAD = 8            # the label looks two seconds ahead
GAP_MS = 600         # a longer gap between samples ends a stretch (screen off)
HIDDEN = 24
TRANSITION_WEIGHT = 5
LIFT_CONFIDENCE = 0.8     # ibnet.c LIFT_CONFIDENCE
Q16 = 65536


def rung_for(load, app):
    r = sum(load > m for m in MARKS)
    return max(r, 1) if app else r


def features(rows):
    """ibnet_features() over the rows in order (V2: demand only; nothing that
    echoes the rung zrnperfd chose); the history is never reset"""
    hf, hs, hb = [0.0] * HISTORY, [0.0] * HISTORY, [0.0] * HISTORY
    n = 0
    out = []
    for r in rows:
        hf[n % HISTORY] = r["used_front"]
        hs[n % HISTORY] = r["used_shell"]
        hb[n % HISTORY] = r["c1_load"]
        n += 1
        have = min(n, HISTORY)
        last = [(n - 1 - i) % HISTORY for i in range(have)]
        four = min(have, 4)
        mean_front = sum(hf[j] for j in last[:4]) / four
        mean_shell = sum(hs[j] for j in last[:4]) / four
        mean_big = sum(hb[j] for j in last[:4]) / four
        peak_front = max(0.0, max(hf[j] for j in last))
        out.append([r["used_front"], r["used_shell"], r["c0_load"], r["c1_load"], r["touching"],
                    r["waydroid"], r["plugged"], mean_front, mean_shell, mean_big, peak_front,
                    r["used_front"] - mean_front])
    return np.array(out, dtype=np.float64)


def labels(rows):
    y = np.full(len(rows), -1)
    for i, r in enumerate(rows):
        peak, ok = 0, True
        for j in range(i + 1, i + 1 + AHEAD):
            gap = rows[j]["ms"] - rows[j - 1]["ms"] if j < len(rows) else 0
            if j >= len(rows) or not 0 < gap <= GAP_MS:      # screen off, or a reboot
                ok = False
                break
            peak = max(peak, rows[j]["rung"])
        if ok and r["profile"] == 1:          # balanced only: the ladder's own profile
            y[i] = int(peak)
    return y


def read_log(path):
    rows = []
    with open(path) as f:
        for d in csv.DictReader(f):
            try:
                row = {"ms": int(d["ms"]), "app": d["app"],
                       **{k: float(d[k]) for k in ("used_front", "used_shell", "c0_load", "c1_load")},
                       **{k: int(d[k]) for k in ("waydroid", "rung", "profile", "touching", "plugged")}}
                # V1 running "on" lifted the rung itself: those rungs are its own
                # choices, not the ladder's, and must not teach the next model
                ib = d.get("ib_rung") if "t_big" in d else None
                if ib not in (None, "", "-1") and int(ib) == row["rung"] and row["rung"] == 4:
                    continue
                rows.append(row)
            except (KeyError, ValueError):
                continue
    return rows


def synthetic(n=40000, seed=1):
    """bursts of load (scrolls, page loads, a game), calm between, an app or Start"""
    rng = np.random.default_rng(seed)
    rows, ms, load, app, rung = [], 0, 0.1, "-", 0
    for _ in range(n):
        if rng.random() < 0.01:
            app = "-" if rng.random() < 0.2 else rng.choice(["superscan", "waydroid.spotify", "ztmessages"])
        if rng.random() < 0.03:
            load = rng.choice([0.05, 0.3, 0.6, 0.9, 1.4])      # a new phase
        u = max(0.0, load + rng.normal(0, 0.08))
        touching = int(rng.random() < 0.15)
        rung = rung_for(u, app != "-")
        rows.append({"ms": ms, "app": app, "used_front": u if app != "-" else 0.0, "used_shell": 0.1 + 0.2 * touching,
                     "c0_load": min(1, u / 2), "c1_load": min(1, u / 3), "c0_khz": 1.5e6, "c1_khz": [9e5, 1.07e6, 1.8e6, 2.3e6, 2.7e6][rung] / 1e0,
                     "c1_online": [1, 1, 2, 4, 4][rung], "gpu_hz": 2.6e8, "touching": touching, "plugged": 0,
                     "waydroid": int(app.startswith("waydroid.")), "rung": rung, "profile": 1})
        ms += 250
    return rows


def train(X, y, sw, epochs=60, lr=3e-3, seed=0):
    """sw: each sample's weight (the moments before a change of rung count more)"""
    rng = np.random.default_rng(seed)
    n_in = X.shape[1]
    W1 = rng.normal(0, np.sqrt(2 / n_in), (n_in, HIDDEN)); b1 = np.zeros(HIDDEN)
    W2 = rng.normal(0, np.sqrt(2 / HIDDEN), (HIDDEN, len(RUNGS))); b2 = np.zeros(len(RUNGS))
    params = [W1, b1, W2, b2]
    m = [np.zeros_like(p) for p in params]; v = [np.zeros_like(p) for p in params]
    counts = np.bincount(y, minlength=len(RUNGS)).astype(float)
    cw = np.where(counts > 0, len(y) / (len(RUNGS) * np.maximum(counts, 1)), 0)   # balance the rungs
    t = 0
    for epoch in range(epochs):
        idx = rng.permutation(len(y))
        for s in range(0, len(y), 256):
            bi = idx[s:s + 256]; xb, yb, wb = X[bi], y[bi], sw[bi]
            h = np.maximum(0, xb @ W1 + b1)
            o = h @ W2 + b2
            p = np.exp(o - o.max(1, keepdims=True)); p /= p.sum(1, keepdims=True)
            g = p.copy(); g[np.arange(len(yb)), yb] -= 1; g *= (cw[yb] * wb)[:, None] / len(yb)
            gW2 = h.T @ g; gb2 = g.sum(0)
            gh = g @ W2.T * (h > 0)
            gW1 = xb.T @ gh; gb1 = gh.sum(0)
            t += 1
            for i, (pp, gg) in enumerate(zip(params, [gW1, gb1, gW2, gb2])):
                m[i] = 0.9 * m[i] + 0.1 * gg; v[i] = 0.999 * v[i] + 0.001 * gg * gg
                pp -= lr * (m[i] / (1 - 0.9 ** t)) / (np.sqrt(v[i] / (1 - 0.999 ** t)) + 1e-8)
    return params


def q16(a):
    return np.clip(np.round(np.asarray(a) * Q16), -2**31, 2**31 - 1).astype(np.int64)


def predict_int(Xn, W1q, b1q, W2q, b2q):
    """ibnet.c's forward() as SCORE computes it: each product >> 16, summed
    in 32 bits; the ReLU after the bias"""
    def mm(x, w):
        p = (x[:, :, None] * w[None, :, :]) >> 16
        return p.sum(1).astype(np.int32).astype(np.int64)
    xq = (np.clip(Xn, -64, 64) * Q16).astype(np.int64)
    h = np.maximum(mm(xq, W1q) + b1q, 0)
    return np.argmax(mm(h, W2q) + b2q, axis=1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="*")
    ap.add_argument("--synthetic", action="store_true")
    ap.add_argument("-o", "--out", default="model.bin")
    a = ap.parse_args()
    stretches = [synthetic()] if a.synthetic else [read_log(p) for p in a.logs]
    if not any(stretches):
        sys.exit("no samples")
    X = np.vstack([features(r) for r in stretches if r])
    y = np.concatenate([labels(r) for r in stretches if r])
    rungs = np.concatenate([[r["rung"] for r in s] for s in stretches if s])
    keep = y >= 0
    X, y, rungs = X[keep], y[keep], rungs[keep]
    print(f"{len(y)} samples; labels {np.bincount(y, minlength=5).tolist()} ({', '.join(RUNGS)})")
    mean = X.mean(0); scale = 1 / np.maximum(X.std(0), 1e-3)
    Xn = (X - mean) * scale
    cut = int(len(y) * 0.8)                      # the last fifth, unseen, for the scores
    # a change of rung is 2 % of the samples and all of the point: weigh it up
    sw = np.where(y != rungs, TRANSITION_WEIGHT, 1.0)
    W1, b1, W2, b2 = train(Xn[:cut], y[:cut], sw[:cut])
    W1q, b1q, W2q, b2q = q16(W1), q16(b1), q16(W2), q16(b2)
    pf = np.argmax(np.maximum(0, Xn[cut:] @ W1 + b1) @ W2 + b2, 1)
    pi = predict_int(Xn[cut:], W1q, b1q, W2q, b2q)
    yt = y[cut:]
    print(f"held-out: float {np.mean(pf == yt):.3f}, Q16 {np.mean(pi == yt):.3f}, "
          f"the ladder as it ran {np.mean(rungs[cut:] == yt):.3f}; "
          f"Q16 under the need {np.mean(pi < yt):.3f}, over {np.mean(pi > yt):.3f}")
    up = yt > rungs[cut:]                       # the ladder is about to climb
    down = yt < rungs[cut:]                     # the ladder is about to drop
    print(f"before a climb ({up.sum()} samples): Q16 there already {np.mean(pi[up] >= yt[up]):.3f}, "
          f"overshoot {np.mean(pi[up] > yt[up]):.3f}; "
          f"before a drop ({down.sum()}): Q16 down already {np.mean(pi[down] <= yt[down]):.3f}")
    # ibnet_predict's rule: the highest rung it is LIFT_CONFIDENCE sure of, raise-only
    ho = np.maximum(0, Xn[cut:] @ W1 + b1) @ W2 + b2
    pr = np.exp(ho - ho.max(1, keepdims=True)); pr /= pr.sum(1, keepdims=True)
    at_least = np.cumsum(pr[:, ::-1], 1)[:, ::-1]
    pick = np.array([max(k for k in range(len(RUNGS)) if at_least[i, k] >= LIFT_CONFIDENCE) for i in range(len(yt))])
    lift = np.maximum(pick, rungs[cut:])
    print(f"as run (raise-only, {LIFT_CONFIDENCE} sure): climbs caught early {np.mean(lift[up] >= yt[up]):.2f}, "
          f"lifted above the need {np.mean(lift > np.maximum(yt, rungs[cut:])):.3f} of samples")
    with open(a.out, "wb") as f:
        f.write(b"ZIB1" + struct.pack("<3I", X.shape[1], HIDDEN, len(RUNGS)))
        f.write(mean.astype("<f4").tobytes() + scale.astype("<f4").tobytes())
        for t in (W1q, b1q, W2q, b2q):
            f.write(t.astype("<i4").tobytes())
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()

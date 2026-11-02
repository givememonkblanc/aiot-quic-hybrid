#!/usr/bin/env python3
"""Learned MBM, offline: how many bytes can the first hop serve in the next tau, at confidence 1 - eps?

MBM admits at most Q = C(t, t + tau) - L_a bytes ahead of a result that may come at any time, where
C(t, t + tau) is the service of the bottleneck in the next tau (the limited-preemption bound of the
rate-latency server). C is not known at t; the window must use a lower quantile q_eps of C given what
the device knows at t, so that P(C < q) <= eps. A larger q at the same coverage admits more media.

Data: the [MOQ_TRACE] records of the device (AIOT_MOQT_TRACE_MS, every 100 ms): cumulative bytes
acknowledged (results, media), media outstanding, the window, the rate estimate and its deviation,
smoothed, variance, latest and smallest round trip, cwnd, bytes in flight, RSSI, the time to the
earliest end of the running inference. The service in (t, t + h) is the increase of the
acknowledged bytes. It is the service of the link only when the sender was backlogged over the
interval (media outstanding at both ends); other samples are app-limited and left out.

Models of q_eps (each conformalised on held-out runs, CQR: Romano et al., NeurIPS 2019):
  mbm        rate x tau                   (the window of MBM: a mean, no quantile)
  mbm-dev    (rate - deviation) x tau     (the predictive MBM)
  linear     linear quantile regression   (pinball loss)
  gbm        gradient-boosted quantile regression
  mlp        multilayer perceptron, pinball loss, on the last K records
Runs are split into train, calibration and test runs (by run, never by record).

Reported on the test runs, per horizon and model: miss = P(C < q) (target eps), and efficiency =
mean q / mean C (the share of the service that the window admits; larger is better at equal miss).

usage: learn_service.py --prefix mv0 [--prefix ...] [--eps 0.05] [--horizons 1 2] [--out results/learn.json]"""
import argparse
import json
import random
from pathlib import Path

import numpy as np

STEP_S = 0.1
K = 10


def records(serial):
    out = []
    for line in serial.read_text(errors="replace").splitlines():
        i = line.find("[MOQ_TRACE] ")
        if i < 0:
            continue
        try:
            out.append(json.loads(line[i + 12:]))
        except json.JSONDecodeError:
            pass
    return out


def run_samples(recs, horizon):
    """Features at t (the last K records) and the service in (t, t + horizon steps), backlogged only."""
    if len(recs) < K + horizon + 1:
        return [], [], []
    acked = np.array([r["acked"][0] + r["acked"][1] for r in recs], float)
    t = np.array([r["t_us"] for r in recs], float) / 1e6
    out_ = np.array([r["media_out"] for r in recs], float)
    cols = ("rate", "dev", "srtt", "rttvar", "latest", "min_rtt", "cwnd", "inflight", "media_out", "window", "rssi", "ai_safe_us")
    state = np.array([[float(r.get(c, 0)) for c in cols] for r in recs])
    X, y, base = [], [], []
    for i in range(K, len(recs) - horizon):
        dt = t[i + horizon] - t[i]
        if not (0.5 * horizon * STEP_S <= dt <= 1.5 * horizon * STEP_S):
            continue
        if out_[i] < 1200 or out_[i + horizon] < 1:
            continue
        served = np.diff(acked[i - K:i + 1]) / np.maximum(np.diff(t[i - K:i + 1]), 1e-3)
        X.append(np.concatenate([served, state[i]]))
        y.append((acked[i + horizon] - acked[i]))
        base.append((state[i][0] * horizon * STEP_S, max(0.0, state[i][0] - state[i][1]) * horizon * STEP_S))
    return X, y, base


def pinball(q, y, eps):
    d = y - q
    return np.mean(np.maximum(eps * d, (eps - 1) * d))


def conformal_shift(q_cal, y_cal, eps):
    """CQR for a lower quantile: q - s with s the ceil((1 - eps)(n + 1))-th smallest of q - y."""
    s = np.sort(q_cal - y_cal)
    k = min(len(s) - 1, int(np.ceil((1 - eps) * (len(s) + 1))) - 1)
    return s[k]


def fit_predict(name, Xtr, ytr, Xs, eps, seed):
    if name == "linear":
        from sklearn.linear_model import QuantileRegressor
        from sklearn.preprocessing import StandardScaler
        sc = StandardScaler().fit(Xtr)
        idx = np.random.default_rng(seed).permutation(len(Xtr))[:4000]
        m = QuantileRegressor(quantile=eps, alpha=0.0, solver="highs").fit(sc.transform(Xtr[idx]), ytr[idx])
        return [m.predict(sc.transform(X)) for X in Xs]
    if name == "gbm":
        from sklearn.ensemble import HistGradientBoostingRegressor
        m = HistGradientBoostingRegressor(loss="quantile", quantile=eps, max_iter=300, random_state=seed).fit(Xtr, ytr)
        return [m.predict(X) for X in Xs]
    if name == "mlp":
        import torch
        torch.manual_seed(seed)
        mu, sd = Xtr.mean(0), Xtr.std(0) + 1e-9
        ys = ytr.std() + 1e-9
        net = torch.nn.Sequential(torch.nn.Linear(Xtr.shape[1], 64), torch.nn.ReLU(), torch.nn.Linear(64, 64),
                                  torch.nn.ReLU(), torch.nn.Linear(64, 1))
        opt = torch.optim.Adam(net.parameters(), lr=1e-3, weight_decay=1e-4)
        xt = torch.tensor((Xtr - mu) / sd, dtype=torch.float32)
        yt = torch.tensor(ytr / ys, dtype=torch.float32)
        for _ in range(400):
            perm = torch.randperm(len(xt))
            for b in range(0, len(xt), 512):
                j = perm[b:b + 512]
                d = yt[j] - net(xt[j]).squeeze(1)
                loss = torch.maximum(eps * d, (eps - 1) * d).mean()
                opt.zero_grad()
                loss.backward()
                opt.step()
        with torch.no_grad():
            return [net(torch.tensor((X - mu) / sd, dtype=torch.float32)).squeeze(1).numpy() * ys for X in Xs]
    raise ValueError(name)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--results", type=Path, default=Path("results"))
    p.add_argument("--prefix", action="append", required=True)
    p.add_argument("--eps", type=float, default=0.05)
    p.add_argument("--horizons", type=int, nargs="+", default=[1, 2])
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--out", type=Path)
    a = p.parse_args()
    runs = sorted({r for pre in a.prefix for r in a.results.glob(pre + "-*") if (r / "serial.log").exists()})
    traces = {r.name: records(r / "serial.log") for r in runs}
    traces = {k: v for k, v in traces.items() if v}
    names = sorted(traces)
    random.Random(a.seed).shuffle(names)
    n = len(names)
    if n < 3:
        raise SystemExit(f"{n} runs with [MOQ_TRACE]: at least 3 are needed (train, calibration, test)")
    split = {"train": names[:max(1, int(0.6 * n))], "cal": names[max(1, int(0.6 * n)):max(2, int(0.8 * n))],
             "test": names[max(2, int(0.8 * n)):]}
    report = dict(eps=a.eps, runs=split, horizons={})
    for h in a.horizons:
        data = {}
        for part, members in split.items():
            X, y, base = [], [], []
            for name in members:
                xi, yi, bi = run_samples(traces[name], h)
                X += xi
                y += yi
                base += bi
            data[part] = (np.array(X), np.array(y), np.array(base))
        (Xtr, ytr, _), (Xc, yc, bc), (Xt, yt, bt) = data["train"], data["cal"], data["test"]
        print(f"\n== horizon {h} x {STEP_S * 1000:.0f} ms: samples train {len(ytr)} cal {len(yc)} test {len(yt)}; "
              f"mean service {yt.mean() if len(yt) else 0:.0f} B")
        if min(len(ytr), len(yc), len(yt)) < 20:
            print("too few backlogged samples")
            continue
        rows = {}
        preds = {"mbm": (bc[:, 0], bt[:, 0]), "mbm-dev": (bc[:, 1], bt[:, 1])}
        for name in ("linear", "gbm", "mlp"):
            qc, qt = fit_predict(name, Xtr, ytr, [Xc, Xt], a.eps, a.seed)
            preds[name] = (qc, qt)
        print(f"{'model':10} {'raw miss':>9} {'raw eff':>8} {'cqr miss':>9} {'cqr eff':>8} {'pinball':>8}")
        for name, (qc, qt) in preds.items():
            shift = conformal_shift(qc, yc, a.eps)
            qq = np.maximum(0.0, qt - shift)
            row = dict(raw_miss=float(np.mean(yt < qt)), raw_eff=float(np.mean(np.maximum(qt, 0)) / yt.mean()),
                       cqr_miss=float(np.mean(yt < qq)), cqr_eff=float(qq.mean() / yt.mean()),
                       pinball=float(pinball(qq, yt, a.eps)))
            rows[name] = row
            print(f"{name:10} {100 * row['raw_miss']:8.1f}% {row['raw_eff']:8.2f} {100 * row['cqr_miss']:8.1f}% "
                  f"{row['cqr_eff']:8.2f} {row['pinball']:8.1f}")
        report["horizons"][str(h)] = dict(samples=dict(train=len(ytr), cal=len(yc), test=len(yt)), models=rows)
    if a.out:
        a.out.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()

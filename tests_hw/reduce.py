#!/usr/bin/env python3
"""
Reduces the test CSVs to the summary numbers that go into the book.

    python3 reduce.py test1 test1_event_bus_latency.csv
    python3 reduce.py test2 test2_gps_scatter.csv
    python3 reduce.py test3 test3_turret_hor_auto.csv
"""

import csv
import math
import statistics
import sys
from collections import defaultdict


def rows(path):
    with open(path, newline="") as handle:
        return list(csv.DictReader(handle))


def test1(path):
    values = sorted(int(r["latency_us"]) for r in rows(path))
    if not values:
        print("no samples")
        return
    p95 = values[min(len(values) - 1, int(round(0.95 * (len(values) - 1))))]
    print(f"samples            {len(values)}")
    print(f"mean latency       {statistics.mean(values):.1f} us")
    print(f"p95 latency        {p95} us")
    print(f"max latency        {max(values)} us")


def test2(path):
    data = rows(path)
    if not data:
        print("no samples")
        return
    lats = [float(r["latitude"]) for r in data]
    lons = [float(r["longitude"]) for r in data]
    accs = [float(r["h_acc_m"]) for r in data]
    sats = [int(r["num_satellites"]) for r in data]
    mean_lat, mean_lon = statistics.mean(lats), statistics.mean(lons)
    # 1 degree of latitude is about 111320 m; scale longitude by cos(lat).
    scatter = [
        math.hypot(
            (la - mean_lat) * 111320.0,
            (lo - mean_lon) * 111320.0 * math.cos(math.radians(mean_lat)),
        )
        for la, lo in zip(lats, lons)
    ]
    print(f"samples             {len(data)}")
    print(f"duration            {float(data[-1]['elapsed_s']):.0f} s")
    print(f"satellites          {min(sats)} to {max(sats)}, mean {statistics.mean(sats):.1f}")
    print(f"mean reported h_acc {statistics.mean(accs):.2f} m")
    print(f"mean scatter        {statistics.mean(scatter):.2f} m")
    print(f"rms scatter         {statistics.pstdev(scatter):.2f} m")
    print(f"max scatter         {max(scatter):.2f} m")


def _wrap(delta):
    while delta > 180.0:
        delta -= 360.0
    while delta < -180.0:
        delta += 360.0
    return delta


def test3(path):
    data = rows(path)
    if not data:
        print("no samples")
        return

    auto = "heading_deg" in data[0]
    field = "heading_deg" if auto else "measured_deg"

    # readings[(commanded, direction)] = [values across passes]
    readings = defaultdict(list)
    for r in data:
        raw = r[field].strip()
        if not raw:
            continue
        readings[(float(r["commanded_deg"]), r["approach_direction"])].append(float(raw))

    if not readings:
        print(f"the {field} column is empty, nothing to reduce")
        return

    angles = sorted({a for a, _ in readings})

    # Mean reading per angle and direction.
    mean_up = {a: statistics.mean(readings[(a, "up")]) for a in angles if (a, "up") in readings}
    mean_dn = {a: statistics.mean(readings[(a, "down")]) for a in angles if (a, "down") in readings}

    # Scale and offset of reading against commanded angle, from a least squares
    # fit over both directions. For the compass this recovers the mounting
    # offset and the sign; for a protractor it should come out near 1 and 0.
    xs, ys = [], []
    ref = None
    for a in angles:
        for table in (mean_up, mean_dn):
            if a in table:
                value = table[a]
                if ref is None:
                    ref = value
                xs.append(a)
                ys.append(ref + _wrap(value - ref))
    n = len(xs)
    mx, my = statistics.mean(xs), statistics.mean(ys)
    denom = sum((x - mx) ** 2 for x in xs)
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / denom if denom else 1.0
    intercept = my - slope * mx

    print(f"reference column    {field}")
    print(f"fitted scale        {slope:.4f} deg per commanded deg")
    print(f"fitted offset       {intercept:.2f} deg")
    print()
    print("commanded  err_up   err_down  hysteresis  repeat_up  repeat_down")

    errors, hyst, repeat = [], [], []
    for a in angles:
        predicted = slope * a + intercept
        eu = _wrap(mean_up[a] - predicted) / slope if a in mean_up and slope else None
        ed = _wrap(mean_dn[a] - predicted) / slope if a in mean_dn and slope else None
        hy = (
            abs(_wrap(mean_up[a] - mean_dn[a]) / slope)
            if a in mean_up and a in mean_dn and slope
            else None
        )
        ru = (
            statistics.pstdev(readings[(a, "up")]) / abs(slope)
            if len(readings.get((a, "up"), [])) > 1 and slope
            else 0.0
        )
        rd = (
            statistics.pstdev(readings[(a, "down")]) / abs(slope)
            if len(readings.get((a, "down"), [])) > 1 and slope
            else 0.0
        )
        if eu is not None:
            errors.append(abs(eu))
        if ed is not None:
            errors.append(abs(ed))
        if hy is not None:
            hyst.append(hy)
        repeat += [ru, rd]
        print(
            f"{a:8.1f} {eu if eu is None else round(eu,2):>8} "
            f"{ed if ed is None else round(ed,2):>9} "
            f"{hy if hy is None else round(hy,2):>11} "
            f"{ru:>10.2f} {rd:>12.2f}"
        )

    print()
    if errors:
        print(f"mean absolute error   {statistics.mean(errors):.2f} deg")
        print(f"max absolute error    {max(errors):.2f} deg")
    if hyst:
        print(f"mean hysteresis       {statistics.mean(hyst):.2f} deg")
        print(f"max hysteresis        {max(hyst):.2f} deg")
    if any(repeat):
        print(f"mean repeatability sd {statistics.mean(repeat):.2f} deg")
        print(f"max repeatability sd  {max(repeat):.2f} deg")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        raise SystemExit(1)
    {"test1": test1, "test2": test2, "test3": test3}[sys.argv[1]](sys.argv[2])

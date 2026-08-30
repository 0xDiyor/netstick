#!/usr/bin/env python3
"""Turn a survey CSV from the SD card into a table and a self-contained HTML report.

    python3 tools/survey_report.py /Volumes/DONGLE/survey/20260902-101500.csv
    python3 tools/survey_report.py session.csv --html report.html

No dependencies. Bars are drawn with inline SVG so the report opens anywhere.
"""
import argparse, csv, html, sys
from collections import OrderedDict

def load(path):
    pts = OrderedDict()
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            key = (int(r["point"]), r["label"])
            pts.setdefault(key, {"time": r["iso_time"], "bands": {}})
            pts[key]["bands"][r["band_ghz"]] = r
    return pts

def num(v, default=None):
    try: return float(v)
    except (TypeError, ValueError): return default

def table(pts):
    print(f"{'#':>2} {'label':<12} {'band':<4} {'rssi':>5} {'ping':>6} {'loss':>5} {'down':>7} {'up':>7}  ch/phy")
    for (i, label), p in pts.items():
        for b in ("2.4", "5"):
            r = p["bands"].get(b)
            if not r: continue
            if not r.get("rssi_dbm"):
                print(f"{i:>2} {label:<12} {b+'G':<4} {'':>5} {r.get('note',''):<30}")
                continue
            print(f"{i:>2} {label:<12} {b+'G':<4} {r['rssi_dbm']:>5} {r['ping_avg_ms']+'ms':>6} {r['loss_pct']+'%':>5} "
                  f"{r['dl_mbps']:>7} {r['ul_mbps']:>7}  ch{r['channel']} {r['phy']} {r['bw_mhz']}MHz")

def bar(value, vmax, color, width=220):
    w = 0 if value is None or vmax <= 0 else int(width * max(0, value) / vmax)
    return f'<svg width="{width}" height="14"><rect width="{width}" height="14" fill="#eee"/><rect width="{w}" height="14" fill="{color}"/></svg>'

def html_report(pts, path, src):
    dmax = max([num(r.get("dl_mbps"), 0) for p in pts.values() for r in p["bands"].values()] + [1])
    rows = []
    for (i, label), p in pts.items():
        for b, color in (("2.4", "#e8963c"), ("5", "#3ba7e8")):
            r = p["bands"].get(b)
            if not r: continue
            if not r.get("rssi_dbm"):
                rows.append(f"<tr><td>{i}</td><td>{html.escape(label)}</td><td>{b} GHz</td><td colspan=6 class=dim>{html.escape(r.get('note',''))}</td></tr>")
                continue
            rssi = num(r["rssi_dbm"], -100)
            q = max(0, min(100, (rssi + 90) * 100 / 40))
            rows.append(
                f"<tr><td>{i}</td><td>{html.escape(label)}</td><td>{b} GHz</td>"
                f"<td>{r['rssi_dbm']} dBm {bar(q, 100, color, 80)}</td>"
                f"<td>{r['ping_avg_ms']} ms / {r['loss_pct']}%</td>"
                f"<td>{r['dl_mbps']} {bar(num(r['dl_mbps']), dmax, color)}</td>"
                f"<td>{r['ul_mbps']} {bar(num(r['ul_mbps']), dmax, color)}</td>"
                f"<td>ch{r['channel']} {r['phy']} {r['bw_mhz']}MHz</td><td class=dim>{r['bssid'][9:]}</td></tr>")
    doc = f"""<!doctype html><meta charset=utf-8><title>survey {html.escape(src)}</title>
<style>body{{font:14px/1.4 -apple-system,Segoe UI,sans-serif;margin:2em;color:#222}}table{{border-collapse:collapse}}
td,th{{padding:4px 10px;border-bottom:1px solid #ddd;text-align:left;white-space:nowrap}}th{{background:#f4f4f4}}.dim{{color:#888}}svg{{vertical-align:middle;margin-left:6px}}</style>
<h1>Walking survey</h1><p class=dim>{html.escape(src)} &middot; {len(pts)} points &middot; speeds in Mbit/s, bars scaled to the fastest download ({dmax:g})</p>
<table><tr><th>#</th><th>spot</th><th>band</th><th>signal</th><th>ping / loss</th><th>down</th><th>up</th><th>link</th><th>ap</th></tr>
{''.join(rows)}</table>"""
    open(path, "w").write(doc)
    print("wrote", path)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv"); ap.add_argument("--html")
    a = ap.parse_args()
    pts = load(a.csv)
    table(pts)
    html_report(pts, a.html or a.csv.rsplit(".", 1)[0] + ".html", a.csv)

if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# ============================================================================
#  NUBY INDEX BUILDER — de pages.jsonl al índice que lee el motor
# ============================================================================
#  Produce nuby_engine/data/crawl_pages.tsv con el MISMO formato y escapado que
#  SearchIndex::save() del motor (campos: url \t title \t text; \t \n \r \\ se
#  escapan con barra invertida). Así el motor C++ (nativo y WebAssembly) carga
#  el índice sin cambiar una línea de su código.
#
#  También escribe crawler/data/index_stats.json con números reales:
#  tamaño del TSV, tamaño comprimido, páginas por dominio y proyecciones.
#
#  Uso:
#    python3 crawler/build_index.py            # fusiona y mide
#    python3 crawler/build_index.py --check    # solo informa, no escribe
# ============================================================================

import argparse
import gzip
import json
import os
import re
import sys
from collections import Counter
from urllib.parse import urlsplit

TSV = "nuby_engine/data/crawl_pages.tsv"
PAGES = "crawler/data/pages.jsonl"
STATS = "crawler/data/index_stats.json"
TERM_RE = re.compile(r"[0-9a-záéíóúüñçàèìòùâêîôûäëïöüß]+", re.I)


def esc(s: str) -> str:
    return (s.replace("\\", "\\\\").replace("\t", "\\t")
             .replace("\n", "\\n").replace("\r", "\\r"))


def unesc(s: str) -> str:
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            n = s[i + 1]
            out.append({"t": "\t", "n": "\n", "r": "\r"}.get(n, n))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def domain_of(url):
    try:
        return urlsplit(url).netloc.lower().replace("www.", "")
    except Exception:
        return ""


def load_existing(path):
    rows = {}
    if not os.path.exists(path):
        return rows
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            parts, cur = [], []
            i = 0
            while i < len(line):
                if line[i] == "\\" and i + 1 < len(line):
                    cur.append(line[i]); cur.append(line[i + 1]); i += 2
                elif line[i] == "\t":
                    parts.append("".join(cur)); cur = []; i += 1
                else:
                    cur.append(line[i]); i += 1
            parts.append("".join(cur))
            if len(parts) < 3:
                continue
            url = unesc(parts[0])
            rows[url] = (unesc(parts[1]), unesc(parts[2]))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="solo informa, no escribe")
    ap.add_argument("--tsv", default=TSV)
    ap.add_argument("--pages", default=PAGES)
    args = ap.parse_args()

    base = load_existing(args.tsv)
    base_n = len(base)
    print(f"[index] índice existente: {base_n} páginas ({args.tsv})")

    added = 0
    if os.path.exists(args.pages):
        with open(args.pages, encoding="utf-8") as f:
            for line in f:
                try:
                    rec = json.loads(line)
                except Exception:
                    continue
                url = rec.get("url")
                if not url:
                    continue
                title = (rec.get("title") or url)[:200]
                text = (rec.get("text") or "").strip()
                if len(text) < 40:      # una página sin texto no sirve para buscar
                    continue
                if url not in base:
                    added += 1
                base[url] = (title, text)
    print(f"[index] páginas nuevas aportadas por el rastreo: {added}")
    print(f"[index] total tras fusionar: {len(base)}")

    if args.check:
        return 0

    os.makedirs(os.path.dirname(args.tsv) or ".", exist_ok=True)
    os.makedirs(os.path.dirname(STATS), exist_ok=True)
    with open(args.tsv, "w", encoding="utf-8") as f:
        for url, (title, text) in base.items():
            f.write(f"{esc(url)}\t{esc(title)}\t{esc(text)}\n")

    raw = os.path.getsize(args.tsv)
    with open(args.tsv, "rb") as f:
        gz = len(gzip.compress(f.read(), 9))

    domains = Counter(domain_of(u) for u in base)
    wiki = sum(c for d, c in domains.items() if "wikipedia.org" in d or "wikimedia" in d)
    n = max(len(base), 1)
    terms = 0
    sample = 0
    uniq = set()
    for url, (title, text) in base.items():
        if sample < 3000:
            uniq.update(t.lower() for t in TERM_RE.findall((title + " " + text)[:1200]))
            sample += 1
        terms += len(TERM_RE.findall(text))
    est_unique = len(uniq) * (n / max(sample, 1))
    per_page = raw / n

    stats = {
        "pages": len(base),
        "pages_previous": base_n,
        "pages_new": added,
        "domains": len(domains),
        "wikipedia_share_pct": round(100.0 * wiki / n, 1),
        "tsv_bytes": raw,
        "tsv_gzip_bytes": gz,
        "bytes_per_page": round(per_page, 1),
        "bytes_per_page_gzip": round(gz / n, 1),
        "terms_total_estimate": terms,
        "terms_unique_estimate": int(est_unique),
        "top_domains": domains.most_common(20),
        "projections": {
            f"{scale}_pages": {
                "tsv_mb": round(per_page * scale / 1e6, 1),
                "gzip_mb": round((gz / n) * scale / 1e6, 1),
            }
            for scale in (10_000, 100_000, 500_000, 1_000_000)
        },
        "github_pages_limit_mb": 1000,
    }
    with open(STATS, "w", encoding="utf-8") as f:
        json.dump(stats, f, ensure_ascii=False, indent=2)

    print("\n===== ÍNDICE CONSTRUIDO =====")
    print(f"  páginas                    : {stats['pages']} (+{added} nuevas)")
    print(f"  dominios distintos         : {stats['domains']}")
    print(f"  peso de Wikipedia          : {stats['wikipedia_share_pct']}%  (línea base era 97%)")
    print(f"  tamaño del índice (TSV)    : {raw/1e6:.2f} MB ({stats['bytes_per_page']} B/página)")
    print(f"  comprimido (gzip)          : {gz/1e6:.2f} MB ({stats['bytes_per_page_gzip']} B/página)")
    print(f"  términos únicos (estimado) : ~{int(est_unique):,}")
    print("  proyección con este mismo formato:")
    for k, v in stats["projections"].items():
        pages = k.split("_")[0]
        print(f"      {pages:>9} páginas → {v['tsv_mb']:>7.1f} MB crudo / {v['gzip_mb']:>6.1f} MB gzip")
    print(f"  (límite recomendado de GitHub Pages: {stats['github_pages_limit_mb']} MB)")
    print("  top dominios:")
    for d, c in stats["top_domains"][:12]:
        print(f"      {c:5d}  {d}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

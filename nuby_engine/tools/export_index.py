#!/usr/bin/env python3
"""
Exporta el rastreo REAL de spider.py (SQLite) al formato TSV del índice Nuby.
Los datos son páginas web reales rastreadas el 8-ago-2026 (Wikipedia,
Hacker News, etc.). Uso:  python3 tools/export_index.py
"""
import sqlite3, os, sys, re

DB = os.path.join(os.path.dirname(__file__), "..", "data", "nuby_massive.db")
OUT = os.path.join(os.path.dirname(__file__), "..", "data", "crawl_pages.tsv")

def clean(t):
    return re.sub(r"\s+", " ", (t or "")).strip()

def esc(s):
    return (s or "").replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n").replace("\r", "\\r")

def main():
    if not os.path.exists(DB):
        print(f"no existe {DB}"); sys.exit(1)
    db = sqlite3.connect(DB)
    cur = db.cursor()
    cols = [r[1] for r in cur.execute("PRAGMA table_info(pages_index)").fetchall()]
    print("columnas pages_index:", cols)
    rows = cur.execute("SELECT * FROM pages_index").fetchall()
    n = 0
    with open(OUT, "w", encoding="utf-8") as f:
        for row in rows:
            d = dict(zip(cols, row))
            url = d.get("url", "")
            title = clean(d.get("title"))
            # texto: mejor columna disponible
            text = clean(d.get("content") or d.get("text") or d.get("snippet") or d.get("title"))
            if not url or len(text) < 5:
                continue
            f.write(f"{esc(url)}\t{esc(title)}\t{esc(text)}\n")
            n += 1
    print(f"exportadas {n} paginas reales → {OUT}")

if __name__ == "__main__":
    main()

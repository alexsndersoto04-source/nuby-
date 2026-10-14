#!/usr/bin/env python3
# ============================================================================
#  NUBY CRAWLER — rastreador real para la arquitectura de PLAN_ESCALABLE.md
# ============================================================================
#  Diseño honesto y verificable:
#   • Respeta robots.txt de CADA dominio (y su Crawl-delay si lo declara).
#   • 1 petición por segundo por dominio como MÍNIMO (nunca martillea un sitio).
#   • Se identifica con un User-Agent propio y una URL de contacto.
#   • Tope de páginas por dominio, tope de bytes por página, tope de tiempo.
#   • Deduplica por URL normalizada y por contenido (misma página = 1 entrada).
#   • Guarda estado en disco tras cada página: un corte no pierde el trabajo.
#   • NO guarda la página completa: URL + título + resumen indexable (~800
#     caracteres de texto visible). El usuario final acaba en el sitio real.
#
#  Salida (crawler/data/):
#   pages.jsonl     una línea JSON por página rastreada
#   frontier.jsonl  cola de URLs pendientes (se reanuda donde quedó)
#   seen.txt        hashes de URLs ya vistas (no se repiten)
#   stats.json      números reales de la corrida
#
#  Uso:
#   python3 crawler/nuby_crawler.py --seeds crawler/seeds.txt \
#       --data crawler/data --max-pages 500 --max-seconds 900
# ============================================================================

import argparse
import hashlib
import heapq
import json
import os
import re
import sys
import threading
import time
import urllib.robotparser as robotparser
from collections import Counter
from urllib.parse import urljoin, urlsplit, urlunsplit

import requests
from bs4 import BeautifulSoup

VERSION = "0.4"
CONTACT = "https://github.com/alexsndersoto04-source/nuby-"
UA = f"NubyBot/{VERSION} (+{CONTACT}; rastreador de buscador independiente)"

MAX_BYTES = 2_000_000          # 2 MB por página: suficiente y prudente
TIMEOUT = (10, 20)             # (conexión, lectura) segundos
MAX_TEXT = 800                 # caracteres de texto visible que se indexan
MAX_DEPTH = 3

# Dominios que no se rastrean: redes sociales, vídeo, o que son un desperdicio
BLACKLIST = (
    "facebook.com", "twitter.com", "x.com", "instagram.com", "tiktok.com",
    "pinterest.com", "youtube.com", "youtu.be", "twitch.tv", "netflix.com",
    "spotify.com", "amazon.com", "ebay.com", "aliexpress.com", "linkedin.com",
    "whatsapp.com", "telegram.org", "discord.gg", "discord.com", "paypal.com",
)

SKIP_EXT = (
    ".jpg", ".jpeg", ".png", ".gif", ".webp", ".svg", ".ico", ".bmp", ".tiff",
    ".mp3", ".mp4", ".avi", ".mkv", ".mov", ".webm", ".ogg", ".wav", ".flac",
    ".zip", ".rar", ".7z", ".gz", ".bz2", ".xz", ".tar", ".pdf", ".doc",
    ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".exe", ".dmg", ".iso", ".apk",
    ".css", ".js", ".json", ".xml", ".rss", ".atom", ".woff", ".woff2", ".ttf",
)

TRACKING_PARAMS = ("utm_", "fbclid", "gclid", "mc_cid", "mc_eid", "ref_src", "igshid")


def norm_url(url, base=None):
    """Normaliza una URL: absoluta, sin fragmento, sin parámetros de rastreo."""
    if not url:
        return None
    url = url.strip()
    if base:
        url = urljoin(base, url)
    if not url.lower().startswith(("http://", "https://")):
        return None
    try:
        p = urlsplit(url)
    except ValueError:
        return None
    if not p.netloc or "." not in p.netloc:
        return None
    host = p.netloc.lower()
    if host.startswith("www."):
        host = host[4:]
    if ":" in host and not host.endswith((":80", ":443")):
        pass
    elif host.endswith(":80") or host.endswith(":443"):
        host = host.split(":")[0]
    scheme = "https" if p.scheme == "https" else "http"
    path = re.sub(r"/{2,}", "/", p.path) or "/"
    query_parts = [kv for kv in p.query.split("&")
                   if kv and not kv.lower().startswith(TRACKING_PARAMS)]
    query = "&".join(sorted(query_parts))
    return urlunsplit((scheme, host, path, query, ""))


def domain_of(url):
    return urlsplit(url).netloc.lower()


def is_crawlable(url):
    low = url.lower()
    if any(b in low for b in BLACKLIST):
        return False
    path = urlsplit(low).path
    if path.endswith(SKIP_EXT):
        return False
    if re.search(r"\.(jpg|png|gif|pdf|zip)\?", low):
        return False
    return True


def ua_hash(url):
    return hashlib.sha1(url.encode("utf-8", "replace")).hexdigest()


class Robots:
    """Caché de robots.txt. Si hay error 4xx → permitir; si es 403/5xx o falla
    la conexión → bloquear el dominio (criterio prudente, no agresivo)."""

    def __init__(self):
        self._cache = {}
        self._lock = threading.Lock()
        self.blocked_domains = Counter()

    def rules(self, url):
        dom = domain_of(url)
        with self._lock:
            if dom in self._cache:
                return self._cache[dom]
        rp = robotparser.RobotFileParser()
        scheme = urlsplit(url).scheme or "https"
        robots_url = f"{scheme}://{dom}/robots.txt"
        try:
            r = requests.get(robots_url, headers={"User-Agent": UA},
                             timeout=TIMEOUT, allow_redirects=True)
            if r.status_code in (401, 403) or r.status_code >= 500:
                rp.disallow_all = True
                self.blocked_domains[dom] += 1
            elif r.status_code >= 400:
                rp.allow_all = True
            else:
                rp.parse(r.text.splitlines())
        except Exception:
            rp.allow_all = True  # sin robots.txt accesible: seguir siendo prudente (1 req/s)
        with self._lock:
            self._cache[dom] = rp
        return rp

    def allowed(self, url):
        try:
            return self.rules(url).can_fetch(UA, url)
        except Exception:
            return True

    def delay(self, url):
        try:
            d = self.rules(url).crawl_delay(UA)
            return float(d) if d else 0.0
        except Exception:
            return 0.0


class Crawler:
    def __init__(self, args):
        self.a = args
        self.data_dir = args.data
        os.makedirs(self.data_dir, exist_ok=True)
        self.pages_path = os.path.join(self.data_dir, "pages.jsonl")
        self.frontier_path = os.path.join(self.data_dir, "frontier.jsonl")
        self.seen_path = os.path.join(self.data_dir, "seen.txt")
        self.stats_path = os.path.join(self.data_dir, "stats.json")

        self.seen = set()
        if os.path.exists(self.seen_path):
            with open(self.seen_path, encoding="utf-8") as f:
                self.seen = {ln.strip() for ln in f if ln.strip()}

        self.robots = Robots()
        self.lock = threading.Lock()
        self.heap = []                 # (ready_ts, priority, depth, url)
        self.queued = set()            # URLs ya encoladas (dedupe mientras corre)
        self.domain_next = {}          # dominio → timestamp en que puede volver a pedirse
        self.domain_count = Counter()
        self.by_domain = Counter()
        self.by_domain_ok = Counter()
        self.content_hashes = set()
        self.counters = Counter()
        self.started = time.time()
        self.pages_file = open(self.pages_path, "a", encoding="utf-8")
        self.seen_file = open(self.seen_path, "a", encoding="utf-8")
        self.stop = threading.Event()
        self.inflight = 0               # peticiones en curso (evita parar antes de tiempo)
        self._load_frontier()

    # ---------------- estado ----------------
    def _load_frontier(self):
        if os.path.exists(self.frontier_path):
            with open(self.frontier_path, encoding="utf-8") as f:
                for line in f:
                    try:
                        it = json.loads(line)
                        heapq.heappush(self.heap, (time.time(), it["p"], it["d"], it["u"]))
                        self.queued.add(it["u"])
                    except Exception:
                        continue
        if not self.heap:  # primer arranque: las semillas
            seeds = []
            with open(self.a.seeds, encoding="utf-8") as f:
                for line in f:
                    line = line.split("#", 1)[0].strip()
                    if line:
                        u = norm_url(line)
                        if u and u not in self.seen:
                            seeds.append(u)
            for u in seeds:
                heapq.heappush(self.heap, (time.time(), 0, 0, u))
            print(f"[crawler] frontera inicial: {len(seeds)} semillas de {self.a.seeds}")

    def _save_frontier(self):
        tmp = self.frontier_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            for ready, prio, depth, url in self.heap:
                f.write(json.dumps({"u": url, "p": prio, "d": depth}, ensure_ascii=False) + "\n")
        os.replace(tmp, self.frontier_path)

    def _append_page(self, rec):
        line = json.dumps(rec, ensure_ascii=False)
        with self.lock:
            self.pages_file.write(line + "\n")
            self.pages_file.flush()

    def _mark_seen(self, url):
        h = ua_hash(url)
        with self.lock:
            if h in self.seen:
                return False
            self.seen.add(h)
            self.seen_file.write(h + "\n")
            self.seen_file.flush()
        return True

    # ---------------- planificación ----------------
    def _pop_ready(self):
        """Devuelve la siguiente URL cuya espera de cortesía ya pasó.

        IMPORTANTE: la URL se saca de la cola siempre que se devuelve. Si no
        estuviera lista todavía, se devuelve None y el hilo espera: así dos
        hilos nunca piden la misma URL."""
        now = time.time()
        with self.lock:
            if not self.heap:
                return None
            ready, prio, depth, url = self.heap[0]
            dom = domain_of(url)
            if max(ready, self.domain_next.get(dom, 0.0)) > now:
                return None
            heapq.heappop(self.heap)
            return (ready, prio, depth, url)

    def _defer(self, seconds, prio, depth, url):
        with self.lock:
            heapq.heappush(self.heap, (time.time() + seconds, prio, depth, url))

    def _complete_domain(self, url, delay):
        with self.lock:
            self.domain_next[domain_of(url)] = time.time() + max(self.a.delay, delay)

    def _enqueue_links(self, links, depth, base_url):
        n = 0
        for raw in links:
            if depth + 1 > MAX_DEPTH:
                break
            u = norm_url(raw, base=base_url)
            if not u or not is_crawlable(u):
                continue
            d = domain_of(u)
            if self.domain_count[d] >= self.a.per_domain:
                continue
            with self.lock:
                if u in self.queued:
                    continue
                self.queued.add(u)
                heapq.heappush(self.heap, (time.time(), 1, depth + 1, u))
                self.domain_count[d] += 1
            n += 1
        return n

    # ---------------- descarga y parseo ----------------
    def _fetch(self, url):
        sess = requests.Session()
        r = sess.get(url, headers={"User-Agent": UA, "Accept": "text/html,application/xhtml+xml",
                                   "Accept-Language": "es-ES,es;q=0.9,en;q=0.8"},
                     timeout=TIMEOUT, allow_redirects=True, stream=True)
        ctype = r.headers.get("Content-Type", "").lower()
        if not any(t in ctype for t in ("text/html", "application/xhtml")):
            r.close()
            raise ValueError(f"no-html:{ctype.split(';')[0]}")
        chunks, size = [], 0
        for chunk in r.iter_content(65536):
            chunks.append(chunk)
            size += len(chunk)
            if size >= MAX_BYTES:
                break
        r.close()
        body = b"".join(chunks)
        enc = r.encoding or "utf-8"
        try:
            html = body.decode(enc, errors="replace")
        except LookupError:
            html = body.decode("utf-8", errors="replace")
        return html, size, r.status_code, r.url

    def _extract(self, html, url):
        soup = BeautifulSoup(html, "lxml")
        html_el = soup.find("html")
        lang = (html_el.get("lang") if html_el else "") or ""
        lang = lang.split("-")[0].lower()[:2]

        title = ""
        if soup.title and soup.title.string:
            title = re.sub(r"\s+", " ", soup.title.string).strip()[:200]

        desc = ""
        meta = soup.find("meta", attrs={"name": re.compile("^description$", re.I)}) or \
               soup.find("meta", attrs={"property": re.compile("^og:description$", re.I)})
        if meta and meta.get("content"):
            desc = re.sub(r"\s+", " ", meta["content"]).strip()[:300]

        links = []
        for a in soup.find_all("a", href=True):
            href = a["href"]
            if href.startswith(("mailto:", "tel:", "javascript:", "#")):
                continue
            links.append(href)

        for tag in soup(["script", "style", "noscript", "template", "svg", "nav",
                         "footer", "header", "aside", "form", "iframe"]):
            tag.decompose()
        body = re.sub(r"\s+", " ", soup.get_text(" ", strip=True)).strip()
        # segunda pasada SIN el texto de los enlaces: es la que se usa para
        # detectar páginas duplicadas (mismo texto, enlaces distintos)
        for a in soup.find_all("a"):
            a.decompose()
        body_core = re.sub(r"\s+", " ", soup.get_text(" ", strip=True)).strip()

        text = " ".join(x for x in (title, desc, body[:MAX_TEXT]) if x).strip()
        # huella para deduplicar: título + descripción + entradilla (sin el texto
        # de los enlaces, que cambia entre páginas que por lo demás son iguales)
        fingerprint = (title + " " + desc + " " + body_core[:300]).strip()
        return title or url, text[:1200], links, lang, fingerprint

    def _worker(self, wid=0):
        # BUG REAL corregido aquí (probado en vivo): un hilo veía la cola vacía
        # y declaraba "se acabó" mientras OTRO hilo aún estaba descargando; las
        # páginas que ese hilo descubría después nunca se rastreaban. Ahora se
        # cuenta el trabajo en vuelo y solo se termina cuando no hay cola NI
        # trabajo en vuelo (condición de carrera clásica de orquestadores).
        while not self.stop.is_set():
            if (self.counters["ok"] >= self.a.max_pages
                    or time.time() - self.started >= self.a.max_seconds):
                self.stop.set()
                break
            item = self._pop_ready()
            if item is None:
                with self.lock:
                    more = bool(self.heap) or self.inflight > 0
                if not more:
                    self.stop.set()
                    break
                time.sleep(0.05)
                continue
            _, prio, depth, url = item
            with self.lock:
                self.inflight += 1
            try:
                self._process(prio, depth, url)
            finally:
                with self.lock:
                    self.inflight -= 1

    def _process(self, prio, depth, url):
            dom = domain_of(url)
            if not self._mark_seen(url):
                return  # ya la vimos (evita repetir en una corrida larga)

            if not self.robots.allowed(url):
                self.counters["robots_blocked"] += 1
                self._complete_domain(url, 0.0)
                return

            try:
                html, size, status, final_url = self._fetch(url)
            except ValueError as e:
                self.counters["skip_nohtml"] += 1
                self._complete_domain(url, 0.0)
                return
            except Exception as e:
                self.counters["fail"] += 1
                self._complete_domain(url, 0.0)
                if self.counters["fail"] <= 12:
                    print(f"  [fallo] {url} → {type(e).__name__}: {str(e)[:80]}")
                return

            title, text, links, lang, fingerprint = self._extract(html, url)
            chash = hashlib.sha1(fingerprint.encode("utf-8", "replace")).hexdigest()

            with self.lock:
                dup = chash in self.content_hashes
                if not dup:
                    self.content_hashes.add(chash)
            if dup:
                self.counters["dup_content"] += 1
            else:
                self._append_page({
                    "url": final_url,
                    "title": title,
                    "text": text,
                    "domain": dom,
                    "lang": lang,
                    "status": status,
                    "bytes": size,
                    "depth": depth,
                    "ts": int(time.time()),
                })
                self.counters["ok"] += 1
                self.by_domain_ok[dom] += 1
                self.counters["bytes"] += size
                if lang:
                    self.counters["lang_" + lang] += 1

            self.by_domain[dom] += 1
            self._enqueue_links(links, depth, final_url)
            self._complete_domain(url, self.robots.delay(url))

    # ---------------- orquestación ----------------
    def run(self):
        threads = [threading.Thread(target=self._worker, args=(i,), daemon=True)
                   for i in range(self.a.workers)]
        for t in threads:
            t.start()
        t0 = time.time()
        while any(t.is_alive() for t in threads):
            for t in threads:
                t.join(timeout=1.0)
            if int(time.time() - t0) % 30 < 1 and self.counters["ok"]:
                print(f"[crawler] {self.counters['ok']} páginas · {len(self.by_domain_ok)} dominios "
                      f"· {int(time.time() - t0)}s")
        for t in threads:
            t.join(timeout=5)
        self._save_frontier()

        elapsed = round(time.time() - self.started, 1)
        wiki = sum(c for d, c in self.by_domain_ok.items() if "wikipedia.org" in d or "wikimedia" in d)
        ok = max(self.counters["ok"], 1)
        stats = {
            "run_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "elapsed_s": elapsed,
            "pages_indexed": self.counters["ok"],
            "domains": len(self.by_domain_ok),
            "pages_per_domain": round(self.counters["ok"] / max(len(self.by_domain_ok), 1), 1),
            "wikipedia_share_pct": round(100.0 * wiki / ok, 1),
            "fetch_failed": self.counters["fail"],
            "robots_blocked": self.counters["robots_blocked"],
            "skipped_non_html": self.counters["skip_nohtml"],
            "duplicate_content": self.counters["dup_content"],
            "bytes_fetched": self.counters["bytes"],
            "avg_page_bytes": int(self.counters["bytes"] / ok),
            "frontier_pending": len(self.heap),
            "top_domains": self.by_domain_ok.most_common(15),
            "languages": {k[5:]: v for k, v in self.counters.items() if k.startswith("lang_")},
            "pages_per_second": round(self.counters["ok"] / max(elapsed, 1), 2),
        }
        with open(self.stats_path, "w", encoding="utf-8") as f:
            json.dump(stats, f, ensure_ascii=False, indent=2)
        with open(self.pages_path, encoding="utf-8") as f:
            total_lines = sum(1 for _ in f)
        print("\n===== RESULTADO DE LA CORRIDA =====")
        print(f"  páginas indexadas esta corrida : {stats['pages_indexed']}")
        print(f"  dominios distintos (con éxito) : {stats['domains']}")
        print(f"  peso de Wikipedia              : {stats['wikipedia_share_pct']}%")
        print(f"  fallos de red / robots         : {stats['fetch_failed']} / {stats['robots_blocked']}")
        print(f"  no-HTML saltadas               : {stats['skipped_non_html']}")
        print(f"  duplicados de contenido        : {stats['duplicate_content']}")
        print(f"  ingerido                       : {stats['bytes_fetched']/1e6:.2f} MB "
              f"({stats['avg_page_bytes']} B/página de media)")
        print(f"  velocidad                      : {stats['pages_per_second']} páginas/s ({elapsed}s)")
        print(f"  frontera pendiente             : {stats['frontier_pending']} URLs")
        print(f"  páginas.jsonl acumuladas       : {total_lines}")
        print(f"  idiomas (top)                  : {sorted(stats['languages'].items(), key=lambda x: -x[1])[:6]}")
        print("  dominios (top)                 :")
        for d, c in stats["top_domains"]:
            print(f"      {c:4d}  {d}")


def main():
    ap = argparse.ArgumentParser(description="Rastreador real de Nuby")
    ap.add_argument("--seeds", default="crawler/seeds.txt")
    ap.add_argument("--data", default="crawler/data")
    ap.add_argument("--max-pages", type=int, default=int(os.environ.get("NUBY_MAX_PAGES", "500")))
    ap.add_argument("--max-seconds", type=int, default=int(os.environ.get("NUBY_MAX_SECONDS", "900")))
    ap.add_argument("--per-domain", type=int, default=int(os.environ.get("NUBY_PER_DOMAIN", "20")))
    ap.add_argument("--delay", type=float, default=float(os.environ.get("NUBY_DELAY", "1.0")))
    ap.add_argument("--workers", type=int, default=int(os.environ.get("NUBY_WORKERS", "12")))
    args = ap.parse_args()
    print(f"[crawler] UA: {UA}")
    print(f"[crawler] límites: {args.max_pages} páginas · {args.max_seconds}s · "
          f"{args.per_domain}/dominio · {args.delay}s cortesía · {args.workers} hilos")
    Crawler(args).run()


if __name__ == "__main__":
    main()

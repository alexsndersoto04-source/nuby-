#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Lanzador de Ask.

Levanta un servidor local (localhost) y abre el estudio en el navegador.
Hace falta un servidor porque los navegadores solo permiten usar el micrófono
y los procesadores de audio (AudioWorklet) en localhost o HTTPS: abriendo el
archivo index.html con doble clic (file://) el micrófono no funciona.

No necesita instalar nada: usa solo la biblioteca estándar de Python 3.
"""

import http.server
import os
import socket
import sys
import threading
import webbrowser

RAIZ = os.path.dirname(os.path.abspath(__file__))
PUERTOS = [4173, 4174, 4180, 8000]


class Handler(http.server.SimpleHTTPRequestHandler):
    """Sirve la app con los tipos MIME correctos y sin caché."""

    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".js": "text/javascript; charset=utf-8",
        ".mjs": "text/javascript; charset=utf-8",
        ".css": "text/css; charset=utf-8",
        ".json": "application/json; charset=utf-8",
        ".webmanifest": "application/manifest+json; charset=utf-8",
        ".svg": "image/svg+xml",
        ".png": "image/png",
        ".wav": "audio/wav",
        ".mp3": "audio/mpeg",
    }

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, formato, *args):  # silencio: consola limpia
        pass


def puerto_libre(puertos):
    for puerto in puertos:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                s.bind(("127.0.0.1", puerto))
                return puerto
            except OSError:
                continue
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def main():
    os.chdir(RAIZ)
    puerto = puerto_libre(PUERTOS)
    url = f"http://localhost:{puerto}/"

    servidor = http.server.ThreadingHTTPServer(("127.0.0.1", puerto), Handler)

    print()
    print("  ============================================")
    print("   NUBY STUDIO")
    print("  ============================================")
    print()
    print(f"  Abriendo: {url}")
    print()
    print("  NO CIERRES ESTA VENTANA mientras trabajes.")
    print("  Esta ventana es el motor que sirve el estudio.")
    print("  Cuando termines, ciérrala para apagarlo.")
    print()

    threading.Timer(0.8, lambda: webbrowser.open(url)).start()

    try:
        servidor.serve_forever()
    except KeyboardInterrupt:
        print("\n  Estudio detenido. ¡Hasta la próxima grabación!")
    finally:
        servidor.server_close()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:  # noqa: BLE001
        print(f"\n  No se pudo iniciar: {error}")
        print("  Prueba a instalar Python 3 desde https://www.python.org/downloads/")
        input("\n  Pulsa Enter para cerrar…")
        sys.exit(1)

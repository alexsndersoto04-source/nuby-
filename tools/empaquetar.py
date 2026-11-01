#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Empaqueta Nuby Studio en un ZIP listo para descargar y abrir con doble clic.

Uso:
    python3 tools/empaquetar.py

Resultado:
    dist/Nuby-Studio-1.0.zip

El ZIP contiene la app, los lanzadores (Windows/macOS/Linux) y un LEEME.
"""

import os
import shutil
import zipfile

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DESTINO = os.path.join(RAIZ, "dist")
NOMBRE = "Nuby-Studio-1.0.zip"
CARPETA = "Nuby Studio"

# Archivos y carpetas de la app que entran en el paquete.
INCLUIR = [
    "index.html",
    "manifest.webmanifest",
    "sw.js",
    "server.js",
    "package.json",
    "README.md",
    "LICENSE-lamejs.txt",
    "css",
    "js",
    "assets",
]

# Lanzadores: se renombran al copiarlos.
LANZADORES = {
    "iniciar.py": "iniciar.py",
    "ABRIR-EN-WINDOWS.bat": "ABRIR-EN-WINDOWS.bat",
    "ABRIR-EN-MAC.command": "ABRIR-EN-MAC.command",
    "ABRIR-EN-LINUX.sh": "ABRIR-EN-LINUX.sh",
    "LEEME-PRIMERO.txt": "LEEME-PRIMERO.txt",
}

# Archivos que deben conservar permiso de ejecución dentro del ZIP.
EJECUTABLES = {"iniciar.py", "ABRIR-EN-MAC.command", "ABRIR-EN-LINUX.sh"}


def agregar(archivo_zip, ruta_local, ruta_destino, ejecutable=False):
    info = zipfile.ZipInfo(ruta_destino)
    info.compress_type = zipfile.ZIP_DEFLATED
    modo = 0o755 if ejecutable else 0o644
    info.external_attr = (0o100000 | modo) << 16
    info.date_time = (2026, 1, 1, 0, 0, 0)
    with open(ruta_local, "rb") as origen, archivo_zip.open(info, "w") as salida:
        shutil.copyfileobj(origen, salida)


def main():
    os.makedirs(DESTINO, exist_ok=True)
    ruta_zip = os.path.join(DESTINO, NOMBRE)
    total = 0

    with zipfile.ZipFile(ruta_zip, "w", zipfile.ZIP_DEFLATED) as z:
        for elemento in INCLUIR:
            ruta = os.path.join(RAIZ, elemento)
            if not os.path.exists(ruta):
                print(f"  aviso: falta {elemento}, se omite")
                continue
            if os.path.isfile(ruta):
                agregar(z, ruta, f"{CARPETA}/{elemento}")
                total += 1
            else:
                for base, _dirs, archivos in os.walk(ruta):
                    for nombre in archivos:
                        local = os.path.join(base, nombre)
                        relativo = os.path.relpath(local, RAIZ).replace(os.sep, "/")
                        agregar(z, local, f"{CARPETA}/{relativo}")
                        total += 1

        for origen, destino in LANZADORES.items():
            local = os.path.join(RAIZ, "launcher", origen)
            if os.path.exists(local):
                agregar(z, local, f"{CARPETA}/{destino}", ejecutable=destino in EJECUTABLES)
                total += 1

    tam = os.path.getsize(ruta_zip) / 1024
    print(f"Listo: {ruta_zip}")
    print(f"  {total} archivos, {tam:.0f} KB")


if __name__ == "__main__":
    main()

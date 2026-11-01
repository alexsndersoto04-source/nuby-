#!/bin/bash
cd "$(dirname "$0")" || exit 1
echo ""
echo "  NUBY STUDIO"
echo "  ------------------------------------------"
echo "  No cierres esta ventana mientras trabajes."
echo "  ------------------------------------------"
echo ""
if command -v python3 >/dev/null 2>&1; then
  python3 iniciar.py
elif command -v node >/dev/null 2>&1; then
  open "http://localhost:4173/"
  node server.js
else
  echo "  No se encontró Python 3 ni Node.js."
  echo "  Instala Python 3 desde https://www.python.org/downloads/"
  read -n 1 -s -r -p "  Pulsa una tecla para cerrar…"
fi

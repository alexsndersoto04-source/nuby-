#!/bin/sh
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
  (xdg-open "http://localhost:4173/" >/dev/null 2>&1 &) ; node server.js
else
  echo "  No se encontró Python 3 ni Node.js."
  echo "  Instala Python 3 desde https://www.python.org/downloads/"
fi

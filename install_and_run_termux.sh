#!/bin/bash
set -e

echo -e "\033[1;36m========================================================\033[0m"
echo -e "\033[1;32m       Compilando y Ejecutando Nuby en tu Terminal      \033[0m"
echo -e "\033[1;36m========================================================\033[0m"

# Si estamos en Termux o Linux, detectar compilador C++
if command -v clang++ &> /dev/null; then
    export CXX=clang++
elif command -v g++ &> /dev/null; then
    export CXX=g++
else
    echo "Instalando compilador..."
    pkg install clang make git -y || true
    export CXX=clang++
fi

# Detectar carpeta de trabajo
if [ -d "nuby_engine" ]; then
    cd nuby_engine
fi

echo "Compilador detectado: $CXX"
echo "Compilando Nuby en C++20 con optimización -O3..."
make clean
make all

PORT=8085
echo -e "\n\033[1;32m[✔] ¡Compilación 100% exitosa!\033[0m"
echo -e "Arrancando Nuby en el puerto $PORT...\n"
echo -e "Abre tu navegador (en tu teléfono) y entra a:"
echo -e "👉 \033[1;34mhttp://localhost:$PORT\033[0m o \033[1;34mhttp://127.0.0.1:$PORT\033[0m\n"

./bin/nuby_engine --port $PORT

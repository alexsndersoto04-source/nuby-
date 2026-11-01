@echo off
chcp 65001 >nul
title Nuby Studio
cd /d "%~dp0"
echo.
echo   NUBY STUDIO
echo   ------------------------------------------------------
echo   No cierres esta ventana mientras trabajas.
echo   ------------------------------------------------------
echo.
where py >nul 2>nul && goto :usar_py
where python >nul 2>nul && goto :usar_python
where node >nul 2>nul && goto :usar_node
goto :sin_runtime

:usar_py
py iniciar.py
goto :fin

:usar_python
python iniciar.py
goto :fin

:usar_node
echo   Usando Node.js.
start "" http://localhost:4173/
node server.js
goto :fin

:sin_runtime
echo   No se encontro Python ni Node.js en este equipo.
echo.
echo   Instala Python 3 (gratis) y vuelve a intentarlo:
echo   https://www.python.org/downloads/
echo.
echo   IMPORTANTE: al instalar, marca la casilla "Add Python to PATH".
goto :fin

:fin
echo.
echo   La ventana se puede cerrar cuando termines de grabar.
pause

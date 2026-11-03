#!/usr/bin/env python3
"""Genera el corpus de VERIFICACION DEL MOTOR en español (dominio público).

ADVERTENCIA IMPORTANTE
-----------------------
Este corpus NO sirve para medir calidad de modelo. Es el mismo texto base
repetido con cuatro variantes de superficie, así que un modelo lo memoriza y
cualquier BPC medido aquí es optimista. Se usa solo para comprobar que el
motor aprende algo y que el régimen es estable.

Para una cifra honesta de BPC hace falta un corpus real y grande, con
particiones disjuntas (ver `kaggle/` y ESTADO.md).

Por eso se generan DOS ficheros disjuntos:
  data/spanish_small.txt    -> entrenamiento (variantes A y B)
  data/spanish_holdout.txt  -> test, texto NUNCA visto (variantes C y D)
Las variantes de test cambian la superficie (minúsculas, sustituciones), de
modo que medir sobre ellas prueba generalización y no recuerdo literal.
"""
import os
import sys

BASE = """En un lugar de la Mancha, de cuyo nombre no quiero acordarme, no ha mucho
tiempo que vivia un hidalgo de los de lanza en astillero, adarga antigua,
rocin flaco y galgo corredor. Una olla de algo mas vaca que carnero, salpicon
las mas noches, duelos y quebrantos los sabados, lantejas los viernes, algun
palomino de añadidura los domingos, consumian las tres partes de su hacienda.

El resto della concluian sayo de velarte, calzas de velludo para las fiestas,
con sus pantuflos de lo mesmo, y los dias de entresemana se honraba con su
vellorí de lo mas fino. Tenia en su casa una ama que pasaba de los cuarenta, y
una sobrina que no llegaba a los veinte, y un mozo de campo y plaza.

Frisaba la edad de nuestro hidalgo con los cincuenta años. Era de complexion
recia, seco de carnes, enjuto de rostro, gran madrugador y amigo de la caza.
Quieren decir que tenia el sobrenombre de Quijada, o Quesada, que en esto hay
alguna diferencia en los autores que deste caso escriben.

"""


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")
    os.makedirs(root, exist_ok=True)

    # Entrenamiento: dos variantes de superficie.
    train_variants = [BASE, BASE.replace("\n", " ")]
    train = "".join(train_variants) * 40

    # Test: dos variantes de superficie que NO aparecen en entrenamiento.
    hold_variants = [BASE.lower(), BASE.replace("que", "qve")]
    holdout = "".join(hold_variants) * 10

    for name, text in (("spanish_small.txt", train), ("spanish_holdout.txt", holdout)):
        path = os.path.join(root, name)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"{path}: {len(text)} caracteres")


if __name__ == "__main__":
    sys.exit(main())

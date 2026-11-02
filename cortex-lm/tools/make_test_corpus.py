#!/usr/bin/env python3
"""Genera un corpus de prueba en español (dominio público, incluido aquí).

Sirve para verificar que el motor aprende. No sirve para medir calidad final.
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
    out = os.path.join(os.path.dirname(__file__), "..", "data", "spanish_small.txt")
    variants = [BASE, BASE.replace("\n", " "), BASE.lower(),
                BASE.replace("que", "qve")]
    text = "".join(variants) * 40
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"{out}: {len(text)} caracteres")

if __name__ == "__main__":
    sys.exit(main())

# Datos

El corpus no se versiona (son decenas de MB). En Kaggle se monta como dataset.

## Corpus de prueba (local)

Para verificar que el motor aprende, sin red:

```
python3 tools/make_test_corpus.py
```

Genera `spanish_small.txt` (~300 KB) a partir de texto de dominio público
incluido en el script.

## Corpus real (fase 2)

| Fuente | Tamaño | Dónde |
|---|---|---|
| Gutenberg español | 50–100 MB | gutenberg.org |
| Wikipedia ES | 500 MB+ | dumps.wikimedia.org |

En Kaggle: subir como Dataset y montar en `/kaggle/input/`.
No descargar nunca en el teléfono.

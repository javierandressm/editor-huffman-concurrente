# Editor de texto con Huffman concurrente

**Autor:** Javier Andrés Sierra Machado  
**Curso:** Sistemas Operativos, Universidad EAFIT  
**Parcial 2 — Alternativa 1**

Editor CLI en C para POSIX/Linux que incorpora compresión y descompresión Huffman por bloques con pthread.

La sustentación v4 incluye las demostraciones del editor, pruebas de integridad, procesamiento por bloques, sincronización y cierre de recursos. Consulta [la revisión y los capítulos](docs/REVISION_VIDEO.md).

## Descargas

[Video final en Drive](https://drive.google.com/file/d/1_BKVOteCt5HR5_ZXE5QCgXYV_zsN4btQ/view). El código puede descargarse con Code → Download ZIP en GitHub.

## Compilar y probar en Linux

Requisitos: GCC, Make, pthread y herramientas habituales de una distribución Linux.

```bash
make
make test
./editor
```

Las pruebas terminan con:

```text
OK: editor, Huffman binario, casos limite y restricciones POSIX.
```

## Uso

| Orden | Operación |
|---|---|
| `o archivo.txt` | Abrir o crear el documento |
| `a texto` | Añadir una línea y persistirla |
| `p [n]` | Consultar todo o una línea |
| `d n` | Eliminar una línea |
| `e` | Consultar la estructura de la lista |
| `c [salida.huf]` | Comprimir el documento abierto en segundo plano |
| `x entrada.huf [salida]` | Descomprimir en segundo plano |
| `q` | Esperar la operación activa y cerrar |

Espera la finalización de `c` antes de solicitar `x`. Los destinos deben ser nuevos: no se sobrescriben archivos existentes. Evita espacios en las rutas de los argumentos de descompresión.

Para comprobar una recuperación:

```bash
cmp original.txt restaurado.txt
sha256sum original.txt restaurado.txt
```

## Diseño y concurrencia

- El hilo principal atiende la entrada y presenta progreso con `poll`.
- `compresor_iniciar` crea el hilo que ejecuta `hilo_operacion`.
- La operación crea un grupo de hasta cuatro trabajadores, según los procesadores disponibles.
- Los bloques tienen 64 KiB. Primero se cuentan frecuencias mediante histogramas por tarea y reducción; después se construye un árbol global y se codifican los bloques en paralelo.
- El mutex del grupo protege el reparto de tareas y sus contadores. Las condiciones `tareas_disponibles` y `lote_completo` permiten esperar sin espera activa.
- Los trabajadores leen mediante `pread` y generan resultados por tarea. El hilo de operación escribe cada lote en el orden de los índices originales.
- El origen se protege mediante restricciones de edición, una copia temporal estable, `flock` cooperativo y comprobaciones de metadatos. La salida temporal se publica sin reemplazo.
- La descompresión valida el formato, las longitudes, las frecuencias y el CRC-32 de los bloques.

## Estructura

```text
src/
  editor_main.c       Interfaz y ciclo de comandos
  editor.c/.h         Operaciones de edición
  lista.c/.h          Lista de líneas
  archivo.c/.h        Persistencia
  compresor.c/.h      Huffman y sincronización
Makefile
tests/pruebas.sh
docs/
video/README.md
```

## Informe del Parcial 2

- [Informe completo en Word](docs/Informe_Parcial_2_Huffman.docx).
- [Informe completo para leer en GitHub](docs/Informe_Parcial_2_Huffman.md).

Explica la arquitectura, Huffman por bloques, mutex y condiciones, formato, llamadas del sistema, uso, pruebas, recursos y límites conocidos.

## Video y documentación

- [Sustentación v4 en Drive](https://drive.google.com/file/d/1_BKVOteCt5HR5_ZXE5QCgXYV_zsN4btQ/view) — aproximadamente 13 min 18 s.
- [Revisión del video y capítulos](docs/REVISION_VIDEO.md).
- [Manual de usuario](docs/02_Manual_Usuario.md).
- [Manual de desarrollador](docs/03_Manual_Desarrollador.md).
- [Informe original del Parcial 1](docs/01_Informe_Tecnico.pdf). Este PDF corresponde al editor original; no es un informe actualizado del Parcial 2.

## Verificación y límites

El video incluye ejecución en Linux, `make test`, comparación `cmp` y hashes. Esa evidencia no sustituye una auditoría completa de carreras o memoria. Las pruebas automáticas cubren el editor, bytes binarios de varios bloques, archivo vacío, símbolo único, destino existente y corrupción.

No se afirma manejo completo de señales externas como SIGTERM: ese punto requiere revisión adicional. El cierre normal con `q` espera la operación y recoge sus hilos. `flock` es cooperativo; no bloquea las escrituras de aplicaciones que lo ignoren.

La copia del video en Drive conserva 1080p y pesa unos 85 MB. El video se distribuye por separado del código.
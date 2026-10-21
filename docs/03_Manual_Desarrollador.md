# Manual técnico para el desarrollador

## 1. Decisión arquitectónica

El editor es un ejecutable independiente. `editor_main.c` contiene `main()` y
coordina tres módulos: edición, lista y archivo. Por indicación del profesor no
se modifica ni se entrega el Shell de clase.

```text
editor_main.c -> editor.c -> lista.c
       |                 -> archivo.c -> kernel
       +-> compresor.c -> pthreads -> bloques Huffman
```

La separación evita que el análisis de comandos, la estructura de datos y la
persistencia queden mezclados en un único archivo.

## 2. Estructura de datos

Cada línea se representa con un nodo dinámico:

```c
typedef struct NodoLinea {
    char *texto;
    size_t longitud;
    struct NodoLinea *anterior;
    struct NodoLinea *siguiente;
} NodoLinea;
```

La lista conserva ambos extremos y su cantidad:

```c
typedef struct {
    NodoLinea *primera;
    NodoLinea *ultima;
    size_t cantidad;
} ListaLineas;
```

Invariantes importantes:

- Si la lista está vacía, `primera` y `ultima` son `NULL` y `cantidad` es 0.
- El primer nodo tiene `anterior == NULL`.
- El último nodo tiene `siguiente == NULL`.
- `cantidad` coincide con el número real de nodos alcanzables.

## 3. Estado global del editor

```c
typedef struct {
    int fd;
    char *nombre;
    ListaLineas lineas;
} Editor;
```

`fd == -1` significa que no existe un archivo abierto. El descriptor no guarda
el contenido: es un número que identifica una entrada del proceso administrada
por el kernel. `lineas` es la representación temporal del documento en RAM.

## 4. Memoria

- `comando[2048]` y `bloque[4096]` son arreglos automáticos en el stack.
- Cada `NodoLinea` y cada `texto` se reservan en el heap con `malloc`.
- El buffer temporal de una línea crece con `realloc` durante la lectura.
- `lista_borrar` libera el texto y el nodo borrado.
- `lista_vaciar` libera todos los nodos al cambiar de archivo o salir.

Un buffer es una región temporal de RAM usada para transportar bytes. El
buffer de 4096 bytes no es el archivo: `read` copia allí un bloque y después el
programa distribuye sus bytes entre nodos.

## 5. Llamadas POSIX usadas para el documento

| Interfaz | Uso | Retorno esencial |
|---|---|---|
| `open()` | Abre o crea con `O_RDWR | O_CREAT, 0644` | `fd >= 0`; `-1` error |
| `read()` | Copia bytes del archivo a un buffer | bytes; `0` EOF; `-1` error |
| `write()` | Copia bytes desde RAM al archivo o FD 1 | bytes escritos; `-1` error |
| `lseek()` | Cambia el offset de la descripción abierta | nuevo offset; `-1` error |
| `ftruncate()` | Cambia el tamaño del archivo | `0` éxito; `-1` error |
| `close()` | Libera el descriptor del proceso | `0` éxito; `-1` error |
| `pread()` | Lee un bloque en una posición sin mover el offset compartido | bytes; `-1` error |
| `fstat()` / `stat()` | Comprueba identidad, tamaño y fechas del archivo | `0` éxito; `-1` error |
| `flock()` | Mantiene un bloqueo compartido durante la operación | `0` éxito; `-1` error |
| `mkstemp()` / `unlink()` | Crea y limpia las instantáneas y salidas temporales | descriptor / `0`; `-1` error |
| `fsync()` / `link()` | Confirma los bytes y publica la salida completa sin reemplazo | `0` éxito; `-1` error |

Rigurosamente, el programa llama interfaces POSIX expuestas por la biblioteca
del sistema. Sus wrappers realizan la transición de modo usuario a modo kernel.

## 6. Funciones de biblioteca utilizadas

### `<stdio.h>`

| Función | Uso |
|---|---|
| `printf()` | Banner, prompt y mensajes |
| `fprintf()` | Mensaje de uso por `stderr` |
| `fflush()` | Muestra el prompt antes de esperar entrada |
| `perror()` | Explica el `errno` de una operación fallida |
| `snprintf()` / `vsnprintf()` | Formatea rutas y mensajes acotados |

### `<stdlib.h>`

| Función | Uso |
|---|---|
| `malloc()` | Reserva nodos y textos |
| `calloc()` | Reserva descriptores de bloques inicializados en cero |
| `realloc()` | Amplía el buffer temporal de una línea |
| `free()` | Libera buffers, textos y nodos |
| `strtol()` | Convierte el número de `p n` y `d n` |
| `getenv()` | Lee el directorio temporal opcional `TMPDIR` |

### `<string.h>`

| Función | Uso |
|---|---|
| `strcmp()` | Compara comandos completos |
| `strncmp()` | Reconoce prefijos como `o ` y `a ` |
| `strpbrk()` | Busca separadores dentro de los comandos |
| `strcspn()` | Localiza CR o LF para sustituirlo por `\0` |
| `strlen()` | Calcula la longitud del texto añadido |
| `memcpy()` / `memcmp()` | Copia bytes o compara la firma del formato |
| `memset()` | Inicializa buffers, histogramas y bloques |
| `strdup()` | Conserva los nombres de archivo y los argumentos del hilo |

Estas funciones no son llamadas directas al kernel. En especial, `malloc` y
`free` pertenecen al asignador de memoria de la biblioteca.

## 7. Flujo de cada comando

### `o archivo`

```text
editor_abrir -> open -> lseek(SEEK_SET) -> read por bloques
             -> separar por LF -> malloc/memcpy -> enlazar nodos
```

La carga se hace primero sobre una lista temporal. El archivo anterior solo se
cierra después de que la nueva carga termina correctamente.

### `p [n]`

```text
recorrer u obtener nodo -> write(STDOUT_FILENO, texto) -> write("\n")
```

### `a texto`

```text
malloc -> enlazar al final -> lseek(SEEK_END) -> write(texto) -> write("\n")
```

Si el archivo no terminaba en LF, se escribe primero ese separador.

### `d n`

```text
obtener nodo -> reconectar vecinos -> free
             -> lseek(SEEK_SET) -> ftruncate(0) -> reescribir nodos
```

La reescritura materializa el desplazamiento de los bytes posteriores.

### `q`

```text
close(fd) -> lista_vaciar -> free(texto) -> free(nodo) -> return de main
```

## 8. Offset, LF y truncamiento

- El **offset** es la posición usada por el próximo `read` o `write`.
- `SEEK_SET` interpreta el desplazamiento desde el inicio.
- `SEEK_END` lo interpreta desde el tamaño actual del archivo.
- LF (`'\n'`, byte 10) separa las líneas del archivo Unix.
- Truncar a 0 cambia el tamaño lógico del archivo a cero; luego se escriben las
  líneas que permanecen en la lista.

`lseek` solo reposiciona el offset: no lee, no escribe y no cambia por sí sola
el tamaño del archivo.

## 9. Funciones propias

| Módulo | Funciones |
|---|---|
| `editor_main.c` | `main`, `mostrar_bienvenida`, `mostrar_resultado`, `convertir_numero_linea` |
| `editor.c` | `editor_inicializar`, `editor_abrir`, `editor_imprimir`, `editor_agregar`, `editor_borrar`, `editor_mostrar_estructura`, `editor_cerrar` |
| `lista.c` | `lista_inicializar`, `lista_vaciar`, `lista_agregar`, `lista_obtener`, `lista_borrar` |
| `archivo.c` | `archivo_abrir`, `archivo_cerrar`, `archivo_cargar`, `archivo_agregar_linea`, `archivo_guardar` |

Además, `editor.c`, `archivo.c` y `compresor.c` contienen auxiliares `static`,
visibles solo dentro de su propia unidad de compilación.

## 10. Compresión Huffman concurrente

`c [salida.huf]` toma el nombre del archivo abierto y arranca un hilo de
operación. Ese hilo crea una instantánea temporal con `open`, `read`, `write` y
`unlink`; revisa tamaño, identidad y fechas de cambio antes de continuar y antes
de publicar el resultado. También mantiene un bloqueo compartido `flock`. El
editor rechaza `a`, `d` y `o` durante la operación. Así, la compresión trabaja
con los bytes de una copia estable y las órdenes del editor no modifican el
archivo mientras se procesa.

El hilo de operación crea hasta cuatro trabajadores. Cada trabajador espera en
`pthread_cond_wait` hasta recibir un lote. El coordinador entrega como máximo
un bloque por trabajador, los despierta con `pthread_cond_broadcast` y espera
el lote en otra variable de condición. Un mutex protege la cola, los contadores
y los resultados compartidos. Los trabajadores leen posiciones distintas con
`pread`, sin compartir el offset del descriptor.

La primera pasada cuenta en paralelo las frecuencias de los 256 bytes. El
coordinador suma los histogramas y construye un árbol determinista, con
desempates por frecuencia y símbolo. En la segunda pasada, los trabajadores
codifican bloques de 64 KiB con ese árbol. Los resultados ocupan una ventana
de hasta cuatro bloques; el coordinador los escribe en orden y libera la
memoria antes de despachar el siguiente lote. El formato guarda el tamaño
original, las frecuencias globales, la longitud en bits y un CRC-32 por bloque.

`x archivo.huf [salida]` valida cabecera y longitudes antes de crear la salida.
Los trabajadores decodifican bloques independientes y el coordinador los
escribe en orden. Se comprueban CRC-32 y frecuencias reconstruidas.

Ambas operaciones escriben primero junto al destino en un archivo temporal.
`fsync` y `link` publican el resultado completo sin reemplazar un archivo
existente; si falla una etapa, se elimina el temporal. El progreso y el estado
se comparten con mutex y variable de condición. El hilo principal espera la
entrada con `poll`, que permite mostrar el progreso sin bloquear el editor ni
hacer espera activa.

Un archivo vacío usa un árbol vacío y cero bloques. Cuando todos los bytes son
iguales, se asigna un código de un bit al símbolo. La cabecera usa enteros
little-endian y el identificador `SOHUF001`.

## 11. Compilación

`editor.c` no contiene `main()`, por eso no debe compilarse solo. Use:

```bash
make
```

Equivalente manual:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -pthread \
    src/editor_main.c src/editor.c src/lista.c src/archivo.c \
    src/compresor.c -o editor
```

Después ejecute `make test` para validar el comportamiento completo.

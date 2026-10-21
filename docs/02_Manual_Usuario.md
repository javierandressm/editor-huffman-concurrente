# Manual de usuario

## 1. Preparación

Se necesita Linux, `gcc` y `make`. Abra una terminal dentro de la carpeta del
proyecto y ejecute:

```bash
make
./editor
```

Al iniciar aparecerán el banner y el prompt:

```text
editor>
```

## 2. Abrir o crear

```text
editor> o notas.txt
```

Si `notas.txt` existe, sus líneas se cargan en memoria. Si no existe, se crea
con permisos solicitados `0644`.

También puede abrirse directamente:

```bash
./editor notas.txt
```

## 3. Añadir líneas

```text
editor> a Primera linea
editor> a Segunda linea
```

Cada orden añade el texto como una línea nueva al final y lo guarda de
inmediato en el archivo.

## 4. Imprimir

Todo el documento:

```text
editor> p
```

Solo la línea 2:

```text
editor> p 2
```

La numeración comienza en 1.

## 5. Borrar

```text
editor> d 2
```

Elimina la segunda línea y guarda inmediatamente el resultado.

## 6. Ver la estructura didáctica

```text
editor> e
```

Muestra las direcciones de los nodos y sus enlaces `anterior` y `siguiente`.
Este comando es adicional y sirve para estudiar la lista enlazada.

## 7. Salir

```text
editor> q
```

El programa cierra el descriptor abierto, libera los nodos de memoria y
termina.

## 8. Comprimir y descomprimir

Con un archivo abierto, ejecute:

```text
editor> c
```

El editor crea `nombre-del-archivo.huf`. También puede indicar otro destino:

```text
editor> c copia.huf
```

Para descomprimir:

```text
editor> x copia.huf resultado.txt
```

Si se omite `resultado.txt`, el editor elimina la extensión `.huf` del nombre
de entrada. La operación corre en segundo plano y el porcentaje aparece en el
prompt. Se puede imprimir el documento durante el proceso; el editor bloquea
los cambios y el cambio de archivo hasta que termine. El destino debe ser un
archivo nuevo.

## 9. Ejemplo completo

```text
editor> o demo.txt
editor> a Uno
editor> a Dos
editor> a Tres
editor> p
Uno
Dos
Tres
editor> p 2
Dos
editor> d 2
editor> p
Uno
Tres
editor> c
editor> x demo.txt.huf demo-restaurado.txt
editor> q
```

Compare el original y el archivo restaurado byte a byte con
`cmp demo.txt demo-restaurado.txt`.

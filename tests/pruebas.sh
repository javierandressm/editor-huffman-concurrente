#!/bin/sh
set -eu

directorio_pruebas=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
proyecto=$(CDPATH= cd -- "$directorio_pruebas/.." && pwd)
editor_bin="$proyecto/editor"
prueba_dir=$(mktemp -d)
trap 'rm -rf "$prueba_dir"' EXIT

cd "$prueba_dir"

# 1. Flujo completo: abrir, agregar, imprimir, borrar y salir.
"$editor_bin" > salida_base.txt <<'COMANDOS'
o notas.txt
a Uno
a Dos
a Tres
p
p 2
d 2
p
q
COMANDOS

printf 'Uno\nTres\n' > esperado.txt
cmp notas.txt esperado.txt
grep -Fq 'by Javier Sierra' salida_base.txt
grep -Fq 'editor> Dos' salida_base.txt
grep -Fq 'Linea borrada.' salida_base.txt
grep -Fq 'Saliendo del editor.' salida_base.txt

# 2. Archivo cuya ultima linea no tenia LF.
printf 'Original' > sin_lf.txt
"$editor_bin" sin_lf.txt > salida_sin_lf.txt <<'COMANDOS'
a Nueva
p
q
COMANDOS

printf 'Original\nNueva\n' > esperado_sin_lf.txt
cmp sin_lf.txt esperado_sin_lf.txt

# 3. Casos borde: archivo no abierto y numeros invalidos.
"$editor_bin" > salida_bordes.txt <<'COMANDOS'
p
o bordes.txt
p 0
p abc
d 99
q
COMANDOS

grep -Fq 'Primero abre un archivo' salida_bordes.txt
test "$(grep -Fc 'Usa un numero de linea entero desde 1.' salida_bordes.txt)" -eq 2
grep -Fq 'La linea no existe.' salida_bordes.txt

# 4. El archivo tambien puede recibirse al iniciar el ejecutable.
"$editor_bin" argumento.txt > salida_argumento.txt <<'COMANDOS'
a DesdeArgumento
q
COMANDOS

printf 'DesdeArgumento\n' > esperado_argumento.txt
cmp argumento.txt esperado_argumento.txt

# 5. Huffman conserva bytes binarios y procesa varios bloques.
dd if=/dev/urandom of=aleatorio.bin bs=65536 count=16 2>/dev/null
"$editor_bin" aleatorio.bin > salida_compresion.txt <<'COMANDOS'
c
q
COMANDOS
test -s aleatorio.bin.huf
"$editor_bin" > salida_descompresion.txt <<'COMANDOS'
x aleatorio.bin.huf aleatorio-restaurado.bin
q
COMANDOS
cmp aleatorio.bin aleatorio-restaurado.bin

# 6. Casos límite: archivo vacío y un solo símbolo.
: > vacio.bin
"$editor_bin" vacio.bin > salida_vacio.txt <<'COMANDOS'
c
q
COMANDOS
"$editor_bin" > salida_vacio_descomprimido.txt <<'COMANDOS'
x vacio.bin.huf vacio-restaurado.bin
q
COMANDOS
cmp vacio.bin vacio-restaurado.bin

dd if=/dev/zero of=un-simbolo.bin bs=65536 count=3 2>/dev/null
"$editor_bin" un-simbolo.bin > salida_simbolo.txt <<'COMANDOS'
c
q
COMANDOS
"$editor_bin" > salida_simbolo_descomprimido.txt <<'COMANDOS'
x un-simbolo.bin.huf un-simbolo-restaurado.bin
q
COMANDOS
cmp un-simbolo.bin un-simbolo-restaurado.bin

# 7. Un destino existente se conserva.
printf 'Conservar este contenido\n' > protegido.bin
cp protegido.bin protegido-esperado.bin
"$editor_bin" aleatorio.bin > salida_protegido.txt <<'COMANDOS'
c protegido.bin
q
COMANDOS
cmp protegido.bin protegido-esperado.bin

# 8. La corrupción se detecta y no deja una salida parcial.
cp un-simbolo.bin.huf corrupto.huf
printf '\377' | dd of=corrupto.huf bs=1 seek=2092 conv=notrunc 2>/dev/null
"$editor_bin" > salida_corrupto.txt <<'COMANDOS'
x corrupto.huf corrupto-restaurado.bin
q
COMANDOS
grep -Fq 'Error en la operacion:' salida_corrupto.txt
test ! -e corrupto-restaurado.bin

# 9. La manipulacion del documento no usa I/O de alto nivel prohibido.
if grep -En '\<(fopen|fread|fwrite|fclose)\>' \
    "$proyecto/src/editor_main.c" "$proyecto/src/editor.c" \
    "$proyecto/src/compresor.c" \
    "$proyecto/src/lista.c" "$proyecto/src/archivo.c"; then
    printf 'ERROR: se encontro una funcion de I/O prohibida.\n' >&2
    exit 1
fi

printf 'OK: editor, Huffman binario, casos limite y restricciones POSIX.\n'

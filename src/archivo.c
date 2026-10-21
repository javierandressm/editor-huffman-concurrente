#define _POSIX_C_SOURCE 200809L
#include "archivo.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#define BLOQUE 4096

static int escribir_todo(int fd, const void *datos, size_t cantidad) {
    const char *posicion = datos;

    while (cantidad > 0) {
        /* [KERNEL] write transfiere bytes al descriptor indicado. */
        ssize_t escritos = write(fd, posicion, cantidad);
        if (escritos <= 0) return -1;

        posicion += escritos;
        cantidad -= (size_t)escritos;
    }

    return 0;
}

static int agregar_byte(char **linea, size_t *longitud,
                        size_t *capacidad, char byte) {
    if (*longitud == *capacidad) {
        size_t nueva_capacidad = (*capacidad == 0) ? 128 : *capacidad * 2;

        /* [BIBLIOTECA C] realloc aumenta el buffer conservando sus bytes. */
        char *nuevo = realloc(*linea, nueva_capacidad);
        if (nuevo == NULL) return -1;

        *linea = nuevo;
        *capacidad = nueva_capacidad;
    }

    (*linea)[*longitud] = byte;
    (*longitud)++;
    return 0;
}

int archivo_abrir(const char *nombre) {
    /* [KERNEL] open pide abrir el archivo para lectura y escritura.
       O_CREAT lo crea con modo 0644 cuando no existe. */
    return open(nombre, O_RDWR | O_CREAT, 0644);
}

int archivo_cerrar(int fd) {
    /* [KERNEL] close libera el descriptor de este proceso. */
    return close(fd);
}

int archivo_cargar(int fd, ListaLineas *destino) {
    char bloque[BLOQUE];
    char *linea = NULL;
    size_t longitud = 0;
    size_t capacidad = 0;
    ssize_t leidos;

    /* [KERNEL] lseek mueve el offset del archivo al byte cero. */
    if (lseek(fd, 0, SEEK_SET) == -1) return -1;

    /* [KERNEL] read copia bloques del archivo hacia la RAM. */
    while ((leidos = read(fd, bloque, sizeof(bloque))) > 0) {
        for (ssize_t i = 0; i < leidos; i++) {
            if (bloque[i] == '\n') {
                if (lista_agregar(destino, linea == NULL ? "" : linea,
                                  longitud) == -1) {
                    free(linea);
                    lista_vaciar(destino);
                    return -1;
                }

                longitud = 0;
            } else if (agregar_byte(&linea, &longitud,
                                    &capacidad, bloque[i]) == -1) {
                free(linea);
                lista_vaciar(destino);
                return -1;
            }
        }
    }

    if (leidos == -1) {
        free(linea);
        lista_vaciar(destino);
        return -1;
    }

    /* Una ultima linea sin LF tambien pertenece al documento. */
    if (longitud > 0 && lista_agregar(destino, linea, longitud) == -1) {
        free(linea);
        lista_vaciar(destino);
        return -1;
    }

    free(linea);
    return 0;
}

int archivo_agregar_linea(int fd, const char *texto, size_t longitud) {
    /* Guardamos el tamano anterior para poder deshacer una escritura fallida. */
    off_t fin_original = lseek(fd, 0, SEEK_END);
    if (fin_original == (off_t)-1) return -1;

    int necesita_separador = 0;

    /* Si la ultima linea del archivo no terminaba en LF, primero escribimos
       ese separador para que el texto nuevo sea realmente otra linea. */
    if (fin_original > 0) {
        char ultimo_byte;

        if (lseek(fd, -1, SEEK_END) == (off_t)-1) return -1;
        if (read(fd, &ultimo_byte, 1) != 1) return -1;
        necesita_separador = (ultimo_byte != '\n');

        if (lseek(fd, 0, SEEK_END) == (off_t)-1) return -1;
    }

    if ((necesita_separador && escribir_todo(fd, "\n", 1) == -1) ||
        escribir_todo(fd, texto, longitud) == -1 ||
        escribir_todo(fd, "\n", 1) == -1) {
        int error_original = errno;

        /* Intento de rollback: el archivo vuelve al tamano que tenia antes. */
        (void)ftruncate(fd, fin_original);
        (void)lseek(fd, 0, SEEK_END);
        errno = error_original;
        return -1;
    }

    return 0;
}

int archivo_guardar(int fd, const ListaLineas *lista) {
    /* Primero regresamos al inicio y dejamos el archivo con tamaño cero. */
    if (lseek(fd, 0, SEEK_SET) == -1) return -1;

    /* [KERNEL] ftruncate cambia el tamaño del archivo a cero bytes. */
    if (ftruncate(fd, 0) == -1) return -1;

    for (NodoLinea *actual = lista->primera;
         actual != NULL;
         actual = actual->siguiente) {
        if (escribir_todo(fd, actual->texto, actual->longitud) == -1) {
            return -1;
        }

        if (escribir_todo(fd, "\n", 1) == -1) return -1;
    }

    return 0;
}

#ifndef LISTA_H
#define LISTA_H

#include <stddef.h>

/* Cada nodo representa una linea del documento en la memoria RAM. */
typedef struct NodoLinea {
    char *texto;
    size_t longitud;
    struct NodoLinea *anterior;
    struct NodoLinea *siguiente;
} NodoLinea;

/* La lista conoce sus dos extremos y el numero de lineas. */
typedef struct {
    NodoLinea *primera;
    NodoLinea *ultima;
    size_t cantidad;
} ListaLineas;

void lista_inicializar(ListaLineas *lista);
void lista_vaciar(ListaLineas *lista);
int lista_agregar(ListaLineas *lista, const char *texto, size_t longitud);
NodoLinea *lista_obtener(const ListaLineas *lista, size_t numero);
int lista_borrar(ListaLineas *lista, size_t numero);

#endif

#include "lista.h"

#include <stdlib.h>
#include <string.h>

void lista_inicializar(ListaLineas *lista) {
    lista->primera = NULL;
    lista->ultima = NULL;
    lista->cantidad = 0;
}

void lista_vaciar(ListaLineas *lista) {
    NodoLinea *actual = lista->primera;

    while (actual != NULL) {
        NodoLinea *siguiente = actual->siguiente;

        /* [BIBLIOTECA C] free devuelve al asignador la memoria pedida
           anteriormente con malloc. */
        free(actual->texto);
        free(actual);
        actual = siguiente;
    }

    lista_inicializar(lista);
}

int lista_agregar(ListaLineas *lista, const char *texto, size_t longitud) {
    /* [BIBLIOTECA C] malloc reserva memoria dinamica para un nodo. */
    NodoLinea *nuevo = malloc(sizeof(*nuevo));
    if (nuevo == NULL) return -1;

    /* Se reserva un byte adicional para el terminador '\0'. */
    nuevo->texto = malloc(longitud + 1);
    if (nuevo->texto == NULL) {
        free(nuevo);
        return -1;
    }

    /* [BIBLIOTECA C] memcpy copia los bytes del texto al nuevo nodo. */
    memcpy(nuevo->texto, texto, longitud);
    nuevo->texto[longitud] = '\0';
    nuevo->longitud = longitud;
    nuevo->anterior = lista->ultima;
    nuevo->siguiente = NULL;

    if (lista->ultima == NULL) {
        /* La lista estaba vacia: el nodo es primero y ultimo. */
        lista->primera = nuevo;
    } else {
        lista->ultima->siguiente = nuevo;
    }

    lista->ultima = nuevo;
    lista->cantidad++;
    return 0;
}

NodoLinea *lista_obtener(const ListaLineas *lista, size_t numero) {
    if (numero < 1 || numero > lista->cantidad) return NULL;

    /* Al ser doblemente enlazada, elegimos el extremo mas cercano. */
    if (numero <= (lista->cantidad + 1) / 2) {
        NodoLinea *actual = lista->primera;

        for (size_t i = 1; i < numero; i++) {
            actual = actual->siguiente;
        }

        return actual;
    }

    NodoLinea *actual = lista->ultima;

    for (size_t i = lista->cantidad; i > numero; i--) {
        actual = actual->anterior;
    }

    return actual;
}

int lista_borrar(ListaLineas *lista, size_t numero) {
    NodoLinea *objetivo = lista_obtener(lista, numero);
    if (objetivo == NULL) return 1;

    if (objetivo->anterior == NULL) {
        lista->primera = objetivo->siguiente;
    } else {
        objetivo->anterior->siguiente = objetivo->siguiente;
    }

    if (objetivo->siguiente == NULL) {
        lista->ultima = objetivo->anterior;
    } else {
        objetivo->siguiente->anterior = objetivo->anterior;
    }

    free(objetivo->texto);
    free(objetivo);
    lista->cantidad--;
    return 0;
}

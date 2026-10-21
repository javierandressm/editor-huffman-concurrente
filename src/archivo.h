#ifndef ARCHIVO_H
#define ARCHIVO_H

#include "lista.h"

int archivo_abrir(const char *nombre);
int archivo_cerrar(int fd);
int archivo_cargar(int fd, ListaLineas *destino);
int archivo_agregar_linea(int fd, const char *texto, size_t longitud);
int archivo_guardar(int fd, const ListaLineas *lista);

#endif

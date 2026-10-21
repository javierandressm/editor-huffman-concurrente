#ifndef EDITOR_H
#define EDITOR_H

#include "lista.h"

typedef struct {
    int fd;
    char *nombre;
    ListaLineas lineas;
} Editor;

void editor_inicializar(Editor *editor);
int editor_abrir(Editor *editor, const char *nombre);
int editor_imprimir(const Editor *editor, long numero);
int editor_agregar(Editor *editor, const char *texto);
int editor_borrar(Editor *editor, long numero);
void editor_mostrar_estructura(const Editor *editor);
int editor_cerrar(Editor *editor);

#endif

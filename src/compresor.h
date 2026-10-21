#ifndef COMPRESOR_H
#define COMPRESOR_H

#include <pthread.h>
#include <stdint.h>

#define COMPRESOR_TEXTO_MAX 4096
#define COMPRESOR_MENSAJE_MAX 256

typedef enum {
    COMPRESOR_COMPRIMIR = 1,
    COMPRESOR_DESCOMPRIMIR = 2
} CompresorTipo;

typedef struct {
    pthread_t hilo;
    pthread_mutex_t mutex;
    pthread_cond_t cambio;
    int sincronizado;
    int hilo_creado;
    int activo;
    int terminado;
    int exito;
    CompresorTipo tipo;
    unsigned hilos_trabajo;
    uint64_t progreso;
    uint64_t total;
    char salida[COMPRESOR_TEXTO_MAX];
    char mensaje[COMPRESOR_MENSAJE_MAX];
} CompresorOperacion;

typedef struct {
    int activo;
    int terminado;
    int exito;
    CompresorTipo tipo;
    unsigned hilos_trabajo;
    uint64_t progreso;
    uint64_t total;
    char salida[COMPRESOR_TEXTO_MAX];
    char mensaje[COMPRESOR_MENSAJE_MAX];
} CompresorEstado;

int compresor_inicializar(CompresorOperacion *operacion);
int compresor_iniciar(CompresorOperacion *operacion, CompresorTipo tipo,
                      const char *origen, const char *destino);
int compresor_obtener_estado(CompresorOperacion *operacion,
                             CompresorEstado *estado);
int compresor_recolectar(CompresorOperacion *operacion,
                         CompresorEstado *resultado);
int compresor_esperar(CompresorOperacion *operacion,
                      CompresorEstado *resultado);
void compresor_destruir(CompresorOperacion *operacion);

#endif

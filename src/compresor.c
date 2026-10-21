#define _POSIX_C_SOURCE 200809L
#include "compresor.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAMANO_BLOQUE 65536U
#define MAX_HILOS_TRABAJO 4U
#define TAMANO_CABECERA_FIJA 28U
#define TAMANO_CABECERA_BLOQUE 16U

static const unsigned char MAGIA_HUFFMAN[8] = {
    'S', 'O', 'H', 'U', 'F', '0', '0', '1'
};

typedef struct {
    uint64_t frecuencia;
    int simbolo;
    int izquierdo;
    int derecho;
    unsigned minimo_simbolo;
    unsigned orden;
} NodoHuffman;

typedef struct {
    unsigned char bits[256];
    unsigned short longitud;
} CodigoHuffman;

typedef struct {
    unsigned char *datos;
    size_t capacidad;
    uint64_t bits;
} EscritorBits;

typedef struct {
    unsigned char *datos;
    size_t bytes;
    uint64_t bits;
    uint32_t longitud_original;
    uint32_t crc;
} BloqueCodificado;

typedef struct {
    uint64_t posicion;
    uint64_t bits;
    uint64_t desplazamiento;
    uint32_t longitud_original;
    uint32_t crc;
} BloqueArchivo;

typedef enum {
    TAREA_FRECUENCIAS,
    TAREA_CODIFICAR,
    TAREA_DECODIFICAR
} TipoTarea;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t tareas_disponibles;
    pthread_cond_t lote_completo;
    pthread_t hilos[MAX_HILOS_TRABAJO];
    unsigned cantidad_hilos;
    unsigned hilos_creados;
    int detener;
    uint64_t generacion;
    unsigned siguiente_tarea;
    unsigned cantidad_tareas;
    unsigned pendientes;
    size_t bloques[MAX_HILOS_TRABAJO];
    TipoTarea tipo;
    int error;
    char mensaje_error[COMPRESOR_MENSAJE_MAX];
    int archivo_fd;
    uint64_t tamano_original;
    uint64_t *frecuencias;
    NodoHuffman *arbol;
    int raiz;
    CodigoHuffman *codigos;
    BloqueArchivo *descriptores;
    uint64_t progreso_base;
    CompresorOperacion *operacion;
    uint64_t frecuencias_lote[MAX_HILOS_TRABAJO][256];
    BloqueCodificado codificados[MAX_HILOS_TRABAJO];
    unsigned char *decodificados[MAX_HILOS_TRABAJO];
} GrupoHilos;

typedef struct {
    CompresorOperacion *operacion;
    CompresorTipo tipo;
    char *origen;
    char *destino;
    unsigned hilos_trabajo;
} ArgumentosOperacion;

static void establecer_error(char *destino, size_t capacidad,
                             const char *formato, ...) {
    va_list argumentos;

    va_start(argumentos, formato);
    (void)vsnprintf(destino, capacidad, formato, argumentos);
    va_end(argumentos);
}

static void establecer_error_errno(char *destino, size_t capacidad,
                                   const char *accion) {
    int error = errno;
    establecer_error(destino, capacidad, "%s: %s", accion, strerror(error));
}

static int escribir_todo(int fd, const void *datos, size_t cantidad) {
    const unsigned char *posicion = datos;

    while (cantidad > 0) {
        ssize_t escritos = write(fd, posicion, cantidad);
        if (escritos == -1 && errno == EINTR) continue;
        if (escritos <= 0) {
            if (escritos == 0) errno = EIO;
            return -1;
        }

        posicion += (size_t)escritos;
        cantidad -= (size_t)escritos;
    }

    return 0;
}

static int leer_exactamente_en(int fd, void *datos, size_t cantidad,
                               uint64_t desplazamiento) {
    unsigned char *posicion = datos;

    while (cantidad > 0) {
        if (desplazamiento > (uint64_t)INT64_MAX) {
            errno = EOVERFLOW;
            return -1;
        }

        ssize_t leidos = pread(fd, posicion, cantidad, (off_t)desplazamiento);
        if (leidos == -1 && errno == EINTR) continue;
        if (leidos <= 0) {
            if (leidos == 0) errno = EINVAL;
            return -1;
        }

        posicion += (size_t)leidos;
        cantidad -= (size_t)leidos;
        desplazamiento += (uint64_t)leidos;
    }

    return 0;
}

static void guardar_u32(unsigned char salida[4], uint32_t valor) {
    for (unsigned i = 0; i < 4; i++) {
        salida[i] = (unsigned char)(valor >> (i * 8U));
    }
}

static void guardar_u64(unsigned char salida[8], uint64_t valor) {
    for (unsigned i = 0; i < 8; i++) {
        salida[i] = (unsigned char)(valor >> (i * 8U));
    }
}

static uint32_t cargar_u32(const unsigned char entrada[4]) {
    uint32_t valor = 0;
    for (unsigned i = 0; i < 4; i++) {
        valor |= (uint32_t)entrada[i] << (i * 8U);
    }
    return valor;
}

static uint64_t cargar_u64(const unsigned char entrada[8]) {
    uint64_t valor = 0;
    for (unsigned i = 0; i < 8; i++) {
        valor |= (uint64_t)entrada[i] << (i * 8U);
    }
    return valor;
}

static void actualizar_progreso(CompresorOperacion *operacion,
                                uint64_t cantidad) {
    (void)pthread_mutex_lock(&operacion->mutex);
    if (UINT64_MAX - operacion->progreso < cantidad) {
        operacion->progreso = operacion->total;
    } else {
        operacion->progreso += cantidad;
        if (operacion->progreso > operacion->total) {
            operacion->progreso = operacion->total;
        }
    }
    (void)pthread_cond_broadcast(&operacion->cambio);
    (void)pthread_mutex_unlock(&operacion->mutex);
}

static int nodo_menor(const NodoHuffman *a, const NodoHuffman *b) {
    if (a->frecuencia != b->frecuencia) {
        return a->frecuencia < b->frecuencia;
    }
    if (a->minimo_simbolo != b->minimo_simbolo) {
        return a->minimo_simbolo < b->minimo_simbolo;
    }
    return a->orden < b->orden;
}

static int construir_arbol(const uint64_t frecuencias[256],
                           NodoHuffman nodos[511], size_t *cantidad_nodos,
                           int *raiz, char *error, size_t capacidad_error) {
    unsigned char activos[511] = {0};
    size_t cantidad_activos = 0;
    unsigned orden = 0;

    *cantidad_nodos = 0;
    *raiz = -1;

    for (unsigned simbolo = 0; simbolo < 256; simbolo++) {
        if (frecuencias[simbolo] == 0) continue;

        size_t indice = (*cantidad_nodos)++;
        nodos[indice].frecuencia = frecuencias[simbolo];
        nodos[indice].simbolo = (int)simbolo;
        nodos[indice].izquierdo = -1;
        nodos[indice].derecho = -1;
        nodos[indice].minimo_simbolo = simbolo;
        nodos[indice].orden = orden++;
        activos[indice] = 1;
        cantidad_activos++;
    }

    if (cantidad_activos == 0) return 0;

    while (cantidad_activos > 1) {
        int primero = -1;
        int segundo = -1;

        for (size_t i = 0; i < *cantidad_nodos; i++) {
            if (!activos[i]) continue;
            if (primero == -1 || nodo_menor(&nodos[i], &nodos[primero])) {
                segundo = primero;
                primero = (int)i;
            } else if (segundo == -1 || nodo_menor(&nodos[i], &nodos[segundo])) {
                segundo = (int)i;
            }
        }

        if (primero == -1 || segundo == -1 || *cantidad_nodos >= 511) {
            establecer_error(error, capacidad_error,
                             "No se pudo construir el arbol Huffman.");
            return -1;
        }
        if (UINT64_MAX - nodos[primero].frecuencia < nodos[segundo].frecuencia) {
            establecer_error(error, capacidad_error,
                             "La suma de frecuencias excede el limite permitido.");
            return -1;
        }

        size_t padre = (*cantidad_nodos)++;
        nodos[padre].frecuencia = nodos[primero].frecuencia +
                                  nodos[segundo].frecuencia;
        nodos[padre].simbolo = -1;
        nodos[padre].izquierdo = primero;
        nodos[padre].derecho = segundo;
        nodos[padre].minimo_simbolo =
            nodos[primero].minimo_simbolo < nodos[segundo].minimo_simbolo
                ? nodos[primero].minimo_simbolo
                : nodos[segundo].minimo_simbolo;
        nodos[padre].orden = orden++;

        activos[primero] = 0;
        activos[segundo] = 0;
        activos[padre] = 1;
        cantidad_activos--;
    }

    for (size_t i = 0; i < *cantidad_nodos; i++) {
        if (activos[i]) {
            *raiz = (int)i;
            return 0;
        }
    }

    establecer_error(error, capacidad_error,
                     "No se encontro la raiz del arbol Huffman.");
    return -1;
}

static int asignar_codigos_recursivo(const NodoHuffman nodos[511], int indice,
                                    unsigned char camino[256], unsigned profundidad,
                                    CodigoHuffman codigos[256],
                                    char *error, size_t capacidad_error) {
    const NodoHuffman *nodo = &nodos[indice];

    if (nodo->simbolo >= 0) {
        if (profundidad == 0) {
            camino[0] = 0;
            profundidad = 1;
        }
        memcpy(codigos[nodo->simbolo].bits, camino, profundidad);
        codigos[nodo->simbolo].longitud = (unsigned short)profundidad;
        return 0;
    }

    if (profundidad >= 255 || nodo->izquierdo < 0 || nodo->derecho < 0) {
        establecer_error(error, capacidad_error,
                         "El arbol Huffman tiene una estructura invalida.");
        return -1;
    }

    camino[profundidad] = 0;
    if (asignar_codigos_recursivo(nodos, nodo->izquierdo, camino,
                                  profundidad + 1, codigos,
                                  error, capacidad_error) == -1) {
        return -1;
    }

    camino[profundidad] = 1;
    return asignar_codigos_recursivo(nodos, nodo->derecho, camino,
                                     profundidad + 1, codigos,
                                     error, capacidad_error);
}

static int construir_codigos(const uint64_t frecuencias[256],
                             NodoHuffman nodos[511], int *raiz,
                             CodigoHuffman codigos[256],
                             char *error, size_t capacidad_error) {
    size_t cantidad_nodos;
    unsigned char camino[256];

    memset(codigos, 0, sizeof(CodigoHuffman) * 256U);
    if (construir_arbol(frecuencias, nodos, &cantidad_nodos, raiz,
                        error, capacidad_error) == -1) {
        return -1;
    }
    (void)cantidad_nodos;

    if (*raiz == -1) return 0;
    return asignar_codigos_recursivo(nodos, *raiz, camino, 0,
                                     codigos, error, capacidad_error);
}

static int escribir_bit(EscritorBits *escritor, unsigned bit) {
    if (escritor->bits == UINT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    uint64_t byte_indice = escritor->bits / 8U;
    if (byte_indice > SIZE_MAX - 1U) {
        errno = EOVERFLOW;
        return -1;
    }

    size_t necesario = (size_t)byte_indice + 1U;
    if (necesario > escritor->capacidad) {
        size_t nueva = escritor->capacidad == 0 ? 256U : escritor->capacidad;
        while (nueva < necesario) {
            if (nueva > SIZE_MAX / 2U) {
                nueva = necesario;
                break;
            }
            nueva *= 2U;
        }

        unsigned char *datos = realloc(escritor->datos, nueva);
        if (datos == NULL) return -1;
        memset(datos + escritor->capacidad, 0, nueva - escritor->capacidad);
        escritor->datos = datos;
        escritor->capacidad = nueva;
    }

    if (bit != 0) {
        unsigned desplazamiento = 7U - (unsigned)(escritor->bits % 8U);
        escritor->datos[byte_indice] |= (unsigned char)(1U << desplazamiento);
    }
    escritor->bits++;
    return 0;
}

static uint32_t calcular_crc32(const unsigned char *datos, size_t longitud) {
    uint32_t crc = UINT32_C(0xffffffff);

    for (size_t i = 0; i < longitud; i++) {
        crc ^= datos[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            uint32_t mascara = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mascara);
        }
    }

    return crc ^ UINT32_C(0xffffffff);
}

static int codificar_bloque(const unsigned char *entrada, size_t longitud,
                            const CodigoHuffman codigos[256],
                            BloqueCodificado *salida) {
    EscritorBits escritor = {0};

    for (size_t i = 0; i < longitud; i++) {
        const CodigoHuffman *codigo = &codigos[entrada[i]];
        if (codigo->longitud == 0) {
            free(escritor.datos);
            errno = EINVAL;
            return -1;
        }

        for (unsigned bit = 0; bit < codigo->longitud; bit++) {
            if (escribir_bit(&escritor, codigo->bits[bit]) == -1) {
                free(escritor.datos);
                return -1;
            }
        }
    }

    salida->datos = escritor.datos;
    salida->bits = escritor.bits;
    salida->bytes = (size_t)(escritor.bits / 8U +
                            (escritor.bits % 8U != 0 ? 1U : 0U));
    salida->longitud_original = (uint32_t)longitud;
    salida->crc = calcular_crc32(entrada, longitud);
    return 0;
}

static uint64_t longitud_bloque(uint64_t tamano, size_t indice) {
    uint64_t inicio = (uint64_t)indice * TAMANO_BLOQUE;
    uint64_t restante = tamano - inicio;
    return restante < TAMANO_BLOQUE ? restante : TAMANO_BLOQUE;
}

static int decodificar_bloque(int fd, const BloqueArchivo *descriptor,
                              const NodoHuffman nodos[511], int raiz,
                              unsigned char **salida) {
    size_t bytes_codificados = (size_t)(descriptor->bits / 8U +
                             (descriptor->bits % 8U != 0 ? 1U : 0U));
    unsigned char *datos_codificados = NULL;
    unsigned char *datos_originales = malloc(descriptor->longitud_original);
    int resultado = -1;

    if (datos_originales == NULL) return -1;
    if (bytes_codificados > 0) {
        datos_codificados = malloc(bytes_codificados);
        if (datos_codificados == NULL) goto fin;
        if (leer_exactamente_en(fd, datos_codificados, bytes_codificados,
                                descriptor->desplazamiento) == -1) {
            goto fin;
        }
    }

    if (descriptor->bits % 8U != 0 && bytes_codificados > 0) {
        unsigned usados = (unsigned)(descriptor->bits % 8U);
        unsigned char mascara = (unsigned char)((1U << (8U - usados)) - 1U);
        if ((datos_codificados[bytes_codificados - 1U] & mascara) != 0) {
            errno = EINVAL;
            goto fin;
        }
    }

    if (raiz < 0) {
        errno = EINVAL;
        goto fin;
    }

    if (nodos[raiz].simbolo >= 0) {
        if (descriptor->bits != descriptor->longitud_original) {
            errno = EINVAL;
            goto fin;
        }
        memset(datos_originales, nodos[raiz].simbolo,
               descriptor->longitud_original);
        for (uint64_t bit = 0; bit < descriptor->bits; bit++) {
            unsigned posicion = 7U - (unsigned)(bit % 8U);
            if (((datos_codificados[bit / 8U] >> posicion) & 1U) != 0) {
                errno = EINVAL;
                goto fin;
            }
        }
    } else {
        size_t producidos = 0;
        int actual = raiz;

        for (uint64_t bit = 0; bit < descriptor->bits; bit++) {
            unsigned posicion = 7U - (unsigned)(bit % 8U);
            unsigned valor = (datos_codificados[bit / 8U] >> posicion) & 1U;
            actual = valor == 0 ? nodos[actual].izquierdo
                                : nodos[actual].derecho;
            if (actual < 0) {
                errno = EINVAL;
                goto fin;
            }

            if (nodos[actual].simbolo >= 0) {
                if (producidos >= descriptor->longitud_original) {
                    errno = EINVAL;
                    goto fin;
                }
                datos_originales[producidos++] =
                    (unsigned char)nodos[actual].simbolo;
                actual = raiz;
            }
        }

        if (producidos != descriptor->longitud_original || actual != raiz) {
            errno = EINVAL;
            goto fin;
        }
    }

    if (calcular_crc32(datos_originales, descriptor->longitud_original) !=
        descriptor->crc) {
        errno = EINVAL;
        goto fin;
    }

    *salida = datos_originales;
    datos_originales = NULL;
    resultado = 0;

fin:
    free(datos_codificados);
    free(datos_originales);
    return resultado;
}

static int procesar_tarea(GrupoHilos *grupo, unsigned ranura,
                          size_t indice_bloque, char *error,
                          size_t capacidad_error) {
    uint64_t longitud64;
    uint64_t desplazamiento;
    unsigned char *datos;
    size_t longitud;

    if (grupo->tipo == TAREA_DECODIFICAR) {
        if (decodificar_bloque(grupo->archivo_fd,
                               &grupo->descriptores[indice_bloque],
                               grupo->arbol, grupo->raiz,
                               &grupo->decodificados[ranura]) == -1) {
            establecer_error_errno(error, capacidad_error,
                                   "No se pudo decodificar un bloque");
            return -1;
        }
        return 0;
    }

    longitud64 = longitud_bloque(grupo->tamano_original, indice_bloque);
    desplazamiento = (uint64_t)indice_bloque * TAMANO_BLOQUE;
    longitud = (size_t)longitud64;
    datos = malloc(longitud);
    if (datos == NULL) {
        establecer_error(error, capacidad_error,
                         "No hay memoria suficiente para procesar un bloque.");
        return -1;
    }

    if (leer_exactamente_en(grupo->archivo_fd, datos, longitud,
                            desplazamiento) == -1) {
        establecer_error_errno(error, capacidad_error,
                               "No se pudo leer un bloque de entrada");
        free(datos);
        return -1;
    }

    if (grupo->tipo == TAREA_FRECUENCIAS) {
        uint64_t *frecuencias = grupo->frecuencias_lote[ranura];
        memset(frecuencias, 0, sizeof(grupo->frecuencias_lote[ranura]));
        for (size_t i = 0; i < longitud; i++) frecuencias[datos[i]]++;
    } else {
        if (codificar_bloque(datos, longitud, grupo->codigos,
                             &grupo->codificados[ranura]) == -1) {
            establecer_error_errno(error, capacidad_error,
                                   "No se pudo codificar un bloque");
            free(datos);
            return -1;
        }
        actualizar_progreso(grupo->operacion, longitud64);
    }

    free(datos);
    if (grupo->tipo == TAREA_FRECUENCIAS) {
        actualizar_progreso(grupo->operacion, longitud64);
    }
    return 0;
}

static void *hilo_trabajador(void *argumento) {
    GrupoHilos *grupo = argumento;
    uint64_t ultima_generacion = 0;

    for (;;) {
        (void)pthread_mutex_lock(&grupo->mutex);
        while (!grupo->detener && grupo->generacion == ultima_generacion) {
            (void)pthread_cond_wait(&grupo->tareas_disponibles, &grupo->mutex);
        }
        if (grupo->detener) {
            (void)pthread_mutex_unlock(&grupo->mutex);
            return NULL;
        }
        ultima_generacion = grupo->generacion;
        (void)pthread_mutex_unlock(&grupo->mutex);

        for (;;) {
            unsigned ranura;
            size_t indice_bloque;

            (void)pthread_mutex_lock(&grupo->mutex);
            if (grupo->siguiente_tarea >= grupo->cantidad_tareas) {
                (void)pthread_mutex_unlock(&grupo->mutex);
                break;
            }
            ranura = grupo->siguiente_tarea++;
            indice_bloque = grupo->bloques[ranura];
            (void)pthread_mutex_unlock(&grupo->mutex);

            char error[COMPRESOR_MENSAJE_MAX] = "";
            int fallo = procesar_tarea(grupo, ranura, indice_bloque,
                                       error, sizeof(error));

            (void)pthread_mutex_lock(&grupo->mutex);
            if (fallo == -1 && !grupo->error) {
                grupo->error = 1;
                (void)snprintf(grupo->mensaje_error,
                               sizeof(grupo->mensaje_error), "%s", error);
            }
            if (grupo->pendientes > 0) grupo->pendientes--;
            if (grupo->pendientes == 0) {
                (void)pthread_cond_signal(&grupo->lote_completo);
            }
            (void)pthread_mutex_unlock(&grupo->mutex);
        }
    }
}

static int iniciar_grupo(GrupoHilos *grupo, unsigned cantidad,
                         char *error, size_t capacidad_error) {
    int codigo;

    memset(grupo, 0, sizeof(*grupo));
    grupo->cantidad_hilos = cantidad;
    codigo = pthread_mutex_init(&grupo->mutex, NULL);
    if (codigo != 0) {
        establecer_error(error, capacidad_error,
                         "No se pudo inicializar el mutex de trabajadores: %s",
                         strerror(codigo));
        return -1;
    }
    codigo = pthread_cond_init(&grupo->tareas_disponibles, NULL);
    if (codigo != 0) {
        establecer_error(error, capacidad_error,
                         "No se pudo inicializar la condicion de tareas: %s",
                         strerror(codigo));
        (void)pthread_mutex_destroy(&grupo->mutex);
        return -1;
    }
    codigo = pthread_cond_init(&grupo->lote_completo, NULL);
    if (codigo != 0) {
        establecer_error(error, capacidad_error,
                         "No se pudo inicializar la condicion de finalizacion: %s",
                         strerror(codigo));
        (void)pthread_cond_destroy(&grupo->tareas_disponibles);
        (void)pthread_mutex_destroy(&grupo->mutex);
        return -1;
    }

    for (unsigned i = 0; i < cantidad; i++) {
        codigo = pthread_create(&grupo->hilos[i], NULL,
                                hilo_trabajador, grupo);
        if (codigo != 0) {
            establecer_error(error, capacidad_error,
                             "No se pudo crear un hilo trabajador: %s",
                             strerror(codigo));
            (void)pthread_mutex_lock(&grupo->mutex);
            grupo->detener = 1;
            (void)pthread_cond_broadcast(&grupo->tareas_disponibles);
            (void)pthread_mutex_unlock(&grupo->mutex);
            for (unsigned j = 0; j < grupo->hilos_creados; j++) {
                (void)pthread_join(grupo->hilos[j], NULL);
            }
            (void)pthread_cond_destroy(&grupo->lote_completo);
            (void)pthread_cond_destroy(&grupo->tareas_disponibles);
            (void)pthread_mutex_destroy(&grupo->mutex);
            return -1;
        }
        grupo->hilos_creados++;
    }

    return 0;
}

static void detener_grupo(GrupoHilos *grupo) {
    (void)pthread_mutex_lock(&grupo->mutex);
    grupo->detener = 1;
    (void)pthread_cond_broadcast(&grupo->tareas_disponibles);
    (void)pthread_mutex_unlock(&grupo->mutex);

    for (unsigned i = 0; i < grupo->hilos_creados; i++) {
        (void)pthread_join(grupo->hilos[i], NULL);
    }

    (void)pthread_cond_destroy(&grupo->lote_completo);
    (void)pthread_cond_destroy(&grupo->tareas_disponibles);
    (void)pthread_mutex_destroy(&grupo->mutex);
}

static int despachar_lote(GrupoHilos *grupo, TipoTarea tipo,
                          const size_t *bloques, unsigned cantidad,
                          char *error, size_t capacidad_error) {
    (void)pthread_mutex_lock(&grupo->mutex);
    grupo->tipo = tipo;
    grupo->cantidad_tareas = cantidad;
    grupo->siguiente_tarea = 0;
    grupo->pendientes = cantidad;
    grupo->error = 0;
    grupo->mensaje_error[0] = '\0';

    for (unsigned i = 0; i < cantidad; i++) {
        grupo->bloques[i] = bloques[i];
        grupo->decodificados[i] = NULL;
        grupo->codificados[i].datos = NULL;
        grupo->codificados[i].bytes = 0;
        grupo->codificados[i].bits = 0;
    }

    grupo->generacion++;
    (void)pthread_cond_broadcast(&grupo->tareas_disponibles);
    while (grupo->pendientes > 0) {
        (void)pthread_cond_wait(&grupo->lote_completo, &grupo->mutex);
    }

    int hubo_error = grupo->error;
    if (hubo_error) {
        (void)snprintf(error, capacidad_error, "%s", grupo->mensaje_error);
    }
    (void)pthread_mutex_unlock(&grupo->mutex);
    return hubo_error ? -1 : 0;
}

static unsigned seleccionar_hilos(void) {
    long disponibles = sysconf(_SC_NPROCESSORS_ONLN);
    if (disponibles < 1) return 1;
    if ((unsigned long)disponibles > MAX_HILOS_TRABAJO) {
        return MAX_HILOS_TRABAJO;
    }
    return (unsigned)disponibles;
}

static int mismos_metadatos(const struct stat *a, const struct stat *b) {
    if (a->st_dev != b->st_dev || a->st_ino != b->st_ino ||
        a->st_size != b->st_size || a->st_mtime != b->st_mtime ||
        a->st_ctime != b->st_ctime) {
        return 0;
    }
#if defined(__linux__)
    if (a->st_mtim.tv_nsec != b->st_mtim.tv_nsec ||
        a->st_ctim.tv_nsec != b->st_ctim.tv_nsec) {
        return 0;
    }
#endif
    return 1;
}

static int verificar_archivo_estable(int fd, const char *ruta,
                                     const struct stat *inicial,
                                     char *error, size_t capacidad_error) {
    struct stat descriptor_actual;
    struct stat ruta_actual;

    if (fstat(fd, &descriptor_actual) == -1) {
        establecer_error_errno(error, capacidad_error,
                               "No se pudo revisar el archivo de entrada");
        return -1;
    }
    if (stat(ruta, &ruta_actual) == -1) {
        establecer_error(error, capacidad_error,
                         "El archivo de entrada cambio o desaparecio durante la operacion.");
        return -1;
    }
    if (!mismos_metadatos(inicial, &descriptor_actual) ||
        descriptor_actual.st_dev != ruta_actual.st_dev ||
        descriptor_actual.st_ino != ruta_actual.st_ino) {
        establecer_error(error, capacidad_error,
                         "El archivo de entrada fue modificado durante la operacion.");
        return -1;
    }
    return 0;
}

static int verificar_destino_nuevo(const char *origen, const struct stat *stat_origen,
                                   const char *destino, char *error,
                                   size_t capacidad_error) {
    struct stat stat_destino;

    if (strcmp(origen, destino) == 0) {
        establecer_error(error, capacidad_error,
                         "El destino no puede ser el mismo archivo de entrada.");
        return -1;
    }
    if (stat(destino, &stat_destino) == 0) {
        if (stat_destino.st_dev == stat_origen->st_dev &&
            stat_destino.st_ino == stat_origen->st_ino) {
            establecer_error(error, capacidad_error,
                             "El destino apunta al archivo de entrada.");
        } else {
            establecer_error(error, capacidad_error,
                             "El destino ya existe; elija otro nombre.");
        }
        return -1;
    }
    if (errno != ENOENT) {
        establecer_error_errno(error, capacidad_error,
                               "No se pudo revisar el destino");
        return -1;
    }
    return 0;
}

static int crear_snapshot(char **ruta_temporal) {
    const char *directorio = getenv("TMPDIR");
    if (directorio == NULL || directorio[0] == '\0') directorio = "/tmp";

    size_t longitud_directorio = strlen(directorio);
    const char *separador = longitud_directorio > 0 &&
                            directorio[longitud_directorio - 1U] == '/'
                                ? "" : "/";
    size_t longitud = longitud_directorio + strlen(separador) +
                      sizeof("sohuf-snapshot-XXXXXX");
    char *plantilla = malloc(longitud);
    if (plantilla == NULL) return -1;

    (void)snprintf(plantilla, longitud, "%s%ssohuf-snapshot-XXXXXX",
                   directorio, separador);
    int fd = mkstemp(plantilla);
    if (fd == -1) {
        free(plantilla);
        return -1;
    }
#if !defined(_WIN32)
    if (unlink(plantilla) == -1) {
        int error = errno;
        (void)close(fd);
        (void)unlink(plantilla);
        free(plantilla);
        errno = error;
        return -1;
    }
#endif
    *ruta_temporal = plantilla;
    return fd;
}

static int crear_temporal_destino(const char *destino, char **ruta_temporal) {
    size_t longitud = strlen(destino);
    static const char sufijo[] = ".tmp.XXXXXX";
    if (longitud > SIZE_MAX - sizeof(sufijo)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    char *plantilla = malloc(longitud + sizeof(sufijo));
    if (plantilla == NULL) return -1;
    (void)snprintf(plantilla, longitud + sizeof(sufijo), "%s%s",
                   destino, sufijo);

    int fd = mkstemp(plantilla);
    if (fd == -1) {
        free(plantilla);
        return -1;
    }
    *ruta_temporal = plantilla;
    return fd;
}

static int publicar_temporal(int *fd, const char *temporal,
                             const char *destino, char *error,
                             size_t capacidad_error) {
    if (fsync(*fd) == -1) {
        establecer_error_errno(error, capacidad_error,
                               "No se pudo sincronizar el archivo de salida");
        return -1;
    }
    if (close(*fd) == -1) {
        *fd = -1;
        establecer_error_errno(error, capacidad_error,
                               "No se pudo cerrar el archivo de salida");
        return -1;
    }
    *fd = -1;

    if (link(temporal, destino) == -1) {
        if (errno == EEXIST) {
            establecer_error(error, capacidad_error,
                             "El destino aparecio durante la operacion; no se reemplazo.");
        } else {
            establecer_error_errno(error, capacidad_error,
                                   "No se pudo publicar el archivo de salida");
        }
        return -1;
    }
    if (unlink(temporal) == -1) {
        int error_original = errno;
        (void)unlink(destino);
        errno = error_original;
        establecer_error_errno(error, capacidad_error,
                               "No se pudo limpiar el archivo temporal");
        return -1;
    }
    return 0;
}

static int copiar_a_snapshot(int origen_fd, int snapshot_fd,
                             uint64_t tamano, CompresorOperacion *operacion,
                             char *error, size_t capacidad_error) {
    unsigned char *bloque = malloc(TAMANO_BLOQUE);
    uint64_t copiados = 0;
    if (bloque == NULL) {
        establecer_error(error, capacidad_error,
                         "No hay memoria suficiente para crear la instantanea.");
        return -1;
    }

    while (copiados < tamano) {
        uint64_t restante = tamano - copiados;
        size_t solicitar = (size_t)(restante < TAMANO_BLOQUE
                                        ? restante : TAMANO_BLOQUE);
        ssize_t leidos;
        do {
            leidos = read(origen_fd, bloque, solicitar);
        } while (leidos == -1 && errno == EINTR);
        if (leidos <= 0) {
            if (leidos == 0) errno = EIO;
            establecer_error_errno(error, capacidad_error,
                                   "No se pudo copiar el archivo de entrada");
            free(bloque);
            return -1;
        }
        if (escribir_todo(snapshot_fd, bloque, (size_t)leidos) == -1) {
            establecer_error_errno(error, capacidad_error,
                                   "No se pudo guardar la instantanea");
            free(bloque);
            return -1;
        }
        copiados += (uint64_t)leidos;
        actualizar_progreso(operacion, (uint64_t)leidos);
    }

    free(bloque);
    return 0;
}

static void liberar_bloques_codificados(GrupoHilos *grupo, unsigned cantidad) {
    for (unsigned i = 0; i < cantidad; i++) {
        free(grupo->codificados[i].datos);
        grupo->codificados[i].datos = NULL;
    }
}

static int escribir_cabecera(int fd, uint64_t tamano, uint64_t cantidad_bloques,
                              const uint64_t frecuencias[256]) {
    unsigned char campo[8];
    unsigned char tamano_bloque[4];

    if (escribir_todo(fd, MAGIA_HUFFMAN, sizeof(MAGIA_HUFFMAN)) == -1) return -1;
    guardar_u32(tamano_bloque, TAMANO_BLOQUE);
    if (escribir_todo(fd, tamano_bloque, sizeof(tamano_bloque)) == -1) return -1;
    guardar_u64(campo, tamano);
    if (escribir_todo(fd, campo, sizeof(campo)) == -1) return -1;
    guardar_u64(campo, cantidad_bloques);
    if (escribir_todo(fd, campo, sizeof(campo)) == -1) return -1;

    for (unsigned simbolo = 0; simbolo < 256; simbolo++) {
        guardar_u64(campo, frecuencias[simbolo]);
        if (escribir_todo(fd, campo, sizeof(campo)) == -1) return -1;
    }
    return 0;
}

static int comprimir_archivo(ArgumentosOperacion *argumentos,
                             char *error, size_t capacidad_error) {
    int origen_fd = -1;
    int snapshot_fd = -1;
    int salida_fd = -1;
    char *ruta_snapshot = NULL;
    char *ruta_temporal = NULL;
    struct stat inicial;
    int grupo_activo = 0;
    GrupoHilos grupo;
    uint64_t frecuencias[256] = {0};
    NodoHuffman nodos[511];
    CodigoHuffman codigos[256];
    int raiz = -1;
    uint64_t cantidad_bloques64;
    size_t cantidad_bloques;
    unsigned hilos = argumentos->hilos_trabajo;
    int resultado = -1;

    origen_fd = open(argumentos->origen, O_RDONLY);
    if (origen_fd == -1) {
        establecer_error_errno(error, capacidad_error,
                               "No se pudo abrir el archivo de entrada");
        goto fin;
    }
    if (flock(origen_fd, LOCK_SH | LOCK_NB) == -1) {
        establecer_error_errno(error, capacidad_error,
                               "No se pudo proteger el archivo de entrada");
        goto fin;
    }
    if (fstat(origen_fd, &inicial) == -1) {
        establecer_error_errno(error, capacidad_error,
                               "No se pudo consultar el archivo de entrada");
        goto fin;
    }
    if (!S_ISREG(inicial.st_mode) || inicial.st_size < 0) {
        establecer_error(error, capacidad_error,
                         "La entrada debe ser un archivo regular.");
        goto fin;
    }
    struct stat ruta_inicial;
    if (stat(argumentos->origen, &ruta_inicial) == -1 ||
        ruta_inicial.st_dev != inicial.st_dev ||
        ruta_inicial.st_ino != inicial.st_ino) {
        establecer_error(error, capacidad_error,
                         "El archivo de entrada cambio o desaparecio.");
        goto fin;
    }
    if (verificar_destino_nuevo(argumentos->origen, &inicial,
                                argumentos->destino, error,
                                capacidad_error) == -1) {
        goto fin;
    }

    uint64_t tamano = (uint64_t)inicial.st_size;
    if (tamano > UINT64_MAX / 4U) {
        establecer_error(error, capacidad_error,
                         "El archivo supera el tamano maximo admitido.");
        goto fin;
    }
    cantidad_bloques64 = tamano / TAMANO_BLOQUE +
                         (tamano % TAMANO_BLOQUE != 0 ? 1U : 0U);
    if (cantidad_bloques64 > SIZE_MAX) {
        establecer_error(error, capacidad_error,
                         "El archivo contiene demasiados bloques para esta plataforma.");
        goto fin;
    }
    cantidad_bloques = (size_t)cantidad_bloques64;
    (void)pthread_mutex_lock(&argumentos->operacion->mutex);
    argumentos->operacion->total = tamano * 4U;
    (void)pthread_mutex_unlock(&argumentos->operacion->mutex);

    snapshot_fd = crear_snapshot(&ruta_snapshot);
    if (snapshot_fd == -1) {
        establecer_error_errno(error, capacidad_error,
                               "No se pudo crear la instantanea temporal");
        goto fin;
    }
    if (copiar_a_snapshot(origen_fd, snapshot_fd, tamano,
                          argumentos->operacion, error,
                          capacidad_error) == -1) {
        goto fin;
    }
    if (verificar_archivo_estable(origen_fd, argumentos->origen, &inicial,
                                  error, capacidad_error) == -1) {
        goto fin;
    }

    if (iniciar_grupo(&grupo, hilos, error, capacidad_error) == -1) goto fin;
    grupo_activo = 1;
    grupo.archivo_fd = snapshot_fd;
    grupo.tamano_original = tamano;
    grupo.operacion = argumentos->operacion;

    for (size_t inicio = 0; inicio < cantidad_bloques;) {
        size_t indices[MAX_HILOS_TRABAJO];
        unsigned cantidad = (unsigned)(cantidad_bloques - inicio);
        if (cantidad > hilos) cantidad = hilos;
        for (unsigned i = 0; i < cantidad; i++) indices[i] = inicio + i;

        if (despachar_lote(&grupo, TAREA_FRECUENCIAS, indices, cantidad,
                           error, capacidad_error) == -1) {
            goto fin;
        }
        for (unsigned tarea = 0; tarea < cantidad; tarea++) {
            for (unsigned simbolo = 0; simbolo < 256; simbolo++) {
                uint64_t cuenta = grupo.frecuencias_lote[tarea][simbolo];
                if (UINT64_MAX - frecuencias[simbolo] < cuenta) {
                    establecer_error(error, capacidad_error,
                                     "El conteo de frecuencias excedio el limite.");
                    goto fin;
                }
                frecuencias[simbolo] += cuenta;
            }
        }
        inicio += cantidad;
    }

    uint64_t suma_frecuencias = 0;
    for (unsigned simbolo = 0; simbolo < 256; simbolo++) {
        if (UINT64_MAX - suma_frecuencias < frecuencias[simbolo]) {
            establecer_error(error, capacidad_error,
                             "La suma de frecuencias excedio el limite.");
            goto fin;
        }
        suma_frecuencias += frecuencias[simbolo];
    }
    if (suma_frecuencias != tamano) {
        establecer_error(error, capacidad_error,
                         "La instantanea cambio durante el conteo de frecuencias.");
        goto fin;
    }
    if (construir_codigos(frecuencias, nodos, &raiz, codigos,
                          error, capacidad_error) == -1) {
        goto fin;
    }

    salida_fd = crear_temporal_destino(argumentos->destino, &ruta_temporal);
    if (salida_fd == -1) {
        establecer_error_errno(error, capacidad_error,
                               "No se pudo crear el archivo comprimido temporal");
        goto fin;
    }
    if (escribir_cabecera(salida_fd, tamano, cantidad_bloques64,
                          frecuencias) == -1) {
        establecer_error_errno(error, capacidad_error,
                               "No se pudo escribir la cabecera Huffman");
        goto fin;
    }

    grupo.codigos = codigos;
    for (size_t inicio = 0; inicio < cantidad_bloques;) {
        size_t indices[MAX_HILOS_TRABAJO];
        unsigned cantidad = (unsigned)(cantidad_bloques - inicio);
        if (cantidad > hilos) cantidad = hilos;
        for (unsigned i = 0; i < cantidad; i++) indices[i] = inicio + i;

        if (despachar_lote(&grupo, TAREA_CODIFICAR, indices, cantidad,
                           error, capacidad_error) == -1) {
            liberar_bloques_codificados(&grupo, cantidad);
            goto fin;
        }

        for (unsigned i = 0; i < cantidad; i++) {
            BloqueCodificado *bloque = &grupo.codificados[i];
            unsigned char cabecera[TAMANO_CABECERA_BLOQUE];
            guardar_u32(cabecera, bloque->longitud_original);
            guardar_u64(cabecera + 4, bloque->bits);
            guardar_u32(cabecera + 12, bloque->crc);

            if (escribir_todo(salida_fd, cabecera, sizeof(cabecera)) == -1 ||
                escribir_todo(salida_fd, bloque->datos, bloque->bytes) == -1) {
                establecer_error_errno(error, capacidad_error,
                                       "No se pudo escribir un bloque comprimido");
                liberar_bloques_codificados(&grupo, cantidad);
                goto fin;
            }
            actualizar_progreso(argumentos->operacion,
                                bloque->longitud_original);
            free(bloque->datos);
            bloque->datos = NULL;
        }
        inicio += cantidad;
    }

    if (verificar_archivo_estable(origen_fd, argumentos->origen, &inicial,
                                  error, capacidad_error) == -1) {
        goto fin;
    }
    if (publicar_temporal(&salida_fd, ruta_temporal, argumentos->destino,
                          error, capacidad_error) == -1) {
        goto fin;
    }
    resultado = 0;

fin:
    if (grupo_activo) detener_grupo(&grupo);
    if (salida_fd != -1) (void)close(salida_fd);
    if (ruta_temporal != NULL) {
        (void)unlink(ruta_temporal);
        free(ruta_temporal);
    }
    if (snapshot_fd != -1) (void)close(snapshot_fd);
    if (ruta_snapshot != NULL) (void)unlink(ruta_snapshot);
    free(ruta_snapshot);
    if (origen_fd != -1) (void)close(origen_fd);
    return resultado;
}

static int descomprimir_archivo(ArgumentosOperacion *argumentos,
                                char *error, size_t capacidad_error) {
    int entrada_fd = -1;
    int salida_fd = -1;
    char *ruta_temporal = NULL;
    struct stat inicial;
    GrupoHilos grupo;
    int grupo_activo = 0;
    uint64_t frecuencias[256] = {0};
    uint64_t frecuencias_observadas[256] = {0};
    NodoHuffman nodos[511];
    CodigoHuffman codigos[256];
    int raiz = -1;
    BloqueArchivo *descriptores = NULL;
    uint64_t cantidad_bloques64;
    size_t cantidad_bloques;
    uint64_t tamano_original;
    unsigned hilos = argumentos->hilos_trabajo;
    int resultado = -1;

    entrada_fd = open(argumentos->origen, O_RDONLY);
    if (entrada_fd == -1) {
        establecer_error_errno(error, capacidad_error,
                               "No se pudo abrir el archivo comprimido");
        goto fin;
    }
    if (flock(entrada_fd, LOCK_SH | LOCK_NB) == -1) {
        establecer_error_errno(error, capacidad_error,
                               "No se pudo proteger el archivo comprimido");
        goto fin;
    }
    if (fstat(entrada_fd, &inicial) == -1) {
        establecer_error_errno(error, capacidad_error,
                               "No se pudo consultar el archivo comprimido");
        goto fin;
    }
    if (!S_ISREG(inicial.st_mode) || inicial.st_size < 0) {
        establecer_error(error, capacidad_error,
                         "La entrada debe ser un archivo regular.");
        goto fin;
    }
    struct stat ruta_inicial;
    if (stat(argumentos->origen, &ruta_inicial) == -1 ||
        ruta_inicial.st_dev != inicial.st_dev ||
        ruta_inicial.st_ino != inicial.st_ino) {
        establecer_error(error, capacidad_error,
                         "El archivo comprimido cambio o desaparecio.");
        goto fin;
    }
    if (verificar_destino_nuevo(argumentos->origen, &inicial,
                                argumentos->destino, error,
                                capacidad_error) == -1) {
        goto fin;
    }
    if ((uint64_t)inicial.st_size < TAMANO_CABECERA_FIJA + 256U * 8U) {
        establecer_error(error, capacidad_error,
                         "El archivo no contiene una cabecera Huffman completa.");
        goto fin;
    }

    unsigned char cabecera[TAMANO_CABECERA_FIJA];
    if (leer_exactamente_en(entrada_fd, cabecera, sizeof(cabecera), 0) == -1) {
        establecer_error_errno(error, capacidad_error,
                               "No se pudo leer la cabecera Huffman");
        goto fin;
    }
    if (memcmp(cabecera, MAGIA_HUFFMAN, sizeof(MAGIA_HUFFMAN)) != 0) {
        establecer_error(error, capacidad_error,
                         "El archivo no tiene el formato Huffman de este editor.");
        goto fin;
    }
    if (cargar_u32(cabecera + 8) != TAMANO_BLOQUE) {
        establecer_error(error, capacidad_error,
                         "El archivo usa un tamano de bloque no compatible.");
        goto fin;
    }
    tamano_original = cargar_u64(cabecera + 12);
    cantidad_bloques64 = cargar_u64(cabecera + 20);
    uint64_t bloques_esperados = tamano_original / TAMANO_BLOQUE +
        (tamano_original % TAMANO_BLOQUE != 0 ? 1U : 0U);
    if (cantidad_bloques64 != bloques_esperados ||
        cantidad_bloques64 > SIZE_MAX) {
        establecer_error(error, capacidad_error,
                         "La cantidad de bloques del archivo comprimido es invalida.");
        goto fin;
    }
    (void)pthread_mutex_lock(&argumentos->operacion->mutex);
    argumentos->operacion->total = tamano_original == 0 ? 1 : tamano_original;
    (void)pthread_mutex_unlock(&argumentos->operacion->mutex);

    uint64_t suma_frecuencias = 0;
    uint64_t posicion_frecuencias = TAMANO_CABECERA_FIJA;
    for (unsigned simbolo = 0; simbolo < 256; simbolo++) {
        unsigned char campo[8];
        if (leer_exactamente_en(entrada_fd, campo, sizeof(campo),
                                posicion_frecuencias) == -1) {
            establecer_error_errno(error, capacidad_error,
                                   "No se pudieron leer las frecuencias Huffman");
            goto fin;
        }
        frecuencias[simbolo] = cargar_u64(campo);
        if (UINT64_MAX - suma_frecuencias < frecuencias[simbolo]) {
            establecer_error(error, capacidad_error,
                             "Las frecuencias del archivo exceden el limite.");
            goto fin;
        }
        suma_frecuencias += frecuencias[simbolo];
        posicion_frecuencias += sizeof(campo);
    }
    if (suma_frecuencias != tamano_original) {
        establecer_error(error, capacidad_error,
                         "Las frecuencias no coinciden con el tamano original.");
        goto fin;
    }
    if (construir_codigos(frecuencias, nodos, &raiz, codigos,
                          error, capacidad_error) == -1) {
        goto fin;
    }
    if ((tamano_original == 0 && raiz != -1) ||
        (tamano_original > 0 && raiz == -1)) {
        establecer_error(error, capacidad_error,
                         "El arbol Huffman no coincide con el tamano original.");
        goto fin;
    }

    cantidad_bloques = (size_t)cantidad_bloques64;
    if (cantidad_bloques > 0) {
        if (cantidad_bloques > SIZE_MAX / sizeof(*descriptores)) {
            establecer_error(error, capacidad_error,
                             "El indice de bloques supera la memoria disponible.");
            goto fin;
        }
        descriptores = calloc(cantidad_bloques, sizeof(*descriptores));
        if (descriptores == NULL) {
            establecer_error(error, capacidad_error,
                             "No hay memoria suficiente para leer los bloques.");
            goto fin;
        }
    }

    uint64_t posicion = TAMANO_CABECERA_FIJA + 256U * 8U;
    for (size_t indice = 0; indice < cantidad_bloques; indice++) {
        unsigned char datos_cabecera[TAMANO_CABECERA_BLOQUE];
        if (posicion > (uint64_t)inicial.st_size ||
            (uint64_t)inicial.st_size - posicion < sizeof(datos_cabecera)) {
            establecer_error(error, capacidad_error,
                             "El indice de bloques esta incompleto.");
            goto fin;
        }
        if (leer_exactamente_en(entrada_fd, datos_cabecera,
                                sizeof(datos_cabecera), posicion) == -1) {
            establecer_error_errno(error, capacidad_error,
                                   "No se pudo leer el indice de bloques");
            goto fin;
        }

        BloqueArchivo *descriptor = &descriptores[indice];
        descriptor->longitud_original = cargar_u32(datos_cabecera);
        descriptor->bits = cargar_u64(datos_cabecera + 4);
        descriptor->crc = cargar_u32(datos_cabecera + 12);
        uint64_t restante = tamano_original -
                            (uint64_t)indice * TAMANO_BLOQUE;
        uint32_t longitud_esperada = (uint32_t)(restante < TAMANO_BLOQUE
                                                    ? restante : TAMANO_BLOQUE);
        if (descriptor->longitud_original != longitud_esperada ||
            descriptor->bits < descriptor->longitud_original ||
            descriptor->bits > (uint64_t)descriptor->longitud_original * 255U ||
            (nodos[raiz].simbolo >= 0 &&
             descriptor->bits != descriptor->longitud_original)) {
            establecer_error(error, capacidad_error,
                             "Un bloque comprimido tiene longitudes invalidas.");
            goto fin;
        }

        uint64_t bytes_codificados = descriptor->bits / 8U +
            (descriptor->bits % 8U != 0 ? 1U : 0U);
        descriptor->desplazamiento = posicion + sizeof(datos_cabecera);
        if (descriptor->desplazamiento > (uint64_t)inicial.st_size ||
            bytes_codificados > (uint64_t)inicial.st_size -
                                    descriptor->desplazamiento) {
            establecer_error(error, capacidad_error,
                             "Un bloque comprimido esta truncado.");
            goto fin;
        }
        posicion = descriptor->desplazamiento + bytes_codificados;
    }
    if (posicion != (uint64_t)inicial.st_size) {
        establecer_error(error, capacidad_error,
                         "El archivo comprimido contiene datos sobrantes.");
        goto fin;
    }
    if (verificar_archivo_estable(entrada_fd, argumentos->origen, &inicial,
                                  error, capacidad_error) == -1) {
        goto fin;
    }

    salida_fd = crear_temporal_destino(argumentos->destino, &ruta_temporal);
    if (salida_fd == -1) {
        establecer_error_errno(error, capacidad_error,
                               "No se pudo crear el archivo descomprimido temporal");
        goto fin;
    }

    if (iniciar_grupo(&grupo, hilos, error, capacidad_error) == -1) goto fin;
    grupo_activo = 1;
    grupo.archivo_fd = entrada_fd;
    grupo.arbol = nodos;
    grupo.raiz = raiz;
    grupo.descriptores = descriptores;
    grupo.operacion = argumentos->operacion;

    for (size_t inicio = 0; inicio < cantidad_bloques;) {
        size_t indices[MAX_HILOS_TRABAJO];
        unsigned cantidad = (unsigned)(cantidad_bloques - inicio);
        if (cantidad > hilos) cantidad = hilos;
        for (unsigned i = 0; i < cantidad; i++) indices[i] = inicio + i;

        if (despachar_lote(&grupo, TAREA_DECODIFICAR, indices, cantidad,
                           error, capacidad_error) == -1) {
            for (unsigned i = 0; i < cantidad; i++) {
                free(grupo.decodificados[i]);
                grupo.decodificados[i] = NULL;
            }
            goto fin;
        }

        for (unsigned i = 0; i < cantidad; i++) {
            BloqueArchivo *descriptor = &descriptores[inicio + i];
            unsigned char *datos = grupo.decodificados[i];
            for (uint32_t byte = 0; byte < descriptor->longitud_original; byte++) {
                if (frecuencias_observadas[datos[byte]] == UINT64_MAX) {
                    establecer_error(error, capacidad_error,
                                     "El conteo descomprimido excedio el limite.");
                    for (unsigned j = i; j < cantidad; j++) {
                        free(grupo.decodificados[j]);
                        grupo.decodificados[j] = NULL;
                    }
                    goto fin;
                }
                frecuencias_observadas[datos[byte]]++;
            }
            if (escribir_todo(salida_fd, datos,
                              descriptor->longitud_original) == -1) {
                establecer_error_errno(error, capacidad_error,
                                       "No se pudo escribir un bloque descomprimido");
                for (unsigned j = i; j < cantidad; j++) {
                    free(grupo.decodificados[j]);
                    grupo.decodificados[j] = NULL;
                }
                goto fin;
            }
            actualizar_progreso(argumentos->operacion,
                                descriptor->longitud_original);
            free(datos);
            grupo.decodificados[i] = NULL;
        }
        inicio += cantidad;
    }

    for (unsigned simbolo = 0; simbolo < 256; simbolo++) {
        if (frecuencias_observadas[simbolo] != frecuencias[simbolo]) {
            establecer_error(error, capacidad_error,
                             "El contenido no coincide con las frecuencias Huffman.");
            goto fin;
        }
    }
    if (verificar_archivo_estable(entrada_fd, argumentos->origen, &inicial,
                                  error, capacidad_error) == -1) {
        goto fin;
    }
    if (publicar_temporal(&salida_fd, ruta_temporal, argumentos->destino,
                          error, capacidad_error) == -1) {
        goto fin;
    }
    resultado = 0;

fin:
    if (grupo_activo) detener_grupo(&grupo);
    if (salida_fd != -1) (void)close(salida_fd);
    if (ruta_temporal != NULL) {
        (void)unlink(ruta_temporal);
        free(ruta_temporal);
    }
    free(descriptores);
    if (entrada_fd != -1) (void)close(entrada_fd);
    return resultado;
}

static void *hilo_operacion(void *dato) {
    ArgumentosOperacion *argumentos = dato;
    CompresorOperacion *operacion = argumentos->operacion;
    char error[COMPRESOR_MENSAJE_MAX] = "";
    int resultado;

    if (argumentos->tipo == COMPRESOR_COMPRIMIR) {
        resultado = comprimir_archivo(argumentos, error, sizeof(error));
    } else {
        resultado = descomprimir_archivo(argumentos, error, sizeof(error));
    }

    (void)pthread_mutex_lock(&operacion->mutex);
    operacion->exito = resultado == 0;
    if (resultado == 0) {
        operacion->progreso = operacion->total;
        (void)snprintf(operacion->mensaje, sizeof(operacion->mensaje),
                       "%s completada.",
                       operacion->tipo == COMPRESOR_COMPRIMIR
                           ? "Compresion" : "Descompresion");
    } else {
        (void)snprintf(operacion->mensaje, sizeof(operacion->mensaje),
                       "%s", error[0] == '\0'
                           ? "La operacion no se pudo completar." : error);
    }
    operacion->terminado = 1;
    (void)pthread_cond_broadcast(&operacion->cambio);
    (void)pthread_mutex_unlock(&operacion->mutex);

    free(argumentos->origen);
    free(argumentos->destino);
    free(argumentos);
    return NULL;
}

int compresor_inicializar(CompresorOperacion *operacion) {
    memset(operacion, 0, sizeof(*operacion));
    int codigo = pthread_mutex_init(&operacion->mutex, NULL);
    if (codigo != 0) {
        errno = codigo;
        return -1;
    }
    codigo = pthread_cond_init(&operacion->cambio, NULL);
    if (codigo != 0) {
        (void)pthread_mutex_destroy(&operacion->mutex);
        errno = codigo;
        return -1;
    }
    operacion->sincronizado = 1;
    return 0;
}

int compresor_iniciar(CompresorOperacion *operacion, CompresorTipo tipo,
                      const char *origen, const char *destino) {
    if (operacion == NULL || !operacion->sincronizado || origen == NULL ||
        destino == NULL ||
        (tipo != COMPRESOR_COMPRIMIR && tipo != COMPRESOR_DESCOMPRIMIR)) {
        errno = EINVAL;
        return -1;
    }
    if (strlen(destino) >= sizeof(operacion->salida)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    ArgumentosOperacion *argumentos = calloc(1, sizeof(*argumentos));
    if (argumentos == NULL) return -1;
    argumentos->origen = strdup(origen);
    argumentos->destino = strdup(destino);
    if (argumentos->origen == NULL || argumentos->destino == NULL) {
        free(argumentos->origen);
        free(argumentos->destino);
        free(argumentos);
        return -1;
    }
    argumentos->operacion = operacion;
    argumentos->tipo = tipo;
    argumentos->hilos_trabajo = seleccionar_hilos();

    (void)pthread_mutex_lock(&operacion->mutex);
    if (operacion->activo || operacion->hilo_creado) {
        (void)pthread_mutex_unlock(&operacion->mutex);
        free(argumentos->origen);
        free(argumentos->destino);
        free(argumentos);
        errno = EBUSY;
        return -1;
    }

    operacion->activo = 1;
    operacion->terminado = 0;
    operacion->exito = 0;
    operacion->tipo = tipo;
    operacion->hilos_trabajo = argumentos->hilos_trabajo;
    operacion->progreso = 0;
    operacion->total = 0;
    (void)snprintf(operacion->salida, sizeof(operacion->salida), "%s", destino);
    operacion->mensaje[0] = '\0';
    int codigo = pthread_create(&operacion->hilo, NULL, hilo_operacion, argumentos);
    if (codigo != 0) {
        operacion->activo = 0;
        operacion->terminado = 0;
        (void)pthread_mutex_unlock(&operacion->mutex);
        free(argumentos->origen);
        free(argumentos->destino);
        free(argumentos);
        errno = codigo;
        return -1;
    }
    operacion->hilo_creado = 1;
    (void)pthread_mutex_unlock(&operacion->mutex);
    return 0;
}

int compresor_obtener_estado(CompresorOperacion *operacion,
                             CompresorEstado *estado) {
    if (operacion == NULL || estado == NULL || !operacion->sincronizado) {
        errno = EINVAL;
        return -1;
    }

    (void)pthread_mutex_lock(&operacion->mutex);
    estado->activo = operacion->activo;
    estado->terminado = operacion->terminado;
    estado->exito = operacion->exito;
    estado->tipo = operacion->tipo;
    estado->hilos_trabajo = operacion->hilos_trabajo;
    estado->progreso = operacion->progreso;
    estado->total = operacion->total;
    (void)snprintf(estado->salida, sizeof(estado->salida), "%s",
                   operacion->salida);
    (void)snprintf(estado->mensaje, sizeof(estado->mensaje), "%s",
                   operacion->mensaje);
    (void)pthread_mutex_unlock(&operacion->mutex);
    return 0;
}

static void copiar_estado_bloqueado(const CompresorOperacion *operacion,
                                    CompresorEstado *estado) {
    estado->activo = operacion->activo;
    estado->terminado = operacion->terminado;
    estado->exito = operacion->exito;
    estado->tipo = operacion->tipo;
    estado->hilos_trabajo = operacion->hilos_trabajo;
    estado->progreso = operacion->progreso;
    estado->total = operacion->total;
    (void)snprintf(estado->salida, sizeof(estado->salida), "%s",
                   operacion->salida);
    (void)snprintf(estado->mensaje, sizeof(estado->mensaje), "%s",
                   operacion->mensaje);
}

int compresor_recolectar(CompresorOperacion *operacion,
                         CompresorEstado *resultado) {
    pthread_t hilo;

    if (operacion == NULL || resultado == NULL || !operacion->sincronizado) {
        errno = EINVAL;
        return -1;
    }
    (void)pthread_mutex_lock(&operacion->mutex);
    if (!operacion->hilo_creado || !operacion->terminado) {
        (void)pthread_mutex_unlock(&operacion->mutex);
        return 0;
    }
    hilo = operacion->hilo;
    (void)pthread_mutex_unlock(&operacion->mutex);

    int codigo = pthread_join(hilo, NULL);
    if (codigo != 0) {
        errno = codigo;
        return -1;
    }

    (void)pthread_mutex_lock(&operacion->mutex);
    copiar_estado_bloqueado(operacion, resultado);
    operacion->hilo_creado = 0;
    operacion->activo = 0;
    operacion->terminado = 0;
    (void)pthread_mutex_unlock(&operacion->mutex);
    return 1;
}

int compresor_esperar(CompresorOperacion *operacion,
                      CompresorEstado *resultado) {
    if (operacion == NULL || resultado == NULL || !operacion->sincronizado) {
        errno = EINVAL;
        return -1;
    }

    (void)pthread_mutex_lock(&operacion->mutex);
    while (operacion->activo && !operacion->terminado) {
        (void)pthread_cond_wait(&operacion->cambio, &operacion->mutex);
    }
    (void)pthread_mutex_unlock(&operacion->mutex);
    return compresor_recolectar(operacion, resultado);
}

void compresor_destruir(CompresorOperacion *operacion) {
    if (operacion == NULL || !operacion->sincronizado) return;

    CompresorEstado resultado;
    (void)compresor_esperar(operacion, &resultado);
    (void)pthread_cond_destroy(&operacion->cambio);
    (void)pthread_mutex_destroy(&operacion->mutex);
    operacion->sincronizado = 0;
}



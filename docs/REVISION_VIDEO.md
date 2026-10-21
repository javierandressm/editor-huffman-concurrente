# Revisión de la sustentación v3

Fecha: 21 de octubre de 2026. Archivo revisado: huffman_edit_v3.mp4.

La versión v3 incorpora el complemento que faltaba en v2 y es adecuada para acompañar el proyecto como sustentación. Esto no garantiza una nota ni certifica la ausencia absoluta de defectos del programa.

## Capítulos aproximados

| Inicio | Contenido |
|---|---|
| 00:05 | Entorno Linux, compilación y pruebas |
| 00:12 | Editor del Parcial 1 |
| 01:15 | Compresión y descompresión |
| 02:24 | Verificación de integridad |
| 03:13 | Archivo grande, progreso, consulta y modificación rechazada |
| 04:44 | Estructura del proyecto |
| 05:25 | Creación y ciclo de vida de hilos |
| 08:55 | Biblioteca, pthread y operaciones de archivos |
| 10:32 | Bloques, frecuencias, árbol y salida ordenada |
| 10:54 | Mutex, reparto de tareas y condiciones |
| 11:34 | Archivos, limpieza y conclusión |
| 12:00 | Tarjeta final |

## Comprobaciones de contenido

- Se añadieron fragmentos reales de procesar_tarea, comprimir_archivo, hilo_trabajador, despachar_lote y detener_grupo.
- La explicación y las imágenes muestran el procesamiento por bloques y los resultados locales, el árbol global y el orden de escritura.
- Se muestran la reserva de tareas con mutex, el cálculo fuera de la sección crítica, las esperas dentro de while y las notificaciones.
- Se retiró la frase anterior que atribuía a unlink el cierre de descriptores. La nueva sección y las notas en pantalla distinguen unlink de close.
- La tarjeta que anunciaba una continuación ausente se reemplazó por un cierre completo.

## Observaciones menores y límites

Cerca de 11:36 hay un tropiezo verbal antes de la distinción correcta entre unlink y close; el texto en pantalla ayuda a aclararlo. No exige repetir la demostración.

La limpieza del grupo se realiza al terminar cada operación, no solamente al cerrar el programa; una nota en pantalla lo precisa.

La narración no desarrolla todos los detalles del formato ni poll. La documentación técnica los explica. El proyecto no afirma manejo completo de señales externas como SIGTERM; sigue siendo una limitación que debe reconocerse.

La revisión se apoya en fotogramas del MP4, transcripción de la narración, contraste del montaje y las funciones reales, y comprobación del cierre. Las pruebas mostradas son evidencia de ejecución en la máquina grabada, no pruebas nuevas ejecutadas durante esta revisión.
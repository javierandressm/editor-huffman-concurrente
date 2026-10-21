# Correspondencia con la Alternativa 1

| Requisito | Implementación | Evidencia |
|---|---|---|
| C en Linux con pthread | Makefile y src | Compilación y pruebas del video |
| Grupo de hilos y bloques | iniciar_grupo, hilo_trabajador, TAMANO_BLOQUE | Secciones 07, 09 y 10 |
| Conteo concurrente | TAREA_FRECUENCIAS, histogramas y reducción | Sección 09 |
| Árbol global y codificación paralela | construir_codigos, TAREA_CODIFICAR | Sección 09 |
| Orden de salida | Escritura por índice tras cada lote | Sección 09 y código |
| Mutex y condiciones | despachar_lote e hilo_trabajador | Sección 10 |
| Segundo plano y progreso | compresor_iniciar y ciclo de entrada con poll | Prueba grande |
| Protección frente a cambios | Rechazo de edición, instantánea, flock y metadatos | Prueba grande y manual |
| Integridad | CRC-32 y comparación de restaurados | cmp, SHA-256 y tests |
| Recursos | detener_grupo, pthread_join y limpieza | Sección 11 y código |
| Señales externas | No hay manejo completo verificado de SIGTERM | Limitación pendiente |

La tabla documenta el alcance y no asigna automáticamente los puntos de la rúbrica. El informe PDF conservado corresponde al Parcial 1; los cambios del Parcial 2 están descritos en los manuales Markdown y el README.
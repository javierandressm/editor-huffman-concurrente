# Compresor Huffman concurrente integrado al editor
Javier Andrés Sierra Machado
Sistemas Operativos · Universidad EAFIT · Parcial 2 · Alternativa 1
Informe técnico de la implementación · 26 de octubre de 2026

## 1 Alcance y organización
El proyecto extiende el editor de texto del Parcial 1 con compresión y descompresión Huffman en segundo plano. Está escrito en C para Linux y utiliza pthread, mutex y variables de condición de forma explícita. El usuario conserva las operaciones de edición y puede consultar el documento mientras se procesa una operación; las modificaciones se rechazan durante ese intervalo.

La implementación distribuye el conteo de frecuencias y la codificación entre tareas por bloques. El árbol de Huffman es global y se construye secuencialmente entre ambas fases. La descompresión también utiliza tareas por bloques. Un único hilo de operación escribe la salida en orden. Este documento explica el código y sus límites; no sustituye las evidencias de ejecución ni afirma una verificación formal de ausencia de carreras.

### Separación de responsabilidades
| Archivo | Responsabilidad |
| --- | --- |
| src/editor_main.c | Entrada de comandos, progreso e integración con el módulo compresor. |
| src/editor.c y editor.h | Estado del documento y operaciones de apertura, consulta, agregado, borrado y cierre. |
| src/lista.c y lista.h | Lista doblemente enlazada de líneas y su memoria dinámica. |
| src/archivo.c y archivo.h | Persistencia del documento mediante interfaces POSIX. |
| src/compresor.c y compresor.h | Formato Huffman, operaciones de fondo, tareas, sincronización y limpieza. |
| Makefile y tests/pruebas.sh | Compilación con -pthread y pruebas de integración. |

### Flujos de ejecución
El hilo principal ejecuta main y atiende la interfaz. compresor_iniciar crea un hilo cuya función de entrada es hilo_operacion. Ese hilo llama a comprimir_archivo o descomprimir_archivo y crea un grupo que ejecuta hilo_trabajador. Se consulta sysconf con _SC_NPROCESSORS_ONLN y se eligen entre uno y cuatro trabajadores. Con tres trabajadores existen cinco hilos durante el procesamiento: interfaz, operación y tres trabajadores.

Los términos coordinador y trabajador describen responsabilidades, no clases especiales de pthread. Cada hilo tiene su pila; los datos del proceso y los descriptores son compartidos. El grupo se crea por operación y se reutiliza para sucesivos lotes, sin crear un hilo por bloque.

## 2 Huffman y procesamiento por bloques
### División y lectura
TAMANO_BLOQUE vale 65 536 bytes, equivalentes a 64 KiB. El número de bloques se obtiene del tamaño del archivo y el último puede ser más pequeño. La tarea identifica el índice del bloque y su posición se calcula a partir de ese índice. pread permite leer en una posición explícita sin modificar el desplazamiento compartido del descriptor.

En compresión, el origen se copia previamente a una instantánea temporal. Las dos pasadas de Huffman leen esa copia. El coordinador publica lotes con un número de tareas no mayor que el de trabajadores. Los hilos reservan tareas disponibles; la asignación no permanece fija durante toda la operación.

### Conteo concurrente y reducción
TAREA_FRECUENCIAS hace que cada tarea cuente los 256 valores posibles de un byte en su propio histograma de enteros de 64 bits. No se comparte un contador global actualizado por cada byte. Tras completar cada lote, el hilo de operación suma los histogramas en el vector global. Se comprueban desbordamientos y que la suma corresponda al tamaño de la entrada.

### Árbol global y tabla de códigos
construir_codigos combina repetidamente los nodos de menor frecuencia para construir el árbol. Los empates se resuelven de forma determinista, de modo que las mismas frecuencias producen el mismo árbol al descomprimir. La tabla asigna un camino de bits a cada símbolo presente. El código de un símbolo no es prefijo del de otro.

La construcción del árbol es secuencial. Una entrada vacía no tiene bloques; si solo existe un símbolo se utiliza un código de un bit. Huffman no garantiza reducir todas las entradas: la distribución de frecuencias y la cabecera pueden producir una salida mayor que el original.

### Codificación y escritura ordenada
TAREA_CODIFICAR lee un bloque, consulta la tabla global de solo lectura y crea su buffer comprimido, longitud útil en bits y CRC-32. Cada tarea escribe en una ranura distinta. Una tarea puede acabar antes que otra sin alterar el orden lógico del archivo.

El hilo de operación espera el lote completo y escribe sus buffers según el índice original; después los libera. Solo ese hilo escribe el archivo de salida. La ventana de resultados queda limitada al tamaño del lote. Esto limita los buffers simultáneos de compresión, aunque no significa que toda la memoria del programa sea constante: el editor carga el documento y la descompresión mantiene metadatos por bloque.

### Descompresión
Se validan la firma, frecuencias, tamaños y límites de los bloques y se reconstruye el árbol global. Los trabajadores decodifican bloques independientes y verifican sus bits y CRC-32. El hilo de operación escribe en orden y comprueba las frecuencias reconstruidas antes de publicar la salida.

## 3 Protocolo de sincronización
### Estado del grupo
GrupoHilos contiene el mutex, las condiciones tareas_disponibles y lote_completo, los índices de bloques, siguiente_tarea, cantidad_tareas, pendientes, generacion, detener y el estado de error. Las estructuras de entrada y la tabla de códigos se preparan antes de publicar las tareas y permanecen estables hasta completar el lote.

despachar_lote toma el mutex, establece el tipo de tarea, copia los índices, reinicia siguiente_tarea, fija pendientes e incrementa generacion. Después despierta a los trabajadores con pthread_cond_broadcast y espera mientras pendientes sea mayor que cero.

### Ciclo de un trabajador
Cada hilo conserva la última generación observada. Espera dentro de un while mientras no haya una generación nueva ni una solicitud de detenerse. Si detener está activo, libera el mutex y retorna. De lo contrario, intenta reservar tareas del lote.

La reserva comprueba siguiente_tarea frente a cantidad_tareas y avanza el índice bajo el mutex. Así dos trabajadores no reservan la misma ranura. El mutex se libera antes de procesar_tarea para permitir cálculo simultáneo. Al finalizar, el trabajador vuelve a tomarlo, registra el primer error si existe y decrementa pendientes. El último señala lote_completo.

### Esperas y visibilidad de resultados
pthread_cond_wait libera atómicamente el mutex al comenzar la espera y vuelve a adquirirlo antes de retornar. El coordinador no conserva el bloqueo mientras duerme, por lo que los trabajadores pueden reducir pendientes. Las esperas comprueban la condición dentro de while: despertar no garantiza que el estado lógico esperado se cumpla.

Cada resultado ocupa una ranura exclusiva. La finalización se publica bajo el mutex; el hilo de operación solo consume los resultados después de comprobar pendientes igual a cero bajo ese mismo mutex. No libera ni reutiliza los buffers mientras una tarea del lote pueda seguir escribiéndolos.

### Dos ámbitos de exclusión
El mutex de GrupoHilos protege el protocolo de tareas. El mutex de CompresorOperacion protege el estado que consulta la interfaz, incluidos progreso, resultado y mensaje. El procesamiento del bloque ocurre fuera del mutex del grupo; las actualizaciones de progreso utilizan el de la operación. No se mantiene el mutex del grupo durante pthread_join.

### Justificación y límites de la verificación
La reducción local evita carreras sobre los contadores de frecuencias; el reparto protegido evita tareas duplicadas; la escritura única evita carreras sobre el offset de salida; las condiciones evitan espera activa en los trabajadores. El protocolo reduce los riesgos de interbloqueo al mantener cortas las secciones críticas y liberar el mutex en las esperas.

Estas propiedades se argumentan a partir del código. Las pruebas funcionales no equivalen a una demostración formal ni a un análisis exhaustivo con herramientas de detección de carreras.

## 4 Formato y protección de archivos
### Estructura del formato SOHUF001
Los enteros de la cabecera se serializan en little endian. La secuencia comprimida se organiza en una cabecera global y registros por bloque.

| Campo | Tamaño | Uso |
| --- | --- | --- |
| Firma SOHUF001 | 8 bytes | Identificar el formato. |
| Tamaño de bloque | 4 bytes | Validar el tamaño esperado de 65 536 bytes. |
| Tamaño original | 8 bytes | Recuperar y validar la cantidad original de bytes. |
| Número de bloques | 8 bytes | Validar la estructura del archivo. |
| Frecuencias globales | 256 por 8 bytes | Reconstruir el árbol determinista. |
| Cabecera por bloque | 16 bytes | Longitud original de 4 bytes, bits útiles de 8 y CRC-32 de 4. |
| Datos por bloque | Variable | Bits comprimidos y relleno del último byte. |

La cabecera global ocupa 2 076 bytes. La cantidad de bytes de un bloque comprimido se calcula a partir de sus bits útiles. La descompresión valida las longitudes, el relleno, el CRC-32, las frecuencias y que no existan bytes sobrantes. CRC-32 detecta corrupción accidental; no es una firma criptográfica de autenticidad.

### Consistencia del origen
La operación abre un archivo regular y solicita flock con LOCK_SH y LOCK_NB. Comprueba el descriptor con fstat y la ruta con stat. Para comprimir crea una copia temporal y revisa la estabilidad del origen después de copiar y antes de publicar el resultado. Se contrastan identidad, tamaño y tiempos de modificación y cambio; en Linux se incluyen las fracciones de segundo.

El editor rechaza las órdenes de modificación y de cambio de documento durante una operación. flock es cooperativo: una aplicación que lo ignore puede escribir. La instantánea estabiliza los datos procesados y las comprobaciones detectan cambios externos identificables; no debe describirse como una prohibición absoluta de toda escritura ajena ni como una transacción entre procesos no cooperativos.

### Publicación sin reemplazo
El destino se comprueba y la salida se genera en un temporal situado junto a él. Tras finalizar se utiliza fsync, se cierra el descriptor y link crea el nombre final. Si el destino ya existe, link falla en lugar de sobrescribirlo. unlink elimina el nombre temporal. Ubicarlo junto al destino permite que ambos nombres estén en el mismo sistema de archivos.

unlink elimina un nombre; close cierra un descriptor. El archivo puede seguir accesible a través de un descriptor abierto aunque su nombre haya sido eliminado. Si falla una etapa anterior a la publicación, se limpia el temporal correspondiente.

## 5 Funciones e interfaces utilizadas
### Entrada y ciclo de vida
compresor_inicializar prepara el estado y sus mecanismos de sincronización. compresor_iniciar valida parámetros, reserva ArgumentosOperacion con calloc, duplica las rutas con strdup y crea el hilo mediante pthread_create. Devuelve cero si se inició; no indica que la compresión ya haya terminado. Si falla, restaura el estado y libera sus reservas.

hilo_operacion selecciona la función de compresión o descompresión. Al regresar publica el éxito o error y terminado bajo el mutex, notifica cambio y libera sus argumentos. Terminar no equivale a recoger el hilo: compresor_recolectar o compresor_esperar efectúan el join y completan el ciclo de vida antes de reutilizar el estado.

iniciar_grupo inicializa el mutex y las dos condiciones y crea los trabajadores. Si una creación falla, solicita detener a los ya creados, los despierta y recoge y destruye lo inicializado. detener_grupo realiza la finalización normal del grupo.

### Servicios de archivos y biblioteca
| Interfaz | Función en el proyecto |
| --- | --- |
| open y close | Obtener y cerrar descriptores de archivo. |
| read y write | Transferir bytes y crear la instantánea y las salidas. |
| pread | Leer en una posición sin modificar el offset compartido. |
| stat y fstat | Consultar metadatos por ruta y por descriptor. |
| flock | Solicitar el bloqueo cooperativo del origen. |
| mkstemp | Crear y abrir un temporal exclusivo. |
| fsync | Solicitar sincronización de la salida con almacenamiento. |
| link y unlink | Publicar el nombre final y eliminar nombres temporales. |
| poll | Esperar entrada y permitir actualizaciones de progreso. |
| sysconf | Consultar los procesadores disponibles. |

Las operaciones de archivos se solicitan a través de interfaces de biblioteca que acceden a servicios del sistema. No todas las funciones anteriores son llamadas directas al kernel: mkstemp y sysconf son interfaces de biblioteca. calloc, malloc, realloc, strdup y free gestionan memoria; memcpy, memset y snprintf gestionan datos o cadenas.

pthread_create y pthread_join pertenecen a la API POSIX de hilos; pthread_mutex y pthread_cond proporcionan sincronización. No es correcto afirmar que cada operación de mutex entra necesariamente al kernel. El código utiliza estas interfaces y no invoca directamente syscall.

## 6 Uso y verificación de integridad
### Compilación en Linux
Desde la raíz del proyecto se ejecutan make, make test y ./editor. El Makefile compila los módulos con C11, avisos del compilador, macros POSIX y -pthread. También se puede abrir directamente un documento con ./editor notas.txt.

### Comandos del editor
| Orden | Comportamiento |
| --- | --- |
| o archivo.txt | Abrir o crear el documento. |
| a texto y d n | Agregar una línea o borrar la línea n. |
| p y p n | Consultar todas las líneas o una línea concreta. |
| e | Mostrar la estructura de la lista. |
| c salida.huf | Iniciar compresión del documento abierto. |
| x entrada.huf salida.txt | Iniciar descompresión a un destino nuevo. |
| q | Esperar la operación activa y salir limpiamente. |

La interfaz permanece atendida por el hilo principal y el procesamiento pesado se ejecuta en otro hilo y su grupo. poll espera la entrada y, durante la operación, utiliza un tiempo de espera de 250 ms para poder presentar el progreso cuando no hay un comando parcialmente escrito. Solo el principal imprime mensajes; los demás actualizan el estado bajo el mutex. La carga inicial de un documento mediante apertura sigue siendo síncrona.

Debe esperarse el mensaje de finalización de c antes de iniciar x. Las salidas deben ser nuevas. Para evitar ambigüedades del analizador de x se recomienda utilizar rutas sin espacios.

### Prueba reproducible
Dentro del editor se abre original.txt, se agregan líneas y se ejecuta c original.huf. Una vez finalizada la compresión, se ejecuta x original.huf restaurado.txt y se espera su finalización. Después de q, en la terminal de Linux se ejecutan cmp original.txt restaurado.txt y sha256sum original.txt restaurado.txt. cmp compara byte por byte; un retorno cero indica igualdad. Los hashes coincidentes son una comprobación adicional.

### Evidencias disponibles
tests/pruebas.sh comprueba operaciones del editor, un archivo aleatorio de 1 MiB, entradas vacías y de un solo símbolo, conservación de un destino existente y rechazo de datos corruptos sin publicar una salida parcial. También revisa la restricción de entrada y salida de alto nivel del proyecto.

La sustentación muestra ejecución en Linux, comparación de originales y restaurados y una entrada grande de 256 MiB, con consulta atendida y modificación rechazada durante la compresión. Son evidencias de la sesión grabada; este informe no afirma una nueva ejecución de pruebas ni mediciones de aceleración. El enlace de la versión final v4 está al final del documento.

## 7 Recursos límites y correspondencia con la rúbrica
### Limpieza y manejo de errores
Los errores de apertura, lectura, escritura, formato y creación de hilos se propagan hasta el estado que consulta la interfaz. Los bucles auxiliares contemplan transferencias parciales e interrupciones cuando corresponde. Las longitudes y sumas se validan antes de reservar o consumir datos.

El cierre solicita detener el grupo bajo su mutex y despierta los hilos. Después libera el mutex, hace pthread_join y destruye las condiciones y el mutex. Los buffers se liberan al consumirlos o en las rutas de error. Los descriptores se cierran y los temporales se limpian. Los argumentos copiados se liberan desde el hilo de operación. La interfaz espera la operación antes de destruir su estado compartido.

### Límites conocidos
El proyecto no implementa un manejo completo y verificado de señales externas como SIGTERM. La salida normal con q está contemplada, pero no debe afirmarse que toda terminación abrupta conserva las mismas garantías. No se ha certificado ausencia de fugas mediante un análisis exhaustivo de memoria ni ausencia de carreras mediante instrumentación especializada.

El editor guarda las líneas en memoria y la copia temporal requiere espacio en disco. La descompresión reserva metadatos por bloque. El grupo está limitado a cuatro hilos y la sincronización por lotes puede dejar trabajadores sin tarea mientras termina el bloque más lento. No se presentan cifras de mejora de rendimiento frente a una versión secuencial.

### Relación con los criterios del parcial
| Criterio | Puntos | Contenido relacionado |
| --- | --- | --- |
| Concurrencia en Huffman | 25 | Conteo y codificación por bloques, reducción y orden de salida. |
| Sincronización | 25 | Mutex, condiciones, reserva de tareas y fin de lote. |
| Integración con el editor | 20 | Operación de fondo, progreso y protección ante edición. |
| Corrección algorítmica | 15 | Restauración y comparaciones de integridad. |
| Errores y recursos | 15 | Limpieza, joins y validaciones; señales externas pendientes. |

La tabla relaciona implementación y criterios; no asigna una calificación automática. Los manuales Markdown amplían el uso y el desarrollo. El PDF 01_Informe_Tecnico.pdf conservado en docs corresponde al Parcial 1; este informe documenta la extensión del Parcial 2.

### Fuentes del informe y entrega
Código y documentación: https://github.com/javierandressm/editor-huffman-concurrente
Sustentación v4: https://drive.google.com/file/d/1_BKVOteCt5HR5_ZXE5QCgXYV_zsN4btQ/view
Consigna y rúbrica: https://docs.google.com/document/d/14G-E9jXDDqibstuc5X32BbQEvc6kQdTbmRSEkpbLmNg/edit

El informe se elaboró contrastando las funciones reales de compresor.c, compresor.h y editor_main.c, el Makefile, las pruebas y las evidencias de la sustentación. No certifica lo enviado a EAFIT Interactiva; ese registro pertenece a la plataforma de entrega.


/**
 * @file    bt_STM32.h
 * @author  Mamani Flores Carlos (UTN FRT)
 * @brief   Driver genérico para módulos Bluetooth Classic y BLE sobre STM32 HAL.
 * @details Soporta HC-05, HC-06 (Bluetooth Classic, SPP) y HM-10 / clones
 *          CC2541 (BLE 4.0, UART transparente). Características:
 *            - Recepción por interrupción (byte a byte) con búfer circular FIFO.
 *            - Lectura no bloqueante (@ref BT_ReadChar, @ref BT_Available).
 *            - Supervisión del enlace mediante el pin físico STATE, con
 *              antirrebote configurable (necesario en BLE, donde STATE parpadea
 *              mientras no hay conexión).
 *            - Recuperación automática de la recepción ante desconexión o
 *              errores de la UART.
 *            - Envío fragmentado en BLE (paquetes de ~20 bytes con pausa).
 *            - Comandos AT básicos para módulos BLE (nombre, MAC, notificaciones).
 *
 * @note    La transmisión (@ref BT_SendString, @ref BT_SendData) y las lecturas
 *          con timeout (@ref BT_ReadString, @ref BT_BLE_SendAT) son BLOQUEANTES.
 *          Solo la recepción y la lectura de caracteres son no bloqueantes.
 *
 * @version 2.0
 * @date    2026
 */

#ifndef BT_STM32_H
#define BT_STM32_H

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/*                                 INCLUDES                                   */
/* ========================================================================== */

#include "main.h"      /* Incluye la HAL de la familia STM32 utilizada */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ========================================================================== */
/*                      PARÁMETROS CONFIGURABLES (macros)                     */
/* ========================================================================== */

/**
 * @brief Tamaño del búfer circular de recepción, en bytes.
 * @warning Debe ser potencia de 2 (se verifica en compilación). Se puede
 *          redefinir antes de incluir este archivo.
 * @note    Se usan BT_RX_BUFFER_SIZE - 1 posiciones útiles (una queda libre
 *          para distinguir "lleno" de "vacío").
 */
#ifndef BT_RX_BUFFER_SIZE
#define BT_RX_BUFFER_SIZE 128
#endif

_Static_assert((BT_RX_BUFFER_SIZE & (BT_RX_BUFFER_SIZE - 1)) == 0,
               "BT_RX_BUFFER_SIZE debe ser potencia de 2");

/** @brief Tamaño de paquete BLE por defecto (MTU 23 - 3 bytes de cabecera ATT). */
#ifndef BT_BLE_DEFAULT_CHUNK
#define BT_BLE_DEFAULT_CHUNK        20u
#endif

/** @brief Pausa por defecto entre paquetes BLE consecutivos, en ms. */
#ifndef BT_BLE_DEFAULT_TX_DELAY_MS
#define BT_BLE_DEFAULT_TX_DELAY_MS  20u
#endif

/**
 * @brief Tiempo que STATE debe permanecer en alto para considerar "conectado"
 *        en módulos BLE, en ms.
 * @details El pin STATE del HM-10 alterna 500 ms alto / 500 ms bajo mientras
 *          no hay conexión; 700 ms de nivel alto continuo descarta ese parpadeo.
 */
#ifndef BT_BLE_DEFAULT_DEBOUNCE_MS
#define BT_BLE_DEFAULT_DEBOUNCE_MS  700u
#endif

/** @brief Timeout por defecto de los comandos AT, en ms. */
#ifndef BT_AT_TIMEOUT_MS
#define BT_AT_TIMEOUT_MS            500u
#endif

/**
 * @brief Tiempo de silencio en RX que marca el fin de una respuesta AT, en ms.
 * @details Las respuestas AT del HM-10 no tienen terminador (CR/LF), por lo que
 *          se detecta su fin por inactividad de la línea.
 */
#ifndef BT_AT_IDLE_GAP_MS
#define BT_AT_IDLE_GAP_MS           60u
#endif

/** @brief Longitud máxima del nombre BLE aceptada por el HM-10 (caracteres). */
#define BT_BLE_NAME_MAX_LEN         12u

/** @brief Tamaño mínimo del arreglo destino de @ref BT_BLE_GetAddress (12 hex + '\0'). */
#define BT_BLE_ADDR_BUF_SIZE        13u

/* ========================================================================== */
/*                                   TIPOS                                    */
/* ========================================================================== */

/**
 * @brief Códigos de estado y error devueltos por el driver.
 */
typedef enum {
    BT_OK = 0,         /**< Operación exitosa */
    BT_ERROR,          /**< Error general de ejecución o respuesta inesperada */
    BT_TIMEOUT,        /**< Tiempo de espera agotado */
    BT_BUSY,           /**< Periférico o módulo ocupado / operación no permitida en este estado */
    BT_INVALID_PARAM,  /**< Parámetro de entrada inválido o nulo */
    BT_BUFFER_EMPTY,   /**< Búfer de recepción vacío */
    BT_DISCONNECTED    /**< Módulo sin enlace con el dispositivo remoto */
} BT_Status_t;

/**
 * @brief Tipos de módulos Bluetooth soportados.
 */
typedef enum {
    BT_TYPE_HC05 = 0,    /**< Bluetooth Classic HC-05 (Maestro/Esclavo) */
    BT_TYPE_HC06,        /**< Bluetooth Classic HC-06 (solo Esclavo) */
    BT_TYPE_HM10,        /**< BLE 4.0 HM-10 (CC2541) */
    BT_TYPE_BLE_GENERIC  /**< BLE genérico con UART transparente y AT (AT-09, JDY-08, BT05...) */
} BT_ModuleType_t;

/**
 * @brief Búfer circular FIFO de recepción (un productor: ISR; un consumidor: aplicación).
 * @details `head` solo lo modifica la ISR y `tail` solo la aplicación, por lo que
 *          no se requiere sección crítica para uso normal.
 */
typedef struct {
    uint8_t buffer[BT_RX_BUFFER_SIZE]; /**< Almacenamiento de los datos */
    volatile uint16_t head;            /**< Índice de escritura (lo avanza la ISR) */
    volatile uint16_t tail;            /**< Índice de lectura (lo avanza la aplicación) */
} BT_RingBuffer_t;

/**
 * @brief Manejador (instancia) del driver. Se declara una por módulo físico.
 * @note  Los campos son internos; usar siempre las funciones de la API.
 */
typedef struct {
    UART_HandleTypeDef *huart;     /**< UART de la HAL conectada al módulo */
    BT_ModuleType_t module_type;   /**< Tipo de módulo (determina el comportamiento BLE/Classic) */
    uint32_t timeout_ms;           /**< Timeout por defecto de transmisión, en ms */

    GPIO_TypeDef *state_port;      /**< Puerto GPIO del pin STATE (NULL si no se usa) */
    uint16_t state_pin;            /**< Pin GPIO del pin STATE */
    bool was_connected;            /**< Estado de enlace previo (detección de flancos) */
    uint16_t state_debounce_ms;    /**< Tiempo mínimo de STATE en alto para considerar enlace activo */
    bool high_pending;             /**< true si STATE está en alto y se está cronometrando */
    uint32_t high_since_ms;        /**< Marca de tiempo (HAL_GetTick) de inicio del nivel alto */

    uint16_t ble_chunk_size;       /**< Bytes máximos por paquete en TX BLE */
    uint16_t ble_tx_delay_ms;      /**< Pausa entre paquetes BLE, en ms */

    BT_RingBuffer_t rx_buffer;     /**< Búfer circular de recepción */
    uint8_t rx_temp_byte;          /**< Byte de recepción en curso (destino de HAL_UART_Receive_IT) */
    volatile uint32_t rx_overflow; /**< Cantidad de bytes descartados por búfer lleno */
} BT_Handler_t;

/* ========================================================================== */
/*                         API: INICIALIZACIÓN Y CONFIGURACIÓN                */
/* ========================================================================== */

/**
 * @brief Inicializa el driver y arranca la recepción por interrupción.
 * @details Aborta cualquier recepción previa de la UART (permite reinicializar),
 *          limpia el búfer y aplica valores por defecto según el tipo de módulo
 *          (paquetes de 20 bytes y antirrebote de STATE para BLE).
 * @param[out] bt      Manejador a inicializar.
 * @param[in]  huart   UART de la HAL ya configurada (baudrate del módulo, 8N1).
 * @param[in]  type    Tipo de módulo.
 * @param[in]  timeout Timeout de transmisión por defecto, en ms.
 * @retval BT_OK            Inicialización correcta.
 * @retval BT_INVALID_PARAM `bt` o `huart` nulos.
 * @retval BT_ERROR         No se pudo habilitar la recepción por interrupción.
 */
BT_Status_t BT_Init(BT_Handler_t *bt, UART_HandleTypeDef *huart,
                    BT_ModuleType_t type, uint32_t timeout);

/**
 * @brief Configura el pin físico STATE del módulo para supervisar el enlace.
 * @details Sin pin STATE el driver asume que el enlace siempre está activo.
 *          Reinicia el antirrebote y sincroniza el estado previo de conexión.
 * @param[in,out] bt   Manejador.
 * @param[in]     port Puerto GPIO (ej.: GPIOA). Debe estar configurado como entrada.
 * @param[in]     pin  Pin GPIO (ej.: GPIO_PIN_12).
 * @retval BT_OK            Configurado correctamente.
 * @retval BT_INVALID_PARAM Punteros nulos.
 */
BT_Status_t BT_ConfigStatePin(BT_Handler_t *bt, GPIO_TypeDef *port, uint16_t pin);

/**
 * @brief Ajusta el antirrebote del pin STATE.
 * @details La conexión se considera establecida solo si STATE permanece en alto
 *          `debounce_ms` de forma continua. La desconexión se detecta de inmediato.
 *          Con 0 se desactiva el antirrebote (recomendado en HC-05/HC-06).
 * @param[in,out] bt          Manejador.
 * @param[in]     debounce_ms Tiempo en ms (0 = sin antirrebote).
 * @retval BT_OK            Aplicado.
 * @retval BT_INVALID_PARAM `bt` nulo.
 */
BT_Status_t BT_SetStateDebounce(BT_Handler_t *bt, uint16_t debounce_ms);

/**
 * @brief Ajusta la fragmentación de la transmisión en módulos BLE.
 * @details Los envíos se parten en paquetes de `chunk_size` bytes separados por
 *          `delay_ms`. Sin efecto práctico en módulos Classic (se envía de una vez).
 * @param[in,out] bt         Manejador.
 * @param[in]     chunk_size Bytes por paquete (1..65535; típico 20).
 * @param[in]     delay_ms   Pausa entre paquetes en ms (0 = sin pausa).
 * @retval BT_OK            Aplicado.
 * @retval BT_INVALID_PARAM `bt` nulo o `chunk_size` igual a 0.
 */
BT_Status_t BT_SetBLEPacketing(BT_Handler_t *bt, uint16_t chunk_size, uint16_t delay_ms);

/**
 * @brief Indica si el módulo configurado es de tipo BLE.
 * @param[in] bt Manejador.
 * @retval true  HM-10 o BLE genérico.
 * @retval false Classic (HC-05/HC-06) o `bt` nulo.
 */
bool BT_IsBLE(const BT_Handler_t *bt);

/* ========================================================================== */
/*                            API: TRANSMISIÓN                                */
/* ========================================================================== */

/**
 * @brief Envía una cadena terminada en nulo (BLOQUEANTE).
 * @details En BLE la cadena se fragmenta automáticamente. Una cadena vacía
 *          devuelve BT_OK sin transmitir nada.
 * @param[in] bt  Manejador.
 * @param[in] str Cadena a enviar.
 * @retval BT_OK            Enviada completa.
 * @retval BT_INVALID_PARAM Punteros nulos.
 * @retval BT_DISCONNECTED  Sin enlace (antes o durante el envío).
 * @retval BT_TIMEOUT       La UART no completó la transmisión a tiempo.
 * @retval BT_BUSY          UART ocupada.
 * @retval BT_ERROR         Otro fallo de la HAL.
 */
BT_Status_t BT_SendString(BT_Handler_t *bt, const char *str);

/**
 * @brief Envía un bloque de datos binarios (BLOQUEANTE).
 * @details En BLE el bloque se fragmenta automáticamente.
 * @param[in] bt   Manejador.
 * @param[in] data Datos a enviar.
 * @param[in] size Cantidad de bytes (> 0).
 * @retval BT_OK            Enviado completo.
 * @retval BT_INVALID_PARAM Punteros nulos o `size` igual a 0.
 * @retval BT_DISCONNECTED  Sin enlace (antes o durante el envío).
 * @retval BT_TIMEOUT       Timeout de la UART.
 * @retval BT_BUSY          UART ocupada.
 * @retval BT_ERROR         Otro fallo de la HAL.
 */
BT_Status_t BT_SendData(BT_Handler_t *bt, const uint8_t *data, uint16_t size);

/* ========================================================================== */
/*                             API: RECEPCIÓN                                 */
/* ========================================================================== */

/**
 * @brief Lee un byte del búfer circular sin bloquear.
 * @param[in,out] bt Manejador.
 * @return Byte leído (0..255) o -1 si el búfer está vacío o `bt` es nulo.
 */
int16_t BT_ReadChar(BT_Handler_t *bt);

/**
 * @brief Cantidad de bytes pendientes de lectura en el búfer.
 * @param[in] bt Manejador.
 * @return Bytes disponibles (0 si `bt` es nulo).
 */
uint16_t BT_Available(const BT_Handler_t *bt);

/**
 * @brief Lee una línea (hasta '\\n') o hasta agotar espacio/tiempo (BLOQUEANTE).
 * @details Siempre deja `dest` terminado en '\\0'. El '\\n' se incluye en el resultado.
 * @param[in,out] bt         Manejador.
 * @param[out]    dest       Arreglo destino.
 * @param[in]     max_len    Tamaño de `dest` (mínimo 2: 1 carácter + '\\0').
 * @param[in]     timeout_ms Tiempo máximo de espera en ms.
 * @retval BT_OK            Se leyó al menos un carácter.
 * @retval BT_TIMEOUT       No llegó ningún dato a tiempo.
 * @retval BT_INVALID_PARAM Parámetros inválidos.
 */
BT_Status_t BT_ReadString(BT_Handler_t *bt, char *dest, uint16_t max_len, uint32_t timeout_ms);

/**
 * @brief Descarta todos los datos pendientes del búfer de recepción.
 * @note  Segura frente a la ISR (solo mueve `tail`).
 * @param[in,out] bt Manejador.
 */
void BT_Flush(BT_Handler_t *bt);

/**
 * @brief Cantidad acumulada de bytes descartados por búfer lleno.
 * @details Un valor creciente indica que la aplicación lee demasiado lento o que
 *          conviene aumentar @ref BT_RX_BUFFER_SIZE.
 * @param[in] bt Manejador.
 * @return Bytes perdidos desde @ref BT_Init (0 si `bt` es nulo).
 */
uint32_t BT_GetRxOverflowCount(const BT_Handler_t *bt);

/* ========================================================================== */
/*                         API: SUPERVISIÓN DEL ENLACE                        */
/* ========================================================================== */

/**
 * @brief Consulta el estado del enlace según el pin STATE (con antirrebote).
 * @param[in,out] bt Manejador (actualiza el cronómetro del antirrebote).
 * @retval true  Enlace activo, o no hay pin STATE configurado.
 * @retval false Enlace caído, aún no estabilizado, o `bt` nulo.
 */
bool BT_IsConnected(BT_Handler_t *bt);

/**
 * @brief Supervisa el enlace y mantiene viva la recepción.
 * @details Llamar periódicamente desde el lazo principal (idealmente cada
 *          <= 100 ms; el antirrebote depende de estas consultas). Realiza:
 *            - Caída de enlace: aborta la recepción, vacía el búfer y la rearma.
 *            - Reconexión: solo actualiza el estado (no descarta datos recibidos).
 *            - Si la recepción quedó detenida (error de UART), la rearma.
 * @param[in,out] bt Manejador.
 * @retval BT_OK            Enlace activo.
 * @retval BT_DISCONNECTED  Enlace caído (o recién perdido).
 * @retval BT_INVALID_PARAM `bt` nulo.
 * @retval BT_ERROR         No se pudo rearmar la recepción.
 */
BT_Status_t BT_CheckAndRecover(BT_Handler_t *bt);

/* ========================================================================== */
/*                         API: COMANDOS AT PARA BLE                          */
/* ========================================================================== */

/**
 * @brief Envía un comando AT y recoge la respuesta (BLOQUEANTE, solo BLE).
 * @details Los módulos HM-10 no usan terminador. La respuesta se da por
 *          finalizada tras @ref BT_AT_IDLE_GAP_MS de silencio en la línea.
 * @warning Solo funciona SIN conexión activa; con enlace, el módulo interpreta
 *          el texto como datos. Si hay pin STATE y está conectado, devuelve BT_BUSY.
 * @param[in,out] bt         Manejador.
 * @param[in]     cmd        Comando completo (ej.: "AT+NAMEmiDisp"; sin CR/LF).
 * @param[out]    resp       Destino de la respuesta (puede ser NULL si no interesa).
 * @param[in]     resp_size  Tamaño de `resp` (se trunca y termina en '\\0').
 * @param[in]     timeout_ms Espera máxima del primer byte de respuesta.
 * @retval BT_OK            Se recibió respuesta.
 * @retval BT_TIMEOUT       El módulo no respondió.
 * @retval BT_BUSY          Hay un enlace activo.
 * @retval BT_INVALID_PARAM Parámetros inválidos o módulo no BLE.
 * @retval BT_ERROR         Fallo de transmisión.
 */
BT_Status_t BT_BLE_SendAT(BT_Handler_t *bt, const char *cmd, char *resp,
                          uint16_t resp_size, uint32_t timeout_ms);

/**
 * @brief Cambia el nombre anunciado del módulo BLE (AT+NAME).
 * @param[in,out] bt   Manejador.
 * @param[in]     name Nombre (1..@ref BT_BLE_NAME_MAX_LEN caracteres).
 * @retval BT_OK            Nombre aplicado.
 * @retval BT_INVALID_PARAM Nombre nulo, vacío o demasiado largo; módulo no BLE.
 * @retval BT_ERROR         Respuesta inesperada.
 * @retval BT_TIMEOUT / BT_BUSY  Ver @ref BT_BLE_SendAT.
 */
BT_Status_t BT_BLE_SetName(BT_Handler_t *bt, const char *name);

/**
 * @brief Habilita o deshabilita los avisos "OK+CONN" / "OK+LOST" (AT+NOTI).
 * @note  Si se habilitan, esos textos llegan al búfer de recepción al conectar
 *        y desconectar.
 * @param[in,out] bt     Manejador.
 * @param[in]     enable true para habilitar.
 * @retval BT_OK / BT_ERROR / BT_TIMEOUT / BT_BUSY / BT_INVALID_PARAM
 */
BT_Status_t BT_BLE_EnableNotifications(BT_Handler_t *bt, bool enable);

/**
 * @brief Obtiene la dirección MAC del módulo BLE (AT+ADDR?).
 * @param[in,out] bt       Manejador.
 * @param[out]    mac      Destino: 12 caracteres hexadecimales + '\\0'.
 * @param[in]     mac_size Tamaño de `mac` (mínimo @ref BT_BLE_ADDR_BUF_SIZE).
 * @retval BT_OK            MAC obtenida.
 * @retval BT_INVALID_PARAM Parámetros inválidos o módulo no BLE.
 * @retval BT_ERROR         Respuesta con formato inesperado.
 * @retval BT_TIMEOUT / BT_BUSY  Ver @ref BT_BLE_SendAT.
 */
BT_Status_t BT_BLE_GetAddress(BT_Handler_t *bt, char *mac, uint16_t mac_size);

/**
 * @brief Reinicia el módulo BLE (AT+RESET).
 * @note  El módulo tarda unos cientos de ms en volver a estar operativo.
 * @param[in,out] bt Manejador.
 * @retval BT_OK / BT_ERROR / BT_TIMEOUT / BT_BUSY / BT_INVALID_PARAM
 */
BT_Status_t BT_BLE_Reset(BT_Handler_t *bt);

/* ========================================================================== */
/*                         CALLBACKS PARA LA HAL                              */
/* ========================================================================== */

/**
 * @brief Procesa un byte recibido: lo guarda en el búfer y rearma la recepción.
 * @note  Invocar desde `HAL_UART_RxCpltCallback` cuando `huart` coincida con la
 *        UART del manejador. Si el búfer está lleno, el byte se descarta y se
 *        incrementa el contador de overflow.
 * @param[in,out] bt Manejador.
 */
void BT_RxISR_Callback(BT_Handler_t *bt);

/**
 * @brief Recupera la recepción tras un error de la UART (overrun, framing, ruido).
 * @note  Invocar desde `HAL_UART_ErrorCallback` cuando `huart` coincida. Si no
 *        se usa, @ref BT_CheckAndRecover rearmará la recepción en su próxima llamada.
 * @param[in,out] bt Manejador.
 */
void BT_ErrorISR_Callback(BT_Handler_t *bt);

#ifdef __cplusplus
}
#endif

#endif /* BT_STM32_H */

/**
 * @file    bt_STM32.c
 * @author  Mamani Flores Carlos (UTN FRT)
 * @brief   Implementación del driver Bluetooth Classic / BLE para STM32 HAL.
 * @details Contiene el búfer circular FIFO, la recepción por interrupción, la
 *          supervisión del pin STATE con antirrebote, la transmisión fragmentada
 *          para BLE y los comandos AT básicos de módulos BLE.
 * @version 2.0
 * @date    2026
 */

#include "bt_STM32.h"
#include <string.h>

/* ========================================================================== */
/*                        DEFINICIONES Y FUNCIONES PRIVADAS                   */
/* ========================================================================== */

/** Máscara para índices del búfer circular (válida porque el tamaño es potencia de 2). */
#define BT_RX_MASK ((uint16_t)(BT_RX_BUFFER_SIZE - 1u))

/** Tamaño del búfer local para respuestas AT internas de las funciones auxiliares. */
#define BT_AT_LOCAL_RESP_SIZE 48u

/**
 * @brief Traduce un código de retorno de la HAL a un código del driver.
 * @param[in] s Código HAL.
 * @return Código BT equivalente (conserva TIMEOUT y BUSY).
 */
static BT_Status_t BT_FromHAL(HAL_StatusTypeDef s)
{
    switch (s) {
        case HAL_OK:      return BT_OK;
        case HAL_BUSY:    return BT_BUSY;
        case HAL_TIMEOUT: return BT_TIMEOUT;
        default:          return BT_ERROR;
    }
}

/**
 * @brief Rearma la recepción por interrupción si está detenida.
 * @details HAL_UART_Receive_IT solo acepta ser llamada con RxState en READY;
 *          si la recepción ya está en curso (BUSY_RX) no se hace nada.
 * @param[in,out] bt Manejador.
 * @retval BT_OK    Recepción activa (ya lo estaba o se rearmó).
 * @retval BT_ERROR No se pudo rearmar.
 */
static BT_Status_t BT_RearmRx(BT_Handler_t *bt)
{
    if (bt->huart->RxState == HAL_UART_STATE_READY) {
        if (HAL_UART_Receive_IT(bt->huart, &bt->rx_temp_byte, 1) != HAL_OK) {
            return BT_ERROR;
        }
    }
    return BT_OK;
}

/**
 * @brief Transmite un bloque, fragmentándolo si el módulo es BLE.
 * @details Classic: HAL_UART_Transmit en trozos de hasta 65535 bytes (límite del
 *          parámetro de la HAL). BLE: paquetes de `ble_chunk_size` bytes con
 *          pausa `ble_tx_delay_ms` entre ellos, verificando que el enlace siga
 *          activo entre paquetes.
 * @param[in,out] bt   Manejador.
 * @param[in]     data Datos a enviar.
 * @param[in]     len  Cantidad de bytes.
 * @retval BT_OK           Transmisión completa.
 * @retval BT_DISCONNECTED Se perdió el enlace entre paquetes.
 * @retval BT_TIMEOUT / BT_BUSY / BT_ERROR Error propagado desde la HAL.
 */
static BT_Status_t BT_TxRaw(BT_Handler_t *bt, const uint8_t *data, size_t len)
{
    /* Tamaño de trozo: el del paquete BLE o el máximo que admite la HAL */
    size_t chunk = BT_IsBLE(bt) ? bt->ble_chunk_size : (size_t)UINT16_MAX;
    size_t sent = 0;

    while (sent < len) {
        size_t n = len - sent;
        if (n > chunk) n = chunk;

        BT_Status_t st = BT_FromHAL(HAL_UART_Transmit(bt->huart,
                                    (uint8_t *)&data[sent], (uint16_t)n,
                                    bt->timeout_ms));
        if (st != BT_OK) return st;
        sent += n;

        /* Entre paquetes: pausa BLE y verificación de que no se cayó el enlace */
        if (sent < len) {
            if (BT_IsBLE(bt) && bt->ble_tx_delay_ms > 0u) {
                HAL_Delay(bt->ble_tx_delay_ms);
            }
            if (!BT_IsConnected(bt)) return BT_DISCONNECTED;
        }
    }
    return BT_OK;
}

/**
 * @brief Envía un comando AT y verifica que la respuesta contenga "OK".
 * @param[in,out] bt   Manejador.
 * @param[in]     cmd  Comando AT.
 * @param[out]    resp Si no es NULL, recibe una copia de la respuesta.
 * @param[in]     size Tamaño de `resp`.
 * @retval BT_OK    El módulo respondió con "OK...".
 * @retval BT_ERROR Respuesta distinta de OK.
 * @retval Otros    Errores de @ref BT_BLE_SendAT.
 */
static BT_Status_t BT_AT_Expect_OK(BT_Handler_t *bt, const char *cmd,
                                   char *resp, uint16_t size)
{
    char local[BT_AT_LOCAL_RESP_SIZE];
    char *buf = (resp != NULL) ? resp : local;
    uint16_t buf_size = (resp != NULL) ? size : (uint16_t)sizeof(local);

    BT_Status_t st = BT_BLE_SendAT(bt, cmd, buf, buf_size, BT_AT_TIMEOUT_MS);
    if (st != BT_OK) return st;

    return (strstr(buf, "OK") != NULL) ? BT_OK : BT_ERROR;
}

/* ========================================================================== */
/*                    INICIALIZACIÓN Y CONFIGURACIÓN                          */
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
                    BT_ModuleType_t type, uint32_t timeout)
{
    if (bt == NULL || huart == NULL) return BT_INVALID_PARAM;

    /* Parámetros básicos */
    bt->huart       = huart;
    bt->module_type = type;
    bt->timeout_ms  = timeout;

    /* Pin STATE: sin configurar. Sin pin se asume enlace activo. */
    bt->state_port       = NULL;
    bt->state_pin        = 0;
    bt->was_connected    = true;
    bt->high_pending     = false;
    bt->high_since_ms    = 0;

    /* Valores por defecto según tecnología */
    bt->ble_chunk_size   = BT_BLE_DEFAULT_CHUNK;
    bt->ble_tx_delay_ms  = BT_BLE_DEFAULT_TX_DELAY_MS;
    bt->state_debounce_ms = BT_IsBLE(bt) ? BT_BLE_DEFAULT_DEBOUNCE_MS : 0u;

    /* Si la UART ya tenía una recepción armada (reinicialización), se aborta antes
       de tocar los índices, para que la ISR no escriba mientras se limpian. */
    (void)HAL_UART_AbortReceive(bt->huart);
    bt->rx_buffer.head = 0;
    bt->rx_buffer.tail = 0;
    bt->rx_overflow    = 0;

    /* Arranca la recepción asíncrona byte a byte */
    if (HAL_UART_Receive_IT(bt->huart, &bt->rx_temp_byte, 1) != HAL_OK) {
        return BT_ERROR;
    }
    return BT_OK;
}

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
BT_Status_t BT_ConfigStatePin(BT_Handler_t *bt, GPIO_TypeDef *port, uint16_t pin)
{
    if (bt == NULL || port == NULL) return BT_INVALID_PARAM;

    bt->state_port    = port;
    bt->state_pin     = pin;
    bt->high_pending  = false;              /* reinicia el antirrebote */
    bt->was_connected = BT_IsConnected(bt); /* sincroniza el estado previo con el real */
    return BT_OK;
}

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
BT_Status_t BT_SetStateDebounce(BT_Handler_t *bt, uint16_t debounce_ms)
{
    if (bt == NULL) return BT_INVALID_PARAM;
    bt->state_debounce_ms = debounce_ms;
    bt->high_pending = false;
    return BT_OK;
}

/**
 * @brief Ajusta la fragmentación de la transmisión en módulos BLE.
 * @details Los envíos se parten en paquetes de `chunk_size` bytes separados por
 *          `delay_ms`. Sin efecto práctico en módulos Classic.
 * @param[in,out] bt         Manejador.
 * @param[in]     chunk_size Bytes por paquete (1..65535; típico 20).
 * @param[in]     delay_ms   Pausa entre paquetes en ms (0 = sin pausa).
 * @retval BT_OK            Aplicado.
 * @retval BT_INVALID_PARAM `bt` nulo o `chunk_size` igual a 0.
 */
BT_Status_t BT_SetBLEPacketing(BT_Handler_t *bt, uint16_t chunk_size, uint16_t delay_ms)
{
    if (bt == NULL || chunk_size == 0u) return BT_INVALID_PARAM;
    bt->ble_chunk_size  = chunk_size;
    bt->ble_tx_delay_ms = delay_ms;
    return BT_OK;
}

/**
 * @brief Indica si el módulo configurado es de tipo BLE.
 * @param[in] bt Manejador.
 * @retval true  HM-10 o BLE genérico.
 * @retval false Classic (HC-05/HC-06) o `bt` nulo.
 */
bool BT_IsBLE(const BT_Handler_t *bt)
{
    if (bt == NULL) return false;
    return (bt->module_type == BT_TYPE_HM10 ||
            bt->module_type == BT_TYPE_BLE_GENERIC);
}

/* ========================================================================== */
/*                          SUPERVISIÓN DEL ENLACE                            */
/* ========================================================================== */

/**
 * @brief Consulta el estado del enlace según el pin STATE (con antirrebote).
 * @details Nivel bajo: desconexión inmediata. Nivel alto: conectado si no hay
 *          antirrebote o si se mantuvo estable el tiempo configurado. Esto
 *          descarta el parpadeo de STATE de los módulos BLE sin conexión.
 * @param[in,out] bt Manejador (actualiza el cronómetro del antirrebote).
 * @retval true  Enlace activo, o no hay pin STATE configurado.
 * @retval false Enlace caído, aún no estabilizado, o `bt` nulo.
 */
bool BT_IsConnected(BT_Handler_t *bt)
{
    if (bt == NULL) return false;

    /* Sin pin STATE configurado: se asume enlace activo */
    if (bt->state_port == NULL) return true;

    /* Nivel bajo: desconexión inmediata y se cancela el cronómetro */
    if (HAL_GPIO_ReadPin(bt->state_port, bt->state_pin) != GPIO_PIN_SET) {
        bt->high_pending = false;
        return false;
    }

    /* Nivel alto sin antirrebote: conectado */
    if (bt->state_debounce_ms == 0u) return true;

    /* Nivel alto con antirrebote: debe mantenerse estable el tiempo configurado. */
    uint32_t now = HAL_GetTick();
    if (!bt->high_pending) {
        bt->high_pending  = true;
        bt->high_since_ms = now;
    }
    return ((now - bt->high_since_ms) >= bt->state_debounce_ms);
}

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
BT_Status_t BT_CheckAndRecover(BT_Handler_t *bt)
{
    if (bt == NULL) return BT_INVALID_PARAM;

    bool connected = BT_IsConnected(bt);

    if (bt->was_connected && !connected) {
        /* Flanco de bajada (conectado -> desconectado):
           1) detener la ISR, 2) recién entonces limpiar los índices (evita carrera
           con la ISR), 3) volver a escuchar (necesario para respuestas AT en BLE). */
        (void)HAL_UART_AbortReceive(bt->huart);
        bt->rx_buffer.head = 0;
        bt->rx_buffer.tail = 0;
        bt->was_connected  = false;
        (void)BT_RearmRx(bt);
        return BT_DISCONNECTED;
    }

    if (!bt->was_connected && connected) {
        /* Flanco de subida (reconexión): solo se actualiza el estado.
           No se descartan datos: el antirrebote retrasa esta detección y ya
           pudieron llegar datos válidos del dispositivo remoto. */
        bt->was_connected = true;
    }

    /* Autorrecuperación: si la recepción quedó detenida (error de UART),
       se vuelve a armar, haya o no enlace. */
    if (BT_RearmRx(bt) != BT_OK) return BT_ERROR;

    return connected ? BT_OK : BT_DISCONNECTED;
}

/* ========================================================================== */
/*                              TRANSMISIÓN                                   */
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
BT_Status_t BT_SendString(BT_Handler_t *bt, const char *str)
{
    if (bt == NULL || str == NULL) return BT_INVALID_PARAM;
    if (!BT_IsConnected(bt))       return BT_DISCONNECTED;

    size_t len = strlen(str);
    if (len == 0u) return BT_OK;   /* nada que enviar (la HAL rechazaría tamaño 0) */

    return BT_TxRaw(bt, (const uint8_t *)str, len);
}

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
BT_Status_t BT_SendData(BT_Handler_t *bt, const uint8_t *data, uint16_t size)
{
    if (bt == NULL || data == NULL || size == 0u) return BT_INVALID_PARAM;
    if (!BT_IsConnected(bt))                      return BT_DISCONNECTED;

    return BT_TxRaw(bt, data, size);
}

/* ========================================================================== */
/*                               RECEPCIÓN                                    */
/* ========================================================================== */

/**
 * @brief Cantidad de bytes pendientes de lectura en el búfer.
 * @param[in] bt Manejador.
 * @return Bytes disponibles (0 si `bt` es nulo).
 */
uint16_t BT_Available(const BT_Handler_t *bt)
{
    if (bt == NULL) return 0;
    /* Con tamaño potencia de 2, la resta enmascarada resuelve el cruce del fin del arreglo */
    return (uint16_t)((bt->rx_buffer.head - bt->rx_buffer.tail) & BT_RX_MASK);
}

/**
 * @brief Lee un byte del búfer circular sin bloquear.
 * @param[in,out] bt Manejador.
 * @return Byte leído (0..255) o -1 si el búfer está vacío o `bt` es nulo.
 */
int16_t BT_ReadChar(BT_Handler_t *bt)
{
    if (bt == NULL || bt->rx_buffer.head == bt->rx_buffer.tail) return -1;

    uint8_t ch = bt->rx_buffer.buffer[bt->rx_buffer.tail];
    bt->rx_buffer.tail = (uint16_t)((bt->rx_buffer.tail + 1u) & BT_RX_MASK);
    return (int16_t)ch;
}

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
BT_Status_t BT_ReadString(BT_Handler_t *bt, char *dest, uint16_t max_len,
                          uint32_t timeout_ms)
{
    /* max_len < 2 no deja lugar para un carácter y el terminador */
    if (bt == NULL || dest == NULL || max_len < 2u) return BT_INVALID_PARAM;

    uint32_t start = HAL_GetTick();
    uint16_t idx = 0;

    /* La verificación de espacio se hace ANTES de escribir, así siempre queda
       al menos una posición libre para el '\0'. */
    while (idx < (uint16_t)(max_len - 1u) && (HAL_GetTick() - start) < timeout_ms) {
        int16_t ch = BT_ReadChar(bt);
        if (ch != -1) {
            dest[idx++] = (char)ch;
            if (ch == '\n') break;     /* fin de línea */
        }
    }

    dest[idx] = '\0';
    return (idx > 0u) ? BT_OK : BT_TIMEOUT;
}

/**
 * @brief Descarta todos los datos pendientes del búfer de recepción.
 * @note  Segura frente a la ISR (solo mueve `tail`).
 * @param[in,out] bt Manejador.
 */
void BT_Flush(BT_Handler_t *bt)
{
    if (bt == NULL) return;
    /* Solo se mueve `tail` (lado consumidor): es seguro aunque la ISR siga escribiendo */
    bt->rx_buffer.tail = bt->rx_buffer.head;
}

/**
 * @brief Cantidad acumulada de bytes descartados por búfer lleno.
 * @details Un valor creciente indica que la aplicación lee demasiado lento o que
 *          conviene aumentar @ref BT_RX_BUFFER_SIZE.
 * @param[in] bt Manejador.
 * @return Bytes perdidos desde @ref BT_Init (0 si `bt` es nulo).
 */
uint32_t BT_GetRxOverflowCount(const BT_Handler_t *bt)
{
    return (bt != NULL) ? bt->rx_overflow : 0u;
}

/* ========================================================================== */
/*                         COMANDOS AT PARA BLE                               */
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
                          uint16_t resp_size, uint32_t timeout_ms)
{
    if (bt == NULL || cmd == NULL || !BT_IsBLE(bt)) return BT_INVALID_PARAM;

    /* Con enlace activo el módulo trata el texto como datos, no como AT */
    if (bt->state_port != NULL && BT_IsConnected(bt)) return BT_BUSY;

    size_t len = strlen(cmd);
    if (len == 0u || len > UINT16_MAX) return BT_INVALID_PARAM;

    /* Se descartan restos previos para no confundirlos con la respuesta */
    BT_Flush(bt);

    /* Transmisión directa a la HAL (BT_SendData rechazaría sin enlace).
       Los AT del HM-10 se envían sin terminador CR/LF. */
    BT_Status_t st = BT_FromHAL(HAL_UART_Transmit(bt->huart, (uint8_t *)cmd,
                                                  (uint16_t)len, bt->timeout_ms));
    if (st != BT_OK) return st;

    /* Recolección de la respuesta: espera el primer byte hasta `timeout_ms` y luego
       termina cuando la línea queda en silencio BT_AT_IDLE_GAP_MS. */
    uint32_t t0   = HAL_GetTick();
    uint32_t last = t0;
    uint16_t n    = 0;
    bool got      = false;
    bool can_store = (resp != NULL && resp_size > 0u);

    while ((HAL_GetTick() - t0) < timeout_ms) {
        int16_t c = BT_ReadChar(bt);
        if (c != -1) {
            if (can_store && n < (uint16_t)(resp_size - 1u)) {
                resp[n++] = (char)c;   /* si no hay espacio, se descarta el excedente */
            }
            got  = true;
            last = HAL_GetTick();
        } else if (got && (HAL_GetTick() - last) >= BT_AT_IDLE_GAP_MS) {
            break;                      /* respuesta completa */
        }
    }

    if (can_store) resp[n] = '\0';
    return got ? BT_OK : BT_TIMEOUT;
}

/**
 * @brief Cambia el nombre anunciado del módulo BLE (AT+NAME).
 * @param[in,out] bt   Manejador.
 * @param[in]     name Nombre (1..@ref BT_BLE_NAME_MAX_LEN caracteres).
 * @retval BT_OK            Nombre aplicado.
 * @retval BT_INVALID_PARAM Nombre nulo, vacío o demasiado largo; módulo no BLE.
 * @retval BT_ERROR         Respuesta inesperada.
 * @retval BT_TIMEOUT / BT_BUSY  Ver @ref BT_BLE_SendAT.
 */
BT_Status_t BT_BLE_SetName(BT_Handler_t *bt, const char *name)
{
    if (bt == NULL || name == NULL) return BT_INVALID_PARAM;

    size_t nlen = strlen(name);
    if (nlen == 0u || nlen > BT_BLE_NAME_MAX_LEN) return BT_INVALID_PARAM;

    /* Se arma "AT+NAME" + nombre (prefijo de 7 caracteres + nombre + '\0') */
    char cmd[7u + BT_BLE_NAME_MAX_LEN + 1u];
    memcpy(cmd, "AT+NAME", 7u);
    memcpy(&cmd[7], name, nlen);
    cmd[7u + nlen] = '\0';

    return BT_AT_Expect_OK(bt, cmd, NULL, 0);
}

/**
 * @brief Habilita o deshabilita los avisos "OK+CONN" / "OK+LOST" (AT+NOTI).
 * @note  Si se habilitan, esos textos llegan al búfer de recepción al conectar
 *        y desconectar.
 * @param[in,out] bt     Manejador.
 * @param[in]     enable true para habilitar.
 * @retval BT_OK / BT_ERROR / BT_TIMEOUT / BT_BUSY / BT_INVALID_PARAM
 */
BT_Status_t BT_BLE_EnableNotifications(BT_Handler_t *bt, bool enable)
{
    return BT_AT_Expect_OK(bt, enable ? "AT+NOTI1" : "AT+NOTI0", NULL, 0);
}

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
BT_Status_t BT_BLE_GetAddress(BT_Handler_t *bt, char *mac, uint16_t mac_size)
{
    if (bt == NULL || mac == NULL || mac_size < BT_BLE_ADDR_BUF_SIZE) {
        return BT_INVALID_PARAM;
    }

    char resp[BT_AT_LOCAL_RESP_SIZE];
    BT_Status_t st = BT_AT_Expect_OK(bt, "AT+ADDR?", resp, sizeof(resp));
    if (st != BT_OK) return st;

    /* Formato esperado: "OK+ADDR:001A7DDA7113" -> se toma lo que sigue a ':' */
    const char *p = strchr(resp, ':');
    if (p == NULL) return BT_ERROR;
    p++;

    uint8_t i = 0;
    while (i < 12u && p[i] != '\0') {
        mac[i] = p[i];
        i++;
    }
    mac[i] = '\0';

    return (i == 12u) ? BT_OK : BT_ERROR;   /* una MAC completa tiene 12 caracteres */
}

/**
 * @brief Reinicia el módulo BLE (AT+RESET).
 * @note  El módulo tarda unos cientos de ms en volver a estar operativo.
 * @param[in,out] bt Manejador.
 * @retval BT_OK / BT_ERROR / BT_TIMEOUT / BT_BUSY / BT_INVALID_PARAM
 */
BT_Status_t BT_BLE_Reset(BT_Handler_t *bt)
{
    return BT_AT_Expect_OK(bt, "AT+RESET", NULL, 0);
}

/* ========================================================================== */
/*                        CALLBACKS (contexto de interrupción)                */
/* ========================================================================== */

/**
 * @brief Procesa un byte recibido: lo guarda en el búfer y rearma la recepción.
 * @note  Invocar desde `HAL_UART_RxCpltCallback` cuando `huart` coincida con la
 *        UART del manejador. Si el búfer está lleno, el byte se descarta y se
 *        incrementa el contador de overflow.
 * @param[in,out] bt Manejador.
 */
void BT_RxISR_Callback(BT_Handler_t *bt)
{
    if (bt == NULL) return;

    uint16_t next_head = (uint16_t)((bt->rx_buffer.head + 1u) & BT_RX_MASK);

    if (next_head != bt->rx_buffer.tail) {
        /* Hay espacio: se guarda el byte y se avanza la cabeza */
        bt->rx_buffer.buffer[bt->rx_buffer.head] = bt->rx_temp_byte;
        bt->rx_buffer.head = next_head;
    } else {
        /* Búfer lleno: el byte se pierde; se contabiliza para depuración */
        bt->rx_overflow++;
    }

    /* Se arma la recepción del próximo byte. Si fallara, BT_CheckAndRecover()
       lo detecta (RxState == READY) y la rearma en su siguiente llamada. */
    (void)HAL_UART_Receive_IT(bt->huart, &bt->rx_temp_byte, 1);
}

/**
 * @brief Recupera la recepción tras un error de la UART (overrun, framing, ruido).
 * @note  Invocar desde `HAL_UART_ErrorCallback` cuando `huart` coincida. Si no
 *        se usa, @ref BT_CheckAndRecover rearmará la recepción en su próxima llamada.
 * @param[in,out] bt Manejador.
 */
void BT_ErrorISR_Callback(BT_Handler_t *bt)
{
    if (bt == NULL) return;
    /* Tras un error la HAL puede haber detenido la recepción: se vuelve a armar */
    (void)BT_RearmRx(bt);
}

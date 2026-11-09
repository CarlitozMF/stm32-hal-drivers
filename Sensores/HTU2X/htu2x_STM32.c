/**
 * @file htu2x_STM32.c
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Implementación NO BLOQUEANTE del driver para el sensor HTU21D/HTU2x.
 * @details Emplea una máquina de estados cooperativa basada en temporización por ticks, comandos No-Hold,
 *          validación de CRC-8 y del tipo de trama, compensación térmica, reintento ante NACK y
 *          recuperación del bus I2C ante errores.
 * @version 2.2
 * @date 2026
 */

#include "htu2x_STM32.h"

/* ============================================================================
 * DEFINICIONES Y MACROS PRIVADAS
 * ============================================================================ */
#define HTU2X_I2C_ADDR          (0x40 << 1) /**< Dirección I2C de 7 bits desplazada para la HAL de ST (0x80) */

/* Comandos estándar del sensor (Modo No Hold Master) */
#define HTU2X_CMD_TRIG_T        0xF3        /**< Disparar medición asíncrona de Temperatura */
#define HTU2X_CMD_TRIG_RH       0xF5        /**< Disparar medición asíncrona de Humedad Relativa */
#define HTU2X_CMD_RESET         0xFE        /**< Comando de Software Reset por bus I2C */

/* Tiempos máximos de conversión especificados en el datasheet [ms] (resolución por defecto: 14b T / 12b RH) */
#define HTU2X_RESET_TIME_MS     15U         /**< Tiempo de recuperación post-reset requerido por el chip */
#define HTU2X_T_CONV_TIME_MS    50U         /**< Tiempo máximo de conversión de Temperatura (14 bits) */
#define HTU2X_RH_CONV_TIME_MS   16U         /**< Tiempo máximo de conversión de Humedad Relativa (12 bits) */
#define HTU2X_I2C_TIMEOUT_MS    10U         /**< Timeout de seguridad corto para transacciones I2C individuales */
#define HTU2X_READ_RETRY_MS     10U         /**< Ventana extra de reintentos si el sensor responde NACK al leer */

/* Tipo de medición codificado en el bit 1 del LSB de la trama (datasheet HTU21D) */
#define HTU2X_TYPE_TEMP         0U          /**< Bit de estado = 0: la trama contiene temperatura */
#define HTU2X_TYPE_RH           1U          /**< Bit de estado = 1: la trama contiene humedad relativa */

/**
 * Habilita la verificación del bit de tipo de medición. Poner en 0 (por ejemplo con -DHTU2X_CHECK_STATUS_BIT=0)
 * si se usa un clon que no respeta ese bit.
 */
#ifndef HTU2X_CHECK_STATUS_BIT
#define HTU2X_CHECK_STATUS_BIT  1
#endif

/* ============================================================================
 * ESTADOS DE LA MÁQUINA DE ESTADOS INTERNA
 * ============================================================================ */
typedef enum {
    ST_UNINIT = 0,      /**< Driver sin inicializar o en estado de fallo crítico */
    ST_RESETTING,       /**< Esperando el transcurso del tiempo de recuperación post-reset */
    ST_IDLE,            /**< Sistema libre, a la espera de órdenes de medición */
    ST_WAIT_TEMP,       /**< Esperando el fin de la conversión del comando de temperatura */
    ST_WAIT_RH          /**< Esperando el fin de la conversión del comando de humedad */
} HTU2X_State_t;

/* ============================================================================
 * VARIABLES ESTÁTICAS PRIVADAS DEL DRIVER
 * ============================================================================ */
static I2C_HandleTypeDef *htu_hi2c = NULL;  /**< Puntero al manejador del periférico I2C de la HAL */
static HTU2X_State_t state = ST_UNINIT;     /**< Estado actual de la máquina de estados cooperativa */
static uint32_t t_start = 0;                /**< Marca de tiempo inicial (tick) para control de retardos */

static float pending_temp = 0.0f;           /**< Temperatura parcial del ciclo en curso (aún no publicada) */
static float last_temp = 0.0f;              /**< Última temperatura válida publicada (°C) */
static float last_rh = 0.0f;                /**< Última humedad relativa compensada publicada (%RH) */
static bool  data_valid = false;            /**< Bandera que indica si ya se completó al menos un ciclo */

/* ============================================================================
 * FUNCIONES AUXILIARES PRIVADAS
 * ============================================================================ */

/* Comprueba si ya transcurrió un intervalo de tiempo, de forma segura ante desbordamiento del contador HAL_GetTick */
static bool elapsed(uint32_t ms) {
    return (HAL_GetTick() - t_start) >= ms;
}

/* Calcula el CRC-8 del HTU2x usando el polinomio x^8 + x^5 + x^4 + 1 (0x31) con valor inicial 0 */
static uint8_t crc8(const uint8_t *data, uint8_t len) {
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* Indica si la última transacción terminó con NACK (AF). HAL_I2C_GetError() es una máscara de bits */
static bool last_was_nack(void) {
    return (HAL_I2C_GetError(htu_hi2c) & HAL_I2C_ERROR_AF) != 0U;
}

/* Reinicializa el periférico I2C para recuperar el bus ante bloqueos o errores de línea.
 * No genera pulsos de SCL: si un esclavo retiene SDA en bajo hay que liberarlo por fuera del driver. */
static void recover_bus(void) {
    if (htu_hi2c == NULL) return;

    /* Un NACK puro (AF) indica bus sano y sensor ausente o temporalmente ocupado: no requiere resetear */
    if (last_was_nack()) return;

#if defined(I2C_CR1_SWRST)
    /* Periférico I2C "v1" (F1, F2, F4, L1): reset por software del bloque */
    __HAL_I2C_DISABLE(htu_hi2c);
    htu_hi2c->Instance->CR1 |=  I2C_CR1_SWRST;
    htu_hi2c->Instance->CR1 &= ~I2C_CR1_SWRST;
    (void)HAL_I2C_Init(htu_hi2c);
#else
    /* Periférico I2C "v2" (F0, G0, G4, L0, L4, H7, ...): no existe SWRST, se usa DeInit + Init.
     * Requiere que HAL_I2C_MspInit()/MspDeInit() estén implementados (los genera CubeMX). */
    (void)HAL_I2C_DeInit(htu_hi2c);
    (void)HAL_I2C_Init(htu_hi2c);
#endif
}

/* Envía un comando I2C al sensor y captura la marca de tiempo (tick) inicial */
static HTU2X_Status_t send_cmd(uint8_t cmd) {
    if (HAL_I2C_Master_Transmit(htu_hi2c, HTU2X_I2C_ADDR, &cmd, 1, HTU2X_I2C_TIMEOUT_MS) != HAL_OK) {
        return HTU2X_ERROR_I2C;
    }
    t_start = HAL_GetTick();
    return HTU2X_OK;
}

/* Lee la trama de 3 bytes devuelta por el sensor, verifica el CRC-8 y el tipo de medición,
 * y extrae el valor bruto de 16 bits. expected_type: HTU2X_TYPE_TEMP o HTU2X_TYPE_RH */
static HTU2X_Status_t read_raw(uint16_t *raw, uint8_t expected_type) {
    uint8_t data[3];

    if (HAL_I2C_Master_Receive(htu_hi2c, HTU2X_I2C_ADDR, data, 3, HTU2X_I2C_TIMEOUT_MS) != HAL_OK) {
        return HTU2X_ERROR_TIMEOUT;
    }
    if (crc8(data, 2) != data[2]) {
        return HTU2X_ERROR_CRC;
    }

#if HTU2X_CHECK_STATUS_BIT
    /* Bit 1 del LSB: 0 = temperatura, 1 = humedad. Detecta tramas cruzadas entre mediciones */
    if (((data[1] >> 1) & 0x01U) != expected_type) {
        return HTU2X_ERROR_CRC;
    }
#else
    (void)expected_type;
#endif

    /* Limpia los 2 bits de estado menos significativos (bits [1:0] reservados para flags internas) */
    *raw = (uint16_t)(((uint16_t)data[0] << 8) | (data[1] & 0xFC));
    return HTU2X_OK;
}

/* Decide si un fallo de lectura debe reintentarse en la próxima llamada a Process():
 * solo si fue un NACK (conversión posiblemente no terminada) y la ventana extra aún no venció */
static bool should_retry_read(HTU2X_Status_t st, uint32_t conv_ms) {
    return (st == HTU2X_ERROR_TIMEOUT) &&
           last_was_nack() &&
           !elapsed(conv_ms + HTU2X_READ_RETRY_MS);
}

/* Gestiona las fallas del sistema: recupera el bus si hubo error de comunicación y devuelve el estado a IDLE */
static HTU2X_Status_t fail(HTU2X_Status_t err) {
    if (err == HTU2X_ERROR_I2C || err == HTU2X_ERROR_TIMEOUT) {
        recover_bus();
    }
    state = ST_IDLE;
    return err;
}

/* ============================================================================
 * IMPLEMENTACIÓN DE LA API PÚBLICA
 * ============================================================================ */

/* Guarda el handle I2C y envía el comando de reset por software al sensor */
HTU2X_Status_t HTU2X_Init(I2C_HandleTypeDef *hi2c) {
    if (hi2c == NULL) return HTU2X_ERROR_I2C;

    htu_hi2c = hi2c;
    data_valid = false;

    if (send_cmd(HTU2X_CMD_RESET) != HTU2X_OK) {
        state = ST_UNINIT;
        return HTU2X_ERROR_I2C;
    }

    state = ST_RESETTING; /* HTU2X_Process() transicionará automáticamente a IDLE al cumplirse el tiempo */
    return HTU2X_OK;
}

/* Inicia un nuevo ciclo de medición asíncrona (temperatura primero, humedad después) */
HTU2X_Status_t HTU2X_StartMeasurement(void) {
    if (state == ST_UNINIT) return HTU2X_ERROR_NOT_INIT;
    if (state != ST_IDLE)   return HTU2X_BUSY;

    if (send_cmd(HTU2X_CMD_TRIG_T) != HTU2X_OK) {
        return fail(HTU2X_ERROR_I2C);
    }

    state = ST_WAIT_TEMP;
    return HTU2X_OK;
}

/* Motor principal de la máquina de estados cooperativa. Debe llamarse continuamente en el bucle principal */
HTU2X_Status_t HTU2X_Process(void) {
    uint16_t raw;
    HTU2X_Status_t st;

    switch (state) {

        case ST_UNINIT:
            return HTU2X_ERROR_NOT_INIT;

        case ST_IDLE:
            return HTU2X_OK;

        case ST_RESETTING:
            if (elapsed(HTU2X_RESET_TIME_MS)) {
                state = ST_IDLE;
            }
            return HTU2X_BUSY;

        case ST_WAIT_TEMP:
        {
            if (!elapsed(HTU2X_T_CONV_TIME_MS)) {
                return HTU2X_BUSY;
            }

            st = read_raw(&raw, HTU2X_TYPE_TEMP);
            if (should_retry_read(st, HTU2X_T_CONV_TIME_MS)) return HTU2X_BUSY;
            if (st != HTU2X_OK) return fail(st);

            /* Conversión de temperatura: T = -46.85 + (175.72 * raw / 2^16) */
            pending_temp = -46.85f + (175.72f * (float)raw / 65536.0f);

            /* Encadena de forma inmediata el disparo de la medición de Humedad Relativa */
            if (send_cmd(HTU2X_CMD_TRIG_RH) != HTU2X_OK) {
                return fail(HTU2X_ERROR_I2C);
            }
            state = ST_WAIT_RH;
            return HTU2X_BUSY;
        }

        case ST_WAIT_RH:
        {
            if (!elapsed(HTU2X_RH_CONV_TIME_MS)) {
                return HTU2X_BUSY;
            }

            st = read_raw(&raw, HTU2X_TYPE_RH);
            if (should_retry_read(st, HTU2X_RH_CONV_TIME_MS)) return HTU2X_BUSY;
            if (st != HTU2X_OK) return fail(st);

            /* Conversión de humedad relativa: RH = -6.0 + (125.0 * raw / 2^16) */
            float rh = -6.0f + (125.0f * (float)raw / 65536.0f);

            /* Aplicación de compensación térmica respecto a 25°C utilizando la temperatura medida en el mismo ciclo */
            rh += (25.0f - pending_temp) * (-0.15f);

            /* Saturación de límites físicos (0% a 100% RH) */
            if (rh < 0.0f)   rh = 0.0f;
            if (rh > 100.0f) rh = 100.0f;

            /* Publicación atómica de T y RH: garantiza consistencia absoluta entre ambos valores */
            last_temp  = pending_temp;
            last_rh    = rh;
            data_valid = true;

            state = ST_IDLE;
            return HTU2X_DATA_READY;
        }

        default:
            /* Estado corrupto: se pasa a UNINIT; hay que volver a llamar a HTU2X_Init() */
            state = ST_UNINIT;
            return HTU2X_ERROR_NOT_INIT;
    }
}

/* Consulta si el driver se encuentra en estado de reposo (IDLE) */
bool HTU2X_IsIdle(void) {
    return (state == ST_IDLE);
}

/* Copia la última medición completa hacia las variables provistas por el usuario */
bool HTU2X_GetData(float *temperature, float *humidity) {
    if (!data_valid) return false;

    if (temperature != NULL) *temperature = last_temp;
    if (humidity    != NULL) *humidity    = last_rh;
    return true;
}

/* Convierte un código de estado numérico en una cadena de texto legible para depuración o UI */
const char *HTU2X_StatusToString(HTU2X_Status_t status) {
    switch (status) {
        case HTU2X_OK:             return "OK";
        case HTU2X_ERROR_I2C:      return "Error I2C";
        case HTU2X_ERROR_CRC:      return "Error CRC / trama invalida";
        case HTU2X_ERROR_TIMEOUT:  return "Error timeout";
        case HTU2X_ERROR_NOT_INIT: return "No inicializado";
        case HTU2X_BUSY:           return "Ocupado";
        case HTU2X_DATA_READY:     return "Datos listos";
        default:                   return "Desconocido";
    }
}

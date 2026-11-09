/**
 * @file htu2x_STM32.h
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Interfaz del driver NO BLOQUEANTE para el sensor de temperatura y humedad HTU21D/HTU2x via I2C.
 * @details Define la máquina de estados cooperativa basada en HAL_GetTick(), eliminando el uso de
 *          retardos bloqueantes (HAL_Delay) durante las esperas de conversión.
 *
 *          Flujo de uso recomendado:
 *          1. HTU2X_Init()             -> Guarda el handle I2C y envía el reset por software.
 *                                         (El bus debe estar ya inicializado, ej: MX_I2C1_Init()).
 *          2. HTU2X_Process()          -> Llamar permanentemente en cada iteración del bucle principal.
 *          3. HTU2X_StartMeasurement() -> Inicia un ciclo de lectura completo (T + RH) de forma asíncrona.
 *          4. HTU2X_GetData()          -> Obtiene los valores cuando HTU2X_Process() retorna HTU2X_DATA_READY.
 *
 *          Máquina de estados interna:
 *          @code
 *          UNINIT --Init--> RESETTING --15 ms--> IDLE --StartMeasurement--> WAIT_TEMP
 *                                                 ^                            | 50 ms: lee T, dispara RH
 *                                                 |                            v
 *                                                 +------ DATA_READY ------ WAIT_RH (16 ms: lee RH)
 *          @endcode
 *          Cualquier error en los estados de espera devuelve el driver a IDLE.
 *
 * @note Alcance del "no bloqueante": las esperas de conversión (hasta 50 ms) son cooperativas y no
 *       consumen CPU. Sin embargo, cada transacción I2C individual usa la HAL en modo polling: con el
 *       bus sano dura ~0.3-0.5 ms a 100 kHz, pero con el bus colgado puede bloquear hasta
 *       HTU2X_I2C_TIMEOUT_MS (10 ms) antes de abortar.
 *
 * @warning El driver NO es reentrante: todas sus funciones deben llamarse desde el mismo contexto
 *          (típicamente el bucle principal), nunca mezclando main e ISR.
 *
 * @warning Recuperación del bus: ante errores I2C el driver reinicializa el periférico, pero NO
 *          genera pulsos de SCL. Si un esclavo mantiene SDA en bajo, hay que liberar el bus por fuera
 *          del driver (hasta 9 pulsos de SCL en modo GPIO) y luego llamar a HTU2X_Init().
 *
 * @version 2.2
 * @date 2026
 */

#ifndef HTU2X_STM32_H
#define HTU2X_STM32_H

#include "main.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Códigos de estado del driver del HTU2x.
 * @note Los códigos de error son contiguos (HTU2X_ERROR_I2C ... HTU2X_ERROR_NOT_INIT); usar
 *       HTU2X_IS_ERROR() para detectarlos sin depender del orden.
 */
typedef enum {
    HTU2X_OK = 0,          /**< Sin novedades (driver inactivo y operando correctamente) */
    HTU2X_ERROR_I2C,       /**< Fallo de comunicación física en el bus I2C */
    HTU2X_ERROR_CRC,       /**< Error de integridad: CRC-8 inválido o tipo de trama inesperado (T vs RH) */
    HTU2X_ERROR_TIMEOUT,   /**< Tiempo de espera agotado / NACK persistente durante la recepción I2C */
    HTU2X_ERROR_NOT_INIT,  /**< Driver sin inicializar (falta HTU2X_Init() o este falló) */
    HTU2X_BUSY,            /**< Operación en curso (máquina de estados ocupada) */
    HTU2X_DATA_READY       /**< Ciclo de medición finalizado: nuevos datos válidos disponibles */
} HTU2X_Status_t;

/**
 * @brief Evalúa si un HTU2X_Status_t es un código de error.
 * @param[in] s Código devuelto por las funciones del driver.
 */
#define HTU2X_IS_ERROR(s)  ((s) >= HTU2X_ERROR_I2C && (s) <= HTU2X_ERROR_NOT_INIT)

/**
 * @brief Guarda el handle I2C y envía el comando de reset por software de forma no bloqueante.
 * @note El sensor requiere ~15 ms desde su alimentación antes de aceptar comandos. Si se llama
 *       demasiado pronto tras el encendido puede devolver HTU2X_ERROR_I2C; en ese caso se puede
 *       reintentar llamando nuevamente a esta función.
 * @note El tiempo de recuperación post-reset (~15 ms) lo gestiona internamente HTU2X_Process().
 * @param[in] hi2c Puntero al manejador del bus I2C de la HAL de ST (ej: &hi2c1), ya inicializado.
 * @return HTU2X_Status_t HTU2X_OK si se inició correctamente, o HTU2X_ERROR_I2C si falló.
 */
HTU2X_Status_t HTU2X_Init(I2C_HandleTypeDef *hi2c);

/**
 * @brief Avanza la máquina de estados del driver. Debe llamarse de forma continua en el bucle principal.
 * @details Gestiona los tiempos de espera de conversión de manera transparente usando HAL_GetTick().
 *          Si el sensor responde NACK al leer (conversión aún no terminada), reintenta durante una
 *          ventana corta adicional antes de declarar error. Ante errores de I2C/timeout reinicializa
 *          el periférico para recuperar el bus.
 * @return HTU2X_Status_t
 *         - HTU2X_BUSY: El driver se encuentra procesando una medición o reiniciándose.
 *         - HTU2X_DATA_READY: Un ciclo de medición finalizó con éxito (se emite una única vez por ciclo).
 *         - HTU2X_OK: El driver está inactivo y a la espera de órdenes.
 *         - HTU2X_ERROR_NOT_INIT: El driver no fue inicializado (o la inicialización falló).
 *         - HTU2X_ERROR_xxx: Código de error si falló el I2C o el CRC (el driver vuelve a IDLE).
 */
HTU2X_Status_t HTU2X_Process(void);

/**
 * @brief Inicia un nuevo ciclo de medición asíncrona (temperatura seguida de humedad relativa).
 * @return HTU2X_Status_t
 *         - HTU2X_OK: el ciclo arrancó con éxito.
 *         - HTU2X_BUSY: el driver no está libre (reseteándose o midiendo).
 *         - HTU2X_ERROR_NOT_INIT: el driver no fue inicializado.
 *         - HTU2X_ERROR_I2C: falló el envío del comando.
 */
HTU2X_Status_t HTU2X_StartMeasurement(void);

/**
 * @brief Verifica si el driver se encuentra libre para iniciar una nueva solicitud de medición.
 * @return true Si el driver está en estado IDLE, false si está ocupado o sin inicializar.
 */
bool HTU2X_IsIdle(void);

/**
 * @brief Obtiene la última medición completa y consistente (T y RH del mismo ciclo).
 * @param[out] temperature Puntero donde se almacenará la temperatura en °C (puede ser NULL).
 * @param[out] humidity Puntero donde se almacenará la humedad relativa compensada en % (puede ser NULL).
 * @return true Si existe al menos una medición válida (los punteros fueron cargados).
 * @return false Si todavía no se completó ninguna medición (los punteros no se modifican).
 */
bool HTU2X_GetData(float *temperature, float *humidity);

/**
 * @brief Convierte un código de estado en texto legible.
 * @param[in] status Código devuelto por las funciones del driver.
 * @return Cadena constante con la descripción (nunca NULL).
 */
const char *HTU2X_StatusToString(HTU2X_Status_t status);

#ifdef __cplusplus
}
#endif

#endif /* HTU2X_STM32_H */

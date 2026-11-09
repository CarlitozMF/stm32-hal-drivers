/**
 * @file    lm35_STM32.h
 * @author  Mamani Flores Carlos (UTN FRT)
 * @brief   Driver genérico no bloqueante para el sensor de temperatura LM35 usando ST HAL.
 * @details Realiza la conversión mediante interrupciones (IT) e integra un filtro de
 *          media móvil con buffer circular y suma acumulada, de modo que el costo por
 *          muestra en la ISR es O(1).
 *
 *          Flujo de uso típico:
 *          1. LM35_STM32_Init()
 *          2. LM35_STM32_StartConversion()
 *          3. HAL_ADC_ConvCpltCallback() -> LM35_STM32_ADC_Callback()
 *          4. LM35_STM32_GetTemperature()
 *
 * @version 1.2
 * @date    2026
 */

#ifndef LM35_STM32_H_
#define LM35_STM32_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include <stdint.h>
#include <stddef.h>

/* Exported Constants --------------------------------------------------------*/

/**
 * @brief Tamaño de la ventana del filtro de media móvil (en muestras).
 * @warning DEBE ser potencia de 2 (2, 4, 8, 16...). El driver usa una máscara
 *          en lugar de módulo para el índice circular; se verifica en compilación
 *          con _Static_assert.
 */
#define LM35_FILTER_SIZE    8U

/** @brief Máscara para el índice circular (LM35_FILTER_SIZE - 1). */
#define LM35_FILTER_MASK    (LM35_FILTER_SIZE - 1U)

/* Exported Types ------------------------------------------------------------*/

/**
 * @brief Códigos de retorno de las funciones del driver.
 */
typedef enum {
    LM35_STATUS_OK = 0x00U,     /**< Operación exitosa */
    LM35_STATUS_ERROR,          /**< Error reportado por el HAL del ADC */
    LM35_STATUS_BUSY,           /**< Hay una conversión en curso */
    LM35_STATUS_PARAM_ERROR,    /**< Parámetro inválido (puntero nulo o valor fuera de rango) */
    LM35_STATUS_NO_DATA         /**< Todavía no se completó ninguna conversión válida */
} LM35_Status_t;

/**
 * @brief Estados internos de la conversión.
 */
typedef enum {
    LM35_STATE_IDLE = 0x00U,    /**< Sin conversión en curso */
    LM35_STATE_BUSY,            /**< Conversión iniciada, esperando la interrupción del ADC */
    LM35_STATE_READY            /**< Dato nuevo disponible, aún no leído con GetTemperature() */
} LM35_State_t;

/**
 * @brief Estructura de control (handler) del sensor LM35.
 * @note  Los campos marcados como "uso interno" no deben modificarse desde la aplicación.
 */
typedef struct {
    ADC_HandleTypeDef *hadc;                    /**< Handler HAL del ADC asociado */
    uint32_t adc_channel;                       /**< Canal ADC del sensor (ADC_CHANNEL_x) */
    uint32_t sampling_time;                     /**< Tiempo de muestreo (ADC_SAMPLETIME_x del HAL) */
    float    vref_mv;                           /**< Tensión de referencia del ADC en mV */
    uint32_t adc_max_value;                     /**< Valor máximo del ADC: (2^bits) - 1 (precalculado) */

    uint32_t filter_buffer[LM35_FILTER_SIZE];   /**< Buffer circular de muestras RAW (uso interno) */
    uint32_t filter_sum;                        /**< Suma acumulada de las muestras del buffer (uso interno) */
    uint8_t  buffer_index;                      /**< Posición donde se escribirá la próxima muestra (uso interno) */
    uint8_t  samples_count;                     /**< Muestras válidas en el buffer, de 0 a LM35_FILTER_SIZE (uso interno) */

    volatile uint32_t raw_value;                /**< Última muestra RAW sin filtrar */
    volatile uint32_t avg_raw;                  /**< Promedio RAW de la ventana; lo escribe la ISR */
    volatile LM35_State_t state;                /**< Estado actual de la conversión */
} LM35_STM32_t;

/* Exported Functions Prototypes ---------------------------------------------*/

/**
 * @brief  Inicializa el handler del LM35 y limpia la ventana del filtro.
 * @param  hlm35           Puntero al handler del sensor.
 * @param  hadc            Puntero al handler HAL del ADC (ya inicializado por CubeMX/HAL_ADC_Init).
 * @param  channel         Canal ADC donde está conectado el LM35 (ADC_CHANNEL_x).
 * @param  sampling_time   Tiempo de muestreo (ADC_SAMPLETIME_x). Depende de la familia STM32
 *                         y de la impedancia de salida del LM35.
 * @param  vref_mv         Tensión de referencia del ADC en mV (ej. 3300.0f).
 * @param  resolution_bits Resolución del ADC en bits (1 a 16).
 * @retval LM35_STATUS_OK           Inicialización correcta.
 * @retval LM35_STATUS_PARAM_ERROR  Puntero nulo, vref <= 0 o resolución fuera de rango.
 */
LM35_Status_t LM35_STM32_Init(LM35_STM32_t *hlm35, ADC_HandleTypeDef *hadc,
                              uint32_t channel, uint32_t sampling_time,
                              float vref_mv, uint32_t resolution_bits);

/**
 * @brief  Inicia una conversión analógico-digital de forma NO bloqueante (por interrupción).
 * @details Configura el canal, marca el driver como BUSY y arranca el ADC con
 *          HAL_ADC_Start_IT(). El resultado se procesa en LM35_STM32_ADC_Callback().
 * @param  hlm35 Puntero al handler del sensor.
 * @retval LM35_STATUS_OK           Conversión iniciada.
 * @retval LM35_STATUS_BUSY         Ya hay una conversión en curso.
 * @retval LM35_STATUS_PARAM_ERROR  Puntero nulo.
 * @retval LM35_STATUS_ERROR        Falló la configuración o el arranque del ADC.
 * @note   Es segura ante llamadas concurrentes (usa sección crítica corta).
 */
LM35_Status_t LM35_STM32_StartConversion(LM35_STM32_t *hlm35);

/**
 * @brief  Procesa la muestra del ADC y actualiza el filtro de media móvil.
 * @details Debe llamarse desde HAL_ADC_ConvCpltCallback(). Ignora automáticamente
 *          las interrupciones de otros ADC.
 * @param  hlm35 Puntero al handler del sensor.
 * @param  hadc  Handler del ADC que generó la interrupción (el que entrega el HAL).
 * @note   Se ejecuta en contexto de interrupción; solo hace aritmética entera.
 */
void LM35_STM32_ADC_Callback(LM35_STM32_t *hlm35, ADC_HandleTypeDef *hadc);

/**
 * @brief  Maneja un error del ADC y libera el estado BUSY.
 * @details Debe llamarse desde HAL_ADC_ErrorCallback(). Sin esto, un error del ADC
 *          dejaría al driver en BUSY de forma permanente.
 * @param  hlm35 Puntero al handler del sensor.
 * @param  hadc  Handler del ADC que reportó el error.
 */
void LM35_STM32_ADC_ErrorCallback(LM35_STM32_t *hlm35, ADC_HandleTypeDef *hadc);

/**
 * @brief  Obtiene la última temperatura filtrada.
 * @details Convierte a °C el promedio RAW de la ventana. Si había un dato nuevo
 *          (estado READY), lo marca como consumido y vuelve a IDLE.
 * @param  hlm35  Puntero al handler del sensor.
 * @param  temp_c Puntero donde se almacena la temperatura en °C.
 * @retval LM35_STATUS_OK           Temperatura entregada en @p temp_c.
 * @retval LM35_STATUS_NO_DATA      Aún no se completó ninguna conversión.
 * @retval LM35_STATUS_PARAM_ERROR  Puntero nulo.
 */
LM35_Status_t LM35_STM32_GetTemperature(LM35_STM32_t *hlm35, float *temp_c);

/**
 * @brief  Convierte un valor RAW del ADC a grados Celsius.
 * @details T[°C] = (raw * Vref[mV] / ADC_MAX) / 10 mV/°C
 * @param  hlm35   Puntero al handler (provee Vref y resolución).
 * @param  raw_val Valor RAW del ADC a convertir.
 * @return Temperatura en °C, o 0.0f si el handler es inválido.
 */
float LM35_STM32_ConvertRawToTemp(const LM35_STM32_t *hlm35, uint32_t raw_val);

#ifdef __cplusplus
}
#endif

#endif /* LM35_STM32_H_ */

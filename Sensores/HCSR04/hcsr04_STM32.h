/**
 ******************************************************************************
 * @file    hcsr04_STM32.h
 * @brief   Driver no bloqueante para N sensores ultrasónicos HC-SR04 sobre
 *          cualquier microcontrolador / placa STM32 con HAL.
 ******************************************************************************
 * @details
 * El driver mide el ancho del pulso del pin ECHO mediante la función
 * *Input Capture* de un timer (captura de flanco de subida y de bajada).
 * La medición es totalmente por interrupción: @ref HCSR04_Trigger() sólo
 * dispara el sensor y retorna; el resultado se consulta luego con
 * @ref HCSR04_ReadDistance_cm().
 *
 * ## Portabilidad
 * - Sólo usa la capa HAL (sin registros directos), por lo que funciona en
 *   cualquier familia STM32 (F0, F1, F3, F4, F7, G0, G4, H7, L0, L4, U5, C0...).
 * - NO depende de DWT ni de SysTick para generar el pulso de 10 us (DWT no
 *   existe en Cortex-M0/M0+). El retardo se obtiene del propio timer.
 * - Soporta timers de 16 y 32 bits, cualquier valor de ARR y cualquier
 *   frecuencia de tick (ver @ref HCSR04_DEFAULT_TICK_HZ).
 * - Varios sensores pueden compartir el mismo timer (un canal por sensor) o
 *   usar timers distintos. El contador es de libre carrera: nunca se
 *   reinicia, por lo que un sensor no perturba la medición de otro.
 *
 * ## Filtrado de lecturas
 * Cada medición se valida contra el rango [@ref HCSR04_MIN_CM,
 * @ref HCSR04_MAX_CM] y luego pasa por un filtro de mediana de
 * @ref HCSR04_FILTER_SIZE muestras. Esto elimina picos aislados causados por
 * rebotes acústicos (eco de una pared lejana en lugar del objeto). El filtro
 * introduce un retardo de (N-1)/2 mediciones ante cambios rápidos de distancia.
 * Con HCSR04_FILTER_SIZE = 1 el filtro queda deshabilitado (sólo se valida el rango).
 *
 * ## Configuración en STM32CubeMX
 * 1. Timer -> canal(es) en modo **Input Capture direct mode**.
 * 2. Prescaler tal que el tick sea de 1 MHz (1 tick = 1 us):
 *    `PSC = (f_timer_clk / 1 000 000) - 1`. Si se usa otra frecuencia, indicarla
 *    con @ref HCSR04_SetTickFrequency().
 * 3. ARR = máximo del timer (65535 en 16 bits). Debe cubrir al menos ~40 ms.
 * 4. Habilitar la interrupción global del timer en el NVIC.
 * 5. Pin TRIG: GPIO Output (Push-Pull, nivel inicial bajo).
 * 6. Pin ECHO: entrada del canal del timer. El HC-SR04 entrega 5 V en ECHO:
 *    usar un pin 5V-tolerant (FT) o un divisor resistivo.
 *
 * ## Ejemplo de uso (main.c)
 * @code
 * #include "hcsr04_STM32.h"
 *
 * HCSR04_Handle sensor1, sensor2;   // Un handle por sensor
 *
 * // Inicialización (después de MX_TIMx_Init y MX_GPIO_Init)
 * HCSR04_Init(&sensor1, &htim2, TIM_CHANNEL_1, TRIG1_GPIO_Port, TRIG1_Pin);
 * HCSR04_Init(&sensor2, &htim2, TIM_CHANNEL_2, TRIG2_GPIO_Port, TRIG2_Pin);
 *
 * // Callback de HAL: enrutar cada canal al sensor correspondiente
 * void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
 * {
 *     if (htim->Instance == TIM2) {
 *         if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_1) HCSR04_InputCapture_Callback(&sensor1);
 *         if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_2) HCSR04_InputCapture_Callback(&sensor2);
 *     }
 * }
 *
 * // Lazo principal (disparar cada >= 60 ms por sensor)
 * HCSR04_Trigger(&sensor1);
 * ...
 * float d = HCSR04_ReadDistance_cm(&sensor1);
 * if (d >= 0.0f)                       { // medición válida en cm }
 * else if (d == HCSR04_ERR_TIMEOUT)    { // sin eco: fuera de rango o sensor desconectado }
 * else                                 { // HCSR04_ERR_NOT_READY: todavía midiendo }
 * @endcode
 *
 * @note Los sensores montados en el mismo timer no deben dispararse al mismo
 *       tiempo si sus ecos pueden solaparse (interferencia acústica). Se
 *       recomienda disparar en secuencia.
 ******************************************************************************
 */

#ifndef HCSR04_STM32_H
#define HCSR04_STM32_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stdint.h>

/**
 * @brief Cabecera del proyecto generada por STM32CubeMX.
 * @details "main.h" incluye automáticamente el HAL de la familia utilizada
 *          (stm32f4xx_hal.h, stm32g0xx_hal.h, etc.), lo que hace al driver
 *          independiente de la familia. Si el proyecto no usa CubeMX, reemplazar
 *          por la cabecera HAL específica.
 * @note  Requiere que el módulo HAL_TIM esté habilitado (HAL_TIM_MODULE_ENABLED).
 */
#include "main.h"

/* Configuración (puede redefinirse antes de incluir este archivo) ------------*/

/**
 * @brief Frecuencia de tick del timer por defecto, en Hz.
 * @details Se asume 1 MHz (1 tick = 1 us). Cada handle la copia en Init y
 *          puede modificarse con @ref HCSR04_SetTickFrequency().
 */
#ifndef HCSR04_DEFAULT_TICK_HZ
#define HCSR04_DEFAULT_TICK_HZ      1000000UL
#endif

/**
 * @brief Velocidad del sonido en cm/s (aprox. 343 m/s a 20 °C).
 * @details Redefinir para compensar por temperatura:
 *          v = 33150 + 60 * T(°C) [cm/s] aprox.
 */
#ifndef HCSR04_SOUND_SPEED_CM_S
#define HCSR04_SOUND_SPEED_CM_S     34300.0f
#endif

/**
 * @brief Tiempo máximo de espera del eco, en milisegundos.
 * @details El HC-SR04 mantiene ECHO ~38 ms cuando no detecta obstáculo; 60 ms
 *          deja margen. Superado este tiempo se declara timeout.
 */
#ifndef HCSR04_TIMEOUT_MS
#define HCSR04_TIMEOUT_MS           60U
#endif

/** @brief Distancia mínima válida en cm. Lecturas menores se descartan. */
#ifndef HCSR04_MIN_CM
#define HCSR04_MIN_CM               2.0f
#endif

/** @brief Distancia máxima válida en cm. Lecturas mayores se descartan. */
#ifndef HCSR04_MAX_CM
#define HCSR04_MAX_CM               400.0f
#endif

/**
 * @brief Cantidad de muestras del filtro de mediana (1 a 9).
 * @details Se recomienda un valor impar. 1 = filtro deshabilitado. Con 3 se
 *          rechaza un pico aislado; con 5, hasta dos picos consecutivos.
 *          Cada muestra extra ocupa 4 bytes por sensor.
 */
#ifndef HCSR04_FILTER_SIZE
#define HCSR04_FILTER_SIZE          3U
#endif

#if (HCSR04_FILTER_SIZE < 1) || (HCSR04_FILTER_SIZE > 9)
#error "HCSR04_FILTER_SIZE debe estar entre 1 y 9"
#endif

/** @brief Duración del pulso TRIG en microsegundos (mínimo del HC-SR04: 10 us). */
#define HCSR04_TRIG_PULSE_US        10U

/* Códigos de retorno de HCSR04_ReadDistance_cm() ----------------------------*/

/** @brief La medición aún está en curso (o no se ha disparado el sensor). */
#define HCSR04_ERR_NOT_READY        (-1.0f)

/** @brief No llegó el eco dentro de @ref HCSR04_TIMEOUT_MS. */
#define HCSR04_ERR_TIMEOUT          (-2.0f)

/* Tipos ---------------------------------------------------------------------*/

/**
 * @brief Estados de la máquina de captura de un sensor.
 */
typedef enum {
    HCSR04_STATE_IDLE = 0,      /**< Sin medición en curso.                          */
    HCSR04_STATE_WAIT_RISING,   /**< Disparado; esperando flanco de subida de ECHO.  */
    HCSR04_STATE_WAIT_FALLING,  /**< Esperando flanco de bajada de ECHO.             */
    HCSR04_STATE_READY          /**< Medición completa; lista para leerse.           */
} HCSR04_State;

/**
 * @brief Handle (contexto) de un sensor HC-SR04.
 * @details Se debe declarar un handle por sensor. Los campos marcados como
 *          `volatile` son modificados dentro de la ISR y leídos desde el
 *          lazo principal. No acceder directamente: usar las funciones del driver.
 */
typedef struct {
    TIM_HandleTypeDef*  htim;           /**< Handle HAL del timer de captura.                   */
    uint32_t            channel;        /**< Canal del timer (TIM_CHANNEL_1..4).                */
    GPIO_TypeDef*       trig_port;      /**< Puerto GPIO del pin TRIG.                          */
    uint16_t            trig_pin;       /**< Pin GPIO del pin TRIG (GPIO_PIN_x).                */
    uint32_t            tick_hz;        /**< Frecuencia de tick del timer en Hz.                */
    uint32_t            trigger_tick;   /**< Valor de HAL_GetTick() al disparar (para timeout). */
    volatile HCSR04_State edge_state;   /**< Estado actual de la captura.                       */
    volatile uint32_t   rising_time;    /**< Contador capturado en el flanco de subida.         */
    volatile uint32_t   falling_time;   /**< Contador capturado en el flanco de bajada.         */
    volatile uint32_t   pulse_duration; /**< Duración del pulso ECHO en ticks del timer.        */
    float               filter_buf[HCSR04_FILTER_SIZE]; /**< Historial de muestras válidas (filtro). */
    uint8_t             filter_idx;     /**< Próxima posición a escribir en filter_buf.         */
    uint8_t             filter_count;   /**< Muestras válidas almacenadas (0..FILTER_SIZE).     */
    uint8_t             invalid_count;  /**< Lecturas inválidas consecutivas.                   */
} HCSR04_Handle;

/* API -----------------------------------------------------------------------*/

/**
 * @brief  Inicializa el handle de un sensor.
 * @param  hcsr      Puntero al handle del sensor.
 * @param  htim      Puntero al handle HAL del timer configurado en Input Capture.
 * @param  channel   Canal del timer asociado al pin ECHO (TIM_CHANNEL_1..4).
 * @param  trig_port Puerto GPIO del pin TRIG (ej. GPIOA).
 * @param  trig_pin  Pin GPIO del TRIG (ej. GPIO_PIN_5).
 * @note   Deja el pin TRIG en nivel bajo y el estado en IDLE. Asume un tick
 *         de @ref HCSR04_DEFAULT_TICK_HZ.
 */
void HCSR04_Init(HCSR04_Handle* hcsr, TIM_HandleTypeDef* htim, uint32_t channel,
                 GPIO_TypeDef* trig_port, uint16_t trig_pin);

/**
 * @brief  Define la frecuencia de tick del timer si no es 1 MHz.
 * @param  hcsr     Puntero al handle del sensor.
 * @param  tick_hz  Frecuencia del contador en Hz (f_timer_clk / (PSC + 1)).
 * @note   Llamar después de @ref HCSR04_Init(). Valores nulos se ignoran.
 */
void HCSR04_SetTickFrequency(HCSR04_Handle* hcsr, uint32_t tick_hz);

/**
 * @brief  Inicia una medición: arma la captura y emite el pulso TRIG de 10 us.
 * @param  hcsr  Puntero al handle del sensor.
 * @note   No bloqueante salvo por los ~10 us del pulso TRIG. Si había una
 *         medición previa sin terminar, se cancela y se reinicia.
 * @note   Respetar un intervalo mínimo de ~60 ms entre disparos de un mismo sensor.
 */
void HCSR04_Trigger(HCSR04_Handle* hcsr);

/**
 * @brief  Procesa un evento de captura del timer (flanco de ECHO).
 * @param  hcsr  Puntero al handle del sensor al que pertenece el evento.
 * @attention Debe llamarse desde HAL_TIM_IC_CaptureCallback() (contexto de ISR),
 *            enrutando por `htim->Instance` y `htim->Channel` hacia el sensor
 *            correcto. Eventos recibidos fuera de una medición se ignoran.
 */
void HCSR04_InputCapture_Callback(HCSR04_Handle* hcsr);

/**
 * @brief  Obtiene la distancia medida en centímetros.
 * @param  hcsr  Puntero al handle del sensor.
 * @retval >= 0.0f                 Distancia en cm (la medición se consume y el
 *                                 sensor vuelve a IDLE).
 * @retval HCSR04_ERR_NOT_READY    Medición aún en curso o sin disparo previo.
 * @retval HCSR04_ERR_TIMEOUT      No hubo un eco válido: o no llegó eco en
 *                                 @ref HCSR04_TIMEOUT_MS, o la distancia quedó
 *                                 fuera de [@ref HCSR04_MIN_CM, @ref HCSR04_MAX_CM].
 *                                 La muestra se descarta y el sensor queda en
 *                                 IDLE, listo para dispararse de nuevo.
 * @note   El valor devuelto es la mediana de las últimas @ref HCSR04_FILTER_SIZE
 *         muestras válidas. Mientras el historial no se llena (las primeras
 *         mediciones tras Init o tras @ref HCSR04_ClearFilter) se devuelve la
 *         lectura sin filtrar.
 * @note   Si se acumulan @ref HCSR04_FILTER_SIZE lecturas inválidas seguidas,
 *         el historial se borra (se asume que el objeto ya no está o cambió).
 */
float HCSR04_ReadDistance_cm(HCSR04_Handle* hcsr);

/**
 * @brief  Borra el historial del filtro de mediana.
 * @param  hcsr  Puntero al handle del sensor.
 * @note   Útil tras un cambio brusco esperado de distancia. @ref HCSR04_Init()
 *         ya lo invoca.
 */
void HCSR04_ClearFilter(HCSR04_Handle* hcsr);

#ifdef __cplusplus
}
#endif

#endif /* HCSR04_STM32_H */

/**
 * @file rgb_STM32.h
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Driver modular y encapsulado para el control de LEDs RGB mediante PWM.
 * @details Este driver permite gestionar LEDs RGB (Ánodo o Cátodo Común) de forma
 *          no bloqueante. Utiliza estructuras de configuración para facilitar la
 *          inicialización y soporta efectos de color avanzados como HSV y Rainbow Fade.
 *
 *          Convención de brillo:
 *           - Los valores "lógicos" (r, g, b, current_*) van de 0 a MAX_PWM_VALUE.
 *           - Sobre ellos se aplica la corrección Gamma.
 *           - max_brightness es un TOPE sobre el duty final (post-Gamma), 0-1000.
 *
 * @note Configuración del timer: Period (ARR) = MAX_PWM_VALUE - 1 = 999.
 *       Así el período es de 1000 cuentas y CCR = 1000 (> ARR) da 100% real de duty,
 *       lo que garantiza apagado total en ánodo común. Con ARR = 1000 el período
 *       sería de 1001 cuentas y quedaría un pulso residual de 1 cuenta.
 *
 * @note Compatibilidad: funciona en MCUs con y sin FPU (ej. Blue Pill F103).
 *       Con RGB_USE_GAMMA_LUT = 1 (por defecto) el camino de efectos no usa float.
 * @version 1.2
 * @date 2026
 */

#ifndef RGB_STM32_H_
#define RGB_STM32_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>
#include <math.h>

/**
 * @defgroup RGB_Config_Constants Constantes de Configuración
 * @{
 */
#define MAX_PWM_VALUE       1000   /**< Resolución lógica (el ARR del timer debe ser MAX_PWM_VALUE - 1) */
#define GAMMA_CORRECTION    2.2f   /**< Coeficiente de gamma (usado solo si RGB_USE_GAMMA_LUT = 0) */

#ifndef RGB_USE_GAMMA_LUT
#define RGB_USE_GAMMA_LUT   1      /**< 1: tabla en Flash (sin FPU); 0: powf() (requiere FPU) */
#endif
/** @} */

/**
 * @enum RGB_LED_Type_t
 * @brief Define la arquitectura de conexión del LED RGB.
 */
typedef enum {
    LED_TYPE_CATHODE_COMMON, /**< Cátodo a GND: Duty Cycle alto = Más brillo */
    LED_TYPE_ANODE_COMMON    /**< Ánodo a VCC: Duty Cycle bajo = Más brillo (Inverso) */
} RGB_LED_Type_t;

/**
 * @enum RGB_Effect_t
 * @brief Lista de efectos disponibles para el motor de efectos.
 */
typedef enum {
    EFFECT_NONE,          /**< Color estático, sin procesamiento de efectos */
    EFFECT_FADE_RAINBOW,  /**< Transición automática por todo el círculo cromático */
} RGB_Effect_t;

/**
 * @enum RGB_FadeState_t
 * @brief Estados internos de la máquina de estados para el efecto Rainbow.
 */
typedef enum {
    FADE_G_IN_RED_TO_YELLOW,   /**< Incrementando Verde sobre Rojo */
    FADE_R_OUT_YELLOW_TO_GREEN,/**< Decrementando Rojo sobre Amarillo */
    FADE_B_IN_GREEN_TO_CYAN,   /**< Incrementando Azul sobre Verde */
    FADE_G_OUT_CYAN_TO_BLUE,   /**< Decrementando Verde sobre Cian */
    FADE_R_IN_BLUE_TO_MAGENTA, /**< Incrementando Rojo sobre Azul */
    FADE_B_OUT_MAGENTA_TO_RED  /**< Decrementando Azul sobre Magenta */
} RGB_FadeState_t;

/**
 * @enum RGB_PresetColor_t
 * @brief Colores preestablecidos para acceso rápido.
 */
typedef enum {
    COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_YELLOW,
    COLOR_CYAN, COLOR_MAGENTA, COLOR_WHITE, COLOR_OFF
} RGB_PresetColor_t;

/**
 * @struct RGB_Config_t
 * @brief Paquete de configuración inicial (Encapsulamiento de Hardware).
 * @details Esta estructura se utiliza únicamente durante la inicialización para
 *          pasar todos los parámetros de hardware de una sola vez.
 */
typedef struct {
    TIM_HandleTypeDef *htim;  /**< Handle del Timer configurado en CubeMX */
    uint32_t R_channel;       /**< Canal asignado al Rojo (ej. TIM_CHANNEL_1) */
    uint32_t G_channel;       /**< Canal asignado al Verde (ej. TIM_CHANNEL_2) */
    uint32_t B_channel;       /**< Canal asignado al Azul (ej. TIM_CHANNEL_3) */
    RGB_LED_Type_t led_type;  /**< Polaridad del LED (Ánodo o Cátodo común) */
    uint16_t max_brightness;  /**< Tope del duty final, de 0 a 1000 */
} RGB_Config_t;

/**
 * @struct RGB_LED_t
 * @brief Objeto de estado del LED RGB.
 * @details Contiene tanto los parámetros de hardware como las variables de estado
 *          necesarias para la ejecución de efectos en tiempo real.
 */
typedef struct {
    TIM_HandleTypeDef *htim;    /**< Handle del Timer de la HAL */
    uint32_t R_channel;         /**< Canal del canal Rojo */
    uint32_t G_channel;         /**< Canal del canal Verde */
    uint32_t B_channel;         /**< Canal del canal Azul */
    RGB_LED_Type_t led_type;    /**< Configuración de polaridad del hardware */
    uint16_t max_brightness;    /**< Tope del duty final (no lo modifican los efectos) */

    uint16_t current_r;         /**< Intensidad actual del Rojo (0-1000) */
    uint16_t current_g;         /**< Intensidad actual del Verde (0-1000) */
    uint16_t current_b;         /**< Intensidad actual del Azul (0-1000) */

    uint32_t last_tick_time;    /**< Registro de tiempo para control de velocidad de efectos */
    RGB_Effect_t active_effect; /**< Efecto activo actualmente */
    RGB_FadeState_t fade_state; /**< Estado actual de la máquina de estados de color */
    uint16_t effect_brightness; /**< Pico lógico del efecto Rainbow (0-1000) */
} RGB_LED_t;

/* --- API del Driver (Prototipos) --- */

/**
 * @brief Inicializa el hardware y el objeto de estado mediante un paquete de configuración.
 * @param[out] led_ptr Puntero al objeto de estado del LED.
 * @param[in]  config Puntero a la estructura con los parámetros de hardware.
 */
void RGB_Init_Single(RGB_LED_t *led_ptr, RGB_Config_t *config);

/**
 * @brief Establece un color RGB directo aplicando corrección Gamma y tope de brillo.
 * @param[in,out] led_ptr Puntero al LED.
 * @param[in]     r Componente Rojo (0-1000, se recorta si excede).
 * @param[in]     g Componente Verde (0-1000, se recorta si excede).
 * @param[in]     b Componente Azul (0-1000, se recorta si excede).
 */
void RGB_Set_Color_Direct(RGB_LED_t *led_ptr, uint16_t r, uint16_t g, uint16_t b);

/**
 * @brief Establece un color a partir de la lista de presets.
 * @param[in,out] led_ptr Puntero al LED.
 * @param[in]     color Color predefinido (ej. COLOR_MAGENTA).
 */
void RGB_Set_Preset(RGB_LED_t *led_ptr, RGB_PresetColor_t color);

/**
 * @brief Establece el color mediante el modelo HSV (Tono, Saturación, Valor).
 * @param[in,out] led_ptr Puntero al LED.
 * @param[in]     h Hue o Tono (0.0 - 360.0, se normaliza con módulo).
 * @param[in]     s Saturation o Saturación (0.0 - 1.0, se recorta).
 * @param[in]     v Value o Brillo (0.0 - 1.0, se recorta).
 */
void RGB_Set_Color_HSV(RGB_LED_t *led_ptr, float h, float s, float v);

/**
 * @brief Motor de efectos no bloqueante. Debe llamarse periódicamente en el Scheduler.
 * @param[in,out] led_ptr Puntero al LED.
 * @param[in]     fade_interval_ms Tiempo en ms entre cada paso de actualización.
 * @param[in]     fade_step Cantidad de unidades (0-1000) que cambia el color por paso (mínimo 1).
 */
void RGB_Effects_Handler(RGB_LED_t *led_ptr, uint32_t fade_interval_ms, uint16_t fade_step);

/**
 * @brief Inicia el efecto de arcoíris (Rainbow Fade).
 * @param[in,out] led_ptr Puntero al LED.
 * @param[in]     peak_brightness Pico lógico (0-1000) del efecto. No altera max_brightness.
 */
void RGB_Start_Effect_Rainbow(RGB_LED_t *led_ptr, uint16_t peak_brightness);

/**
 * @brief Detiene cualquier efecto activo y mantiene el color actual.
 * @param[in,out] led_ptr Puntero al LED.
 */
void RGB_Stop_Effect(RGB_LED_t *led_ptr);

#ifdef __cplusplus
}
#endif

#endif /* RGB_STM32_H_ */

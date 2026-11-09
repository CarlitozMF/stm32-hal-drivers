/**
 * @file encoder_ky040_STM32.h
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Driver genérico por software para encoder rotativo KY040.
 * @details Utiliza interrupciones externas (EXTI) en ambos flancos de CLK y DT
 *          y un decodificador de cuadratura por tabla de transiciones, que rechaza los
 *          rebotes mecánicos sin depender de filtrado temporal. El pulsador usa
 *          antirrebote por tiempo.
 * @version 1.3
 * @date 2026
 *
 * @note Configuración EXTI requerida:
 *       - CLK (pin_A) y DT (pin_B): flanco de subida y bajada.
 *       - SW: flanco de bajada (suficiente).
 *       - En HAL_GPIO_EXTI_Callback() llamar a KY040_IRQ_Handler() con el pin recibido.
 */

#ifndef ENCODER_KY040_STM32_H
#define ENCODER_KY040_STM32_H

#include "main.h"

/**
 * @struct KY040_t
 * @brief Estructura de control para el encoder rotativo KY-040.
 */
typedef struct {
    /* --- Configuración de Hardware (Pines) --- */
    GPIO_TypeDef* port_A;         /**< Puerto del pin CLK (Canal A) */
    uint16_t pin_A;               /**< Pin del Canal A (CLK) */
    GPIO_TypeDef* port_B;         /**< Puerto del pin DT (Canal B) */
    uint16_t pin_B;               /**< Pin del Canal B (DT) */
    GPIO_TypeDef* port_SW;        /**< Puerto del pin del pulsador (SW) */
    uint16_t pin_SW;              /**< Pin del pulsador (SW) */

    /* --- Estados de Control y Posición --- */
    volatile int32_t position;    /**< Posición actual (escrita en ISR) */
    int32_t min_val;              /**< Límite inferior del contador de posición */
    int32_t max_val;              /**< Límite superior del contador de posición */
    uint8_t state;                /**< Estado previo de cuadratura: (A << 1) | B */
    int8_t  accum;                /**< Pasos de cuadratura acumulados hacia el próximo detent */
    volatile uint8_t sw_pressed;  /**< Flag de pulsador presionado (escrito en ISR) */

    /* --- Temporización para Anti-rebote del pulsador --- */
    uint32_t last_tick_SW;        /**< Instante del último evento válido del botón */

} KY040_t;

/* --- Funciones Públicas --- */

/**
 * @brief Inicializa la estructura de control del encoder y establece el punto inicial.
 * @details Si min > max se intercambian automáticamente. La posición inicial se fija en el centro del rango.
 * @param[out] dev Puntero a la instancia del encoder.
 * @param[in]  pA Puerto del pin CLK.
 * @param[in]  pinA Pin del canal CLK.
 * @param[in]  pB Puerto del pin DT.
 * @param[in]  pinB Pin del canal DT.
 * @param[in]  pSW Puerto del pin del pulsador.
 * @param[in]  pinSW Pin del pulsador.
 * @param[in]  min Límite mínimo permitido.
 * @param[in]  max Límite máximo permitido.
 */
void KY040_Init(KY040_t *dev, GPIO_TypeDef* pA, uint16_t pinA,
                GPIO_TypeDef* pB, uint16_t pinB, GPIO_TypeDef* pSW, uint16_t pinSW,
                int32_t min, int32_t max);

/**
 * @brief Manejador unificado de interrupciones para el Encoder.
 * @details Procesa el giro (CLK o DT) y el botón (SW) según el pin que disparó la EXTI.
 * @param[in,out] dev Puntero a la instancia del encoder.
 * @param[in]     triggered_pin Pin GPIO que disparó la interrupción externa EXTI.
 */
void KY040_IRQ_Handler(KY040_t *dev, uint16_t triggered_pin);

/**
 * @brief Lee la posición actual de forma segura mediante sección crítica.
 * @param[in] dev Puntero a la instancia del encoder.
 * @return Posición actual, garantizada dentro del rango [min_val, max_val].
 */
int32_t KY040_GetPosition(KY040_t *dev);

/**
 * @brief Establece una nueva posición para el encoder respetando los límites.
 * @param[in,out] dev Puntero a la instancia del encoder.
 * @param[in]     new_position Nuevo valor de posición deseado (se satura al rango).
 */
void KY040_SetPosition(KY040_t *dev, int32_t new_position);

/**
 * @brief Consulta y limpia de forma atómica el estado del pulsador.
 * @param[in,out] dev Puntero a la instancia del encoder.
 * @return 1 si el botón fue presionado (y limpia el flag), 0 en caso contrario.
 */
uint8_t KY040_IsButtonPressed(KY040_t *dev);

#endif /* ENCODER_KY040_STM32_H */

/**
 * @file utils.h
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Funciones de utilidad y retardos en microsegundos para cualquier STM32.
 * @details Usa el contador DWT->CYCCNT en Cortex-M3/M4/M7/M33 y, si no está
 *          disponible (Cortex-M0/M0+ u otro caso), recurre a SysTick.
 *          La selección es automática, sin modificar código fuente.
 * @version 2.1
 * @date 2026
 */

#ifndef UTILS_H_
#define UTILS_H_

#include "main.h"

/**
 * @brief Inicializa el temporizador de retardos en microsegundos.
 * @note  Es idempotente: se puede llamar varias veces y no reinicia
 *        el contador DWT si ya estaba corriendo.
 */
void Utils_DelayUs_Init(void);

/**
 * @brief Genera un retardo bloqueante en microsegundos.
 * @note  El retardo nunca es menor al solicitado, pero una interrupción
 *        durante la espera puede alargarlo.
 * @param us Cantidad de microsegundos a esperar.
 */
void Utils_DelayUs(uint32_t us);

#endif /* UTILS_H_ */

/**
 * @file uart_STM32.h
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Cabecera del driver UART genérico 100% no bloqueante (RX/TX por Ring Buffers e Interrupciones).
 * @version 1.3
 * @date 2026
 */

#ifndef UART_STM32_H
#define UART_STM32_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/**
 * @brief Inicializa el driver UART, los buffers circulares y engancha la instancia HAL.
 * @param huart Puntero a la estructura de control UART de la HAL.
 */
void UART_Driver_Init(UART_HandleTypeDef *huart);

/**
 * @brief Envía un único carácter de forma no bloqueante (lo encola en TX).
 * @param c Carácter a transmitir.
 */
void UART_Driver_SendChar(char c);

/**
 * @brief Envía una cadena de caracteres terminada en nulo de forma no bloqueante.
 * @param str Puntero al string a transmitir.
 */
void UART_Driver_SendString(const char *str);

/**
 * @brief Inicia la escucha asíncrona por interrupción para recepción.
 */
void UART_Driver_StartReceive_IT(void);

/**
 * @brief Verifica si hay datos disponibles en el buffer RX.
 */
uint8_t UART_Driver_DataAvailable(void);

/**
 * @brief Lee el siguiente byte del buffer RX (FIFO).
 */
uint8_t UART_Driver_ReadChar(uint8_t *pData);

/**
 * @brief Manejador interno de recepción por interrupción.
 */
void UART_Driver_RxHandler(void);

/**
 * @brief Manejador interno de transmisión completada por interrupción.
 */
void UART_Driver_TxHandler(void);

/**
 * @brief Manejador interno de errores UART.
 */
void UART_Driver_ErrorHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_STM32_H */

/**
 * @file dht11_STM32.h
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Driver genérico e independiente para el sensor de temperatura y humedad DHT11 en STM32.
 * @details Implementa la interfaz pública orientada a manejadores con protección contra bloqueos (timeouts) y retardos internos por SysTick.
 * @version 2.1
 * @date 2026
 */

#ifndef DHT11_STM32_H
#define DHT11_STM32_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Códigos de estado del driver DHT11.
 */
typedef enum {
    DHT11_OK = 0,               /**< Operación exitosa. */
    DHT11_ERR_INVALID_PARAM,    /**< Puntero o parámetro inválido. */
    DHT11_ERR_TIMEOUT,          /**< Tiempo de espera agotado (sensor desconectado o sin respuesta). */
    DHT11_ERR_CHECKSUM          /**< Error en la validación de los datos (Checksum incorrecto). */
} DHT11_Status_t;

/**
 * @brief Estructura para almacenar los datos leídos del sensor.
 */
typedef struct {
    float temperature;          /**< Temperatura en grados Celsius. */
    float humidity;             /**< Humedad relativa en porcentaje. */
} DHT11_Data_t;

/**
 * @brief Estructura de control (Handle) para el sensor DHT11.
 */
typedef struct {
    GPIO_TypeDef *gpio_port;    /**< Puerto GPIO del pin de datos (ej: GPIOA, GPIOB). */
    uint16_t gpio_pin;          /**< Pin de datos (ej: GPIO_PIN_4). */
    DHT11_Data_t last_data;     /**< Última lectura exitosa almacenada. */
} DHT11_Handle_t;

/**
 * @brief Inicializa el driver del DHT11 configurando el puerto, pin y estado inicial.
 * @param dev Puntero al manejador del sensor `DHT11_Handle_t`.
 * @param gpio_port Puerto GPIO donde está conectado el pin de datos.
 * @param gpio_pin Pin GPIO específico de datos.
 * @retval DHT11_Status_t Estado de la inicialización (`DHT11_OK` o `DHT11_ERR_INVALID_PARAM`).
 */
DHT11_Status_t DHT11_Init(DHT11_Handle_t *dev, GPIO_TypeDef *gpio_port, uint16_t gpio_pin);

/**
 * @brief Lee los datos completos del sensor (temperatura, humedad y checksum).
 * @details Ejecuta el protocolo de un solo hilo con protección contra bloqueos mediante timeouts.
 * @param dev Puntero al manejador del sensor `DHT11_Handle_t`.
 * @retval DHT11_Status_t `DHT11_OK` si la lectura y el checksum son correctos, o código de error respectivo.
 */
DHT11_Status_t DHT11_ReadData(DHT11_Handle_t *dev);

/**
 * @brief Obtiene la última lectura exitosa almacenada en el manejador.
 * @param dev Puntero al manejador del sensor `DHT11_Handle_t`.
 * @param data Puntero a la estructura donde se copiarán los valores.
 * @retval DHT11_Status_t `DHT11_OK` si se copiaron correctamente, o `DHT11_ERR_INVALID_PARAM`.
 */
DHT11_Status_t DHT11_GetLastData(DHT11_Handle_t *dev, DHT11_Data_t *data);

#ifdef __cplusplus
}
#endif

#endif /* DHT11_STM32_H */

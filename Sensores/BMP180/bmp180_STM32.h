/**
 * @file bmp180_STM32.h
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Interfaz del driver NO BLOQUEANTE para el sensor de presión barométrica y temperatura BMP180/BMP085.
 * @details Define la máquina de estados cooperativa basada en HAL_GetTick(), eliminando el uso de
 *          retardos bloqueantes (HAL_Delay) para garantizar la compatibilidad con sistemas en tiempo real (ej. drones).
 *
 *          Flujo de uso recomendado:
 *          1. BMP180_Init()             -> Configura el handle, valida el Chip ID y lee los coeficientes de calibración.
 *          2. BMP180_Process()          -> Llamar permanentemente en cada iteración del bucle principal.
 *          3. BMP180_StartMeasurement() -> Inicia un ciclo de lectura completo (T + P) de forma asíncrona.
 *          4. BMP180_GetData()          -> Obtiene los valores cuando BMP180_Process() retorna BMP180_DATA_READY.
 *
 * @warning El driver NO es reentrante: todas sus funciones deben llamarse desde el mismo contexto
 *          (típicamente el bucle principal), nunca mezclando main e ISR.
 * @version 2.0
 * @date 2026
 */

#ifndef BMP180_STM32_H_
#define BMP180_STM32_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

/** @brief Dirección I2C de 7 bits desplazada para la HAL de ST (0x77 << 1) */
#define BMP180_I2C_ADDRESS      (0x77 << 1)

/**
 * @brief Códigos de estado del driver del BMP180.
 */
typedef enum {
    BMP180_OK = 0,         /**< Sin novedades (driver inactivo y operando correctamente) */
    BMP180_ERROR_I2C,      /**< Fallo de comunicación física en el bus I2C */
    BMP180_ERROR_CHIPID,   /**< Error: El ID del chip leído no coincide con 0x55 */
    BMP180_ERROR_TIMEOUT,  /**< Tiempo de espera agotado / NACK durante la transacción I2C */
    BMP180_BUSY,           /**< Operación en curso (máquina de estados ocupada) */
    BMP180_DATA_READY      /**< Ciclo de medición finalizado: nuevos datos válidos disponibles */
} BMP180_Status_t;

/**
 * @brief Modos de Sobremuestreo (OSS - Oversampling Setting) para la presión.
 */
typedef enum {
    BMP180_MODE_ULTRALOWPOWER = 0, /**< 1 muestra, conversión máx ~4.5ms */
    BMP180_MODE_STANDARD      = 1, /**< 2 muestras, conversión máx ~7.5ms */
    BMP180_MODE_HIGHRES       = 2, /**< 3 muestras, conversión máx ~13.5ms */
    BMP180_MODE_ULTRAHIGHRES  = 3  /**< 4 muestras, conversión máx ~25.5ms */
} bmp180_mode_t;

/**
 * @brief Estructura para almacenar los 11 coeficientes de calibración de fábrica de la ROM del sensor.
 */
typedef struct {
    int16_t  ac1;
    int16_t  ac2;
    int16_t  ac3;
    uint16_t ac4;
    uint16_t ac5;
    uint16_t ac6;
    int16_t  b1;
    int16_t  b2;
    int16_t  mb;
    int16_t  mc;
    int16_t  md;
} bmp180_calib_data_t;

/**
 * @brief Estructura principal del manejador del BMP180.
 */
typedef struct {
    I2C_HandleTypeDef   *hi2c;   /**< Puntero al periférico I2C de la HAL */
    bmp180_calib_data_t calib;   /**< Coeficientes de calibración internos */
    bmp180_mode_t       mode;    /**< Modo de sobremuestreo configurado */
} BMP180_Handle_t;

/**
 * @brief Inicializa el sensor BMP180, valida el Chip ID y lee los coeficientes de calibración.
 * @param[in,out] dev Puntero a la estructura del manejador del BMP180.
 * @param[in]     hi2c Puntero al manejador I2C de la HAL (ej: &hi2c1), ya inicializado.
 * @param[in]     mode Modo de sobremuestreo deseado para la presión.
 * @return BMP180_Status_t BMP180_OK si tuvo éxito, o código de error correspondiente.
 */
BMP180_Status_t BMP180_Init(BMP180_Handle_t *dev, I2C_HandleTypeDef *hi2c, bmp180_mode_t mode);

/**
 * @brief Avanza la máquina de estados cooperativa del driver. Debe llamarse de forma continua en el bucle principal.
 * @details Gestiona los tiempos de espera de conversión de temperatura y presión mediante HAL_GetTick()
 *          sin bloquear la ejecución del microcontrolador.
 * @param[in,out] dev Puntero a la estructura del manejador del BMP180.
 * @return BMP180_Status_t
 *         - BMP180_BUSY: El driver está procesando una conversión en segundo plano.
 *         - BMP180_DATA_READY: Un ciclo completo (T + P) finalizó con éxito (se emite una única vez por ciclo).
 *         - BMP180_OK: El driver está inactivo y a la espera de órdenes.
 *         - BMP180_ERROR_xxx: Código de error si falló la comunicación I2C.
 */
BMP180_Status_t BMP180_Process(BMP180_Handle_t *dev);

/**
 * @brief Inicia un nuevo ciclo de medición asíncrona (temperatura seguida de presión).
 * @param[in,out] dev Puntero a la estructura del manejador del BMP180.
 * @return BMP180_Status_t
 *         - BMP180_OK: El ciclo arrancó correctamente.
 *         - BMP180_BUSY: El driver no está libre (ocupado midiendo).
 *         - BMP180_ERROR_I2C: Fallo al enviar el comando inicial.
 */
BMP180_Status_t BMP180_StartMeasurement(BMP180_Handle_t *dev);

/**
 * @brief Verifica si el driver se encuentra libre para aceptar una nueva medición.
 * @param[in] dev Puntero a la estructura del manejador del BMP180.
 * @return true Si el driver está en estado IDLE, false si está ocupado.
 */
bool BMP180_IsIdle(BMP180_Handle_t *dev);

/**
 * @brief Obtiene la última medición completa, consistente y compensada.
 * @param[in]  dev Puntero a la estructura del manejador del BMP180.
 * @param[out] temperature Puntero donde se almacenará la temperatura en °C (puede ser NULL).
 * @param[out] pressure Puntero donde se almacenará la presión en Pascales (Pa) (puede ser NULL).
 * @return true Si existe al menos una medición válida cargada en los punteros.
 * @return false Si todavía no se ha completado ninguna medición.
 */
bool BMP180_GetData(BMP180_Handle_t *dev, float *temperature, float *pressure);

/**
 * @brief Calcula la altitud en metros a partir de la presión atmosférica y la presión de referencia a nivel del mar.
 * @param[in] seaLevel Presión atmosférica de referencia a nivel del mar en hPa (milibares).
 * @param[in] atmospheric Presión atmosférica medida en hPa.
 * @return Altitud estimada en metros.
 */
float BMP180_PressureToAltitude(float seaLevel, float atmospheric);

#ifdef __cplusplus
}
#endif

#endif /* BMP180_STM32_H_ */

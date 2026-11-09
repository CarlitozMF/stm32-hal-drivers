/**
 * @file bh1750_STM32.h
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Cabecera del driver genérico e independiente para el sensor de luz digital BH1750FVI.
 * @details Proporciona las definiciones de registros, modos de operación, estructura de control y prototipos
 *          de funciones para la gestión del sensor mediante I2C con soporte de autoajuste de sensibilidad (MTreg).
 * @version 1.3
 * @date 2026
 */

#ifndef BH1750_STM32_H
#define BH1750_STM32_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @defgroup BH1750_I2C_Addresses Direcciones I2C del sensor BH1750FVI
 * @{
 */
#define BH1750_I2C_ADDR_LO   0x23    /**< Dirección I2C cuando el pin ADDR está conectado a GND (LOW) */
#define BH1750_I2C_ADDR_HI   0x5C    /**< Dirección I2C cuando el pin ADDR está conectado a VCC (HIGH) */
/** @} */

/**
 * @defgroup BH1750_MTreg_Limits Límites y valor por defecto del registro de tiempo de medición
 * @{
 */
#define BH1750_DEFAULT_MTREG 69      /**< Valor predeterminado de MTreg especificado por el fabricante */
#define BH1750_MTREG_MIN     31      /**< Valor mínimo permitido para MTreg (menor sensibilidad / ambientes muy claros) */
#define BH1750_MTREG_MAX     254     /**< Valor máximo permitido para MTreg (mayor sensibilidad / ambientes muy oscuros) */
/** @} */

/**
 * @defgroup BH1750_Thresholds Umbrales para la lógica de autoajuste dinámico
 * @{
 */
#define BH1750_HIGH_LIGHT_THRESHOLD  50000.0f  /**< Límite superior de luxes: riesgo de saturación, requiere disminuir MTreg */
#define BH1750_LOW_LIGHT_THRESHOLD     10.0f   /**< Límite inferior de luxes: baja iluminación, requiere incrementar MTreg */
/** @} */

/**
 * @brief Códigos de estado y error retornados por las funciones del driver.
 */
typedef enum {
    BH1750_OK = 0,               /**< Operación completada con éxito */
    BH1750_ERR_INVALID_PARAM,    /**< Parámetro de entrada nulo o inválido */
    BH1750_ERR_I2C_TRANSMIT,     /**< Error durante la transmisión I2C mediante HAL */
    BH1750_ERR_I2C_RECEIVE,      /**< Error durante la recepción I2C mediante HAL */
    BH1750_ERR_NOT_CONFIGURED,   /**< El dispositivo se encuentra en estado no configurado */
    BH1750_ERR_OUT_OF_RANGE      /**< El valor proporcionado está fuera de los rangos válidos */
} BH1750_Status_t;

/**
 * @brief Modos de operación y resolución del sensor BH1750FVI.
 */
typedef enum {
    BH1750_UNCONFIGURED = 0,             /**< Estado inicial sin configurar */
    BH1750_CONTINUOUS_HIGH_RES_MODE   = 0x10, /**< Medición continua a alta resolución (1 Lux, ~120ms) */
    BH1750_CONTINUOUS_HIGH_RES_MODE_2 = 0x11, /**< Medición continua a alta resolución 2 (0.5 Lux, ~120ms) */
    BH1750_CONTINUOUS_LOW_RES_MODE    = 0x13, /**< Medición continua a baja resolución (4 Lux, ~16ms) */
    BH1750_ONE_TIME_HIGH_RES_MODE     = 0x20, /**< Medición única a alta resolución (1 Lux, ~120ms) */
    BH1750_ONE_TIME_HIGH_RES_MODE_2   = 0x21, /**< Medición única a alta resolución 2 (0.5 Lux, ~120ms) */
    BH1750_ONE_TIME_LOW_RES_MODE      = 0x23  /**< Medición única a baja resolución (4 Lux, ~16ms) */
} BH1750_Mode_t;

/**
 * @brief Estructura del manejador (Handle) para la instancia del sensor BH1750.
 */
typedef struct {
    void *hi2c;                      /**< Puntero al manejador del periférico I2C de la HAL (I2C_HandleTypeDef *) */
    uint8_t address;                 /**< Dirección I2C física configurada para el dispositivo */
    BH1750_Mode_t mode;              /**< Modo de operación actual del sensor */
    uint8_t mtreg;                   /**< Valor actual del registro de tiempo de medición (MTreg) */
    uint32_t last_read_timestamp;    /**< Sello de tiempo (en ms obtenido por HAL_GetTick) de la última lectura */
    float conv_factor;               /**< Factor de corrección de precisión (por defecto 1.2 según datasheet) */
} BH1750_Handle_t;

/* ========================================================================== */
/*                           PROTOTIPOS DE FUNCIONES                          */
/* ========================================================================== */

/**
 * @brief Inicializa la estructura del sensor y aplica la configuración inicial de modo y MTreg.
 * @param dev Puntero a la estructura del manejador BH1750_Handle_t.
 * @param hi2c Puntero al manejador I2C de la HAL de ST (ej. &hi2c1).
 * @param address Dirección I2C del sensor (ej. BH1750_I2C_ADDR_LO).
 * @param mode Modo de operación inicial a configurar.
 * @return BH1750_Status_t Estado de la ejecución (BH1750_OK si todo es correcto).
 */
BH1750_Status_t BH1750_Init(BH1750_Handle_t *dev, void *hi2c, uint8_t address, BH1750_Mode_t mode);

/**
 * @brief Configura el modo de medición del sensor BH1750.
 * @param dev Puntero a la estructura del manejador BH1750_Handle_t.
 * @param mode Modo de operación a establecer.
 * @return BH1750_Status_t Estado de la ejecución.
 */
BH1750_Status_t BH1750_Configure(BH1750_Handle_t *dev, BH1750_Mode_t mode);

/**
 * @brief Configura el registro de tiempo de medición (MTreg) para modificar la ganancia/sensibilidad.
 * @param dev Puntero a la estructura del manejador BH1750_Handle_t.
 * @param mtreg Nuevo valor para el registro (debe estar entre BH1750_MTREG_MIN y BH1750_MTREG_MAX).
 * @return BH1750_Status_t Estado de la ejecución.
 */
BH1750_Status_t BH1750_SetMTreg(BH1750_Handle_t *dev, uint8_t mtreg);

/**
 * @brief Verifica si ha transcurrido el tiempo necesario para recolectar una nueva muestra.
 * @param dev Puntero a la estructura del manejador BH1750_Handle_t.
 * @param max_wait Booleano para elegir entre el tiempo de espera típico (false) o máximo (true).
 * @return true si la medición está lista para ser leída, false en caso contrario.
 */
bool BH1750_IsMeasurementReady(BH1750_Handle_t *dev, bool max_wait);

/**
 * @brief Lee los datos del sensor y calcula el nivel de iluminación actual en Lux.
 * @param dev Puntero a la estructura del manejador BH1750_Handle_t.
 * @param lux_level Puntero a float donde se almacenará el resultado convertido.
 * @return BH1750_Status_t Estado de la ejecución.
 */
BH1750_Status_t BH1750_ReadLightLevel(BH1750_Handle_t *dev, float *lux_level);

/**
 * @brief Función de autoajuste del MTreg basada en el nivel de luz leído en tiempo de ejecución.
 * @details Evalúa si la intensidad lumínica excede los umbrales seguros para ajustar dinámicamente
 *          la sensibilidad del sensor y evitar saturación o pérdida de resolución.
 * @param dev Puntero a la estructura del manejador BH1750_Handle_t.
 * @param current_lux Último valor de lux medido.
 * @return true si se modificó y aplicó un nuevo MTreg, false si se mantuvo el valor actual.
 */
bool BH1750_AutoAdjust(BH1750_Handle_t *dev, float current_lux);

#ifdef __cplusplus
}
#endif

#endif /* BH1750_STM32_H */

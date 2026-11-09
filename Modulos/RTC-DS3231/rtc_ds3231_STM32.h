/**
 * @file rtc_ds3231_STM32.h
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Cabecera para el driver del RTC DS3231 con soporte para verificación de
 *        pérdida de oscilador (OSF) y Alarma 1.
 * @version 1.2
 * @date 2026
 */

#ifndef RTC_DS3231_STM32_H_
#define RTC_DS3231_STM32_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @defgroup DS3231_Driver DS3231 Driver
 * @brief Driver modular para el módulo RTC DS3231 con STM32 HAL.
 * @{
 */

/** @brief Timeout (ms) para las transacciones I2C. */
#define DS3231_I2C_TIMEOUT_MS  100U

/**
 * @brief Códigos de estado y retorno del driver DS3231.
 */
typedef enum {
    DS3231_OK = 0,             /**< Operación exitosa */
    DS3231_ERROR,              /**< Error general de comunicación o ejecución */
    DS3231_ERR_INVALID_PARAM   /**< Puntero nulo, driver sin inicializar o parámetro fuera de rango */
} DS3231_Status;

/**
 * @brief Estructura para almacenar la hora y fecha (formato 24 h).
 * @note El año se expresa en el rango 0-99 (2000-2099). El bit de siglo no se gestiona.
 */
typedef struct {
    uint8_t seconds;       /**< 0-59 */
    uint8_t minutes;       /**< 0-59 */
    uint8_t hours;         /**< 0-23 */
    uint8_t day_of_week;   /**< 1-7 (convención del usuario, p. ej. 1 = Domingo) */
    uint8_t day_of_month;  /**< 1-31 */
    uint8_t month;         /**< 1-12 */
    uint8_t year;          /**< 0-99 */
} DS3231_Time;

/**
 * @brief Modos de coincidencia para la Alarma 1.
 */
typedef enum {
    DS3231_A1_EVERY_SECOND = 0,     /**< Dispara cada segundo */
    DS3231_A1_MATCH_S,              /**< Cuando coinciden los segundos */
    DS3231_A1_MATCH_MS,             /**< Cuando coinciden minutos y segundos */
    DS3231_A1_MATCH_HMS,            /**< Cuando coinciden horas, minutos y segundos */
    DS3231_A1_MATCH_DHMS            /**< Cuando coincide fecha, horas, minutos y segundos */
} DS3231_Alarm1_Mode;

/**
 * @brief Inicializa el driver del DS3231 asociando el bus I2C.
 * @param hi2c Puntero al handler I2C de la HAL.
 * @return DS3231_Status Estado de la inicialización.
 */
DS3231_Status DS3231_Init(I2C_HandleTypeDef *hi2c);

/**
 * @brief Escribe la hora y fecha en el módulo DS3231 y limpia la bandera OSF.
 * @param time Puntero a la estructura de tiempo a escribir (se valida el rango de cada campo).
 * @return DS3231_Status Estado de la operación.
 */
DS3231_Status DS3231_WriteTime(const DS3231_Time *time);

/**
 * @brief Lee la hora y fecha actual del módulo DS3231 (siempre devuelve formato 24 h).
 * @param time Puntero donde se almacenarán los datos leídos.
 * @return DS3231_Status Estado de la lectura.
 */
DS3231_Status DS3231_ReadTime(DS3231_Time *time);

/**
 * @brief Lee la temperatura interna compensada del módulo DS3231.
 * @note El RTC actualiza la medición cada 64 s. Resolución: 0.25 °C.
 * @param temp Puntero donde se guardará la temperatura en grados Celsius.
 * @return DS3231_Status Estado de la lectura.
 */
DS3231_Status DS3231_ReadTemperature(float *temp);

/**
 * @brief Verifica si el oscilador se detuvo (bandera OSF, bit 7 del registro de estado 0x0F).
 * @details OSF = 1 indica que el oscilador estuvo detenido (pérdida de Vcc y batería, etc.)
 *          y que la hora puede no ser confiable. Se limpia al llamar a DS3231_WriteTime().
 * @param is_halted Puntero a booleano donde se guardará el estado (true si se detuvo).
 * @return DS3231_Status Estado de la verificación.
 */
DS3231_Status DS3231_CheckClockHalted(bool *is_halted);

/**
 * @brief Configura y habilita la Alarma 1 (la bandera A1F se limpia antes de habilitar la interrupción).
 * @param alarm_time Puntero al tiempo de alarma deseado (solo se validan los campos usados por el modo).
 * @param mode Modo de coincidencia de la alarma (@ref DS3231_Alarm1_Mode).
 * @return DS3231_Status Estado de la configuración.
 */
DS3231_Status DS3231_SetAlarm1(const DS3231_Time *alarm_time, DS3231_Alarm1_Mode mode);

/**
 * @brief Limpia la bandera de disparo de la Alarma 1 (A1F).
 * @return DS3231_Status Estado de la operación.
 */
DS3231_Status DS3231_ClearAlarm1Flag(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* RTC_DS3231_STM32_H_ */

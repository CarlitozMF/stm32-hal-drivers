/**
 * @file spi_sdcard.h
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Interfaz del driver de MicroSD via SPI (STM32 HAL).
 * @version 1.2
 * @date 2026
 */

#ifndef SPI_SDCARD_STM32_H
#define SPI_SDCARD_STM32_H

#include "main.h"

/**
 * @brief Códigos de estado y retorno del driver de tarjeta SD.
 */
typedef enum {
    SD_OK = 0,               /**< Operación exitosa */
    SD_ERROR_COMMAND,        /**< Error en la ejecución o respuesta de un comando SPI */
    SD_ERROR_INIT,           /**< Fallo crítico durante la secuencia de inicialización */
    SD_ERROR_TIMEOUT         /**< Tiempo de espera agotado sin respuesta de la tarjeta */
} SD_Status_t;

/**
 * @brief Inicializa la tarjeta SD. Fuerza el prescaler lento durante el init y
 *        cambia al prescaler rápido (SD_RUN_PRESCALER) al terminar con éxito.
 * @param hspi Manejador del periférico SPI de la HAL (ej: &hspi3).
 * @param cs_port Puerto GPIO del pin Chip Select.
 * @param cs_pin Pin GPIO del Chip Select (ej: SD_CS_Pin).
 * @return SD_Status_t Estado de la inicialización.
 */
SD_Status_t SPI_SD_Init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin);

/**
 * @brief Lee un sector lógico de 512 bytes (LBA) desde la tarjeta.
 * @param pBuffer Puntero al búfer de destino en RAM donde se guardarán los datos.
 * @param sector Número de bloque o sector lógico a leer.
 * @return SD_Status_t Estado de la lectura.
 */
SD_Status_t SPI_SD_ReadBlock(uint8_t *pBuffer, uint32_t sector);

/**
 * @brief Escribe un sector lógico de 512 bytes (LBA) hacia la tarjeta.
 * @param pBuffer Puntero al búfer de origen en RAM con los datos a grabar.
 * @param sector Número de bloque o sector lógico a escribir.
 * @return SD_Status_t Estado de la escritura.
 */
SD_Status_t SPI_SD_WriteBlock(const uint8_t *pBuffer, uint32_t sector);

/**
 * @brief Obtiene la cantidad total de sectores de 512 bytes de la tarjeta (mediante el registro CSD / CMD9).
 * @param sectors Puntero donde se almacenará el conteo total de sectores para FatFs.
 * @return SD_Status_t Estado de la consulta.
 */
SD_Status_t SPI_SD_GetSectorCount(uint32_t *sectors);

#endif /* SPI_SDCARD_STM32_H */

/**
 * @file ds18b20_STM32.h
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Driver para el sensor de temperatura DS18B20 (1-Wire) sobre HAL de STM32.
 * @details Contiene definiciones de comandos, tipos de datos, estructuras de
 *          instanciación y prototipos de funciones para el manejo de múltiples sensores
 *          DS18B20 sobre STM32 usando temporización por hardware (Timer).
 *
 *          Requisitos de configuración (CubeMX / .ioc):
 *            - Timer de propósito general (16 o 32 bits), modo "Internal Clock", sin interrupciones
 *              y exclusivo para este driver.
 *            - Prescaler (PSC) tal que el tick sea de 1 MHz: PSC = (F_timer / 1 000 000) - 1.
 *              Ej.: F_timer = 72 MHz -> PSC = 71; F_timer = 84 MHz -> PSC = 83.
 *            - Counter Period (ARR) = 65535 (0xFFFF) o mayor.
 *            - Pin de datos con Pull-Up externa (~4.7 kOhm) a VDD y reloj del GPIO habilitado.
 *            - Sensores alimentados externamente (no se soporta modo parásito).
 *            - SysTick en funcionamiento (HAL_Init ya ejecutado) y llamar a DS18B20_Init()
 *              DESPUES de MX_TIMx_Init().
 *            DS18B20_Init() NO modifica el Timer: solo verifica que la configuración sea correcta.
 *
 * @version 1.1
 * @date 2026
 */

#ifndef DS18B20_STM32_H_
#define DS18B20_STM32_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stdint.h>
#include <stddef.h>
#include "main.h"

/* Exported Constants --------------------------------------------------------*/

/** @defgroup DS18B20_STM32_ROM_Commands Comandos de Control de ROM (1-Wire)
  * @{
  */
#define DS18B20_CMD_SEARCH_ROM        0xF0  /*!< Inicia el algoritmo de búsqueda de direcciones en el bus */
#define DS18B20_CMD_READ_ROM          0x33  /*!< Lee la ROM de 64 bits (Solo si hay 1 dispositivo en el bus) */
#define DS18B20_CMD_MATCH_ROM         0x55  /*!< Selecciona un dispositivo específico por su código ROM de 64 bits */
#define DS18B20_CMD_SKIP_ROM          0xCC  /*!< Dirige el siguiente comando a TODOS los dispositivos en el bus */
/**
  * @}
  */

/** @defgroup DS18B20_STM32_Function_Commands Comandos de Funciones del DS18B20
  * @{
  */
#define DS18B20_CMD_CONVERT_T         0x44  /*!< Inicia la conversión de temperatura */
#define DS18B20_CMD_WRITE_SCRATCHPAD  0x4E  /*!< Escribe 3 bytes en la memoria Scratchpad (TH, TL, Config) */
#define DS18B20_CMD_READ_SCRATCHPAD   0xBE  /*!< Lee los 9 bytes completos del Scratchpad incluida la temperatura */
#define DS18B20_CMD_COPY_SCRATCHPAD   0x48  /*!< Copia el contenido de TH, TL y Config a la EEPROM no volátil */
/**
  * @}
  */

#define DS18B20_ROM_CODE_SIZE         8     /*!< Tamaño del código ROM en bytes (64 bits) */
#define DS18B20_CONVERSION_TIMEOUT_MS 1000  /*!< Tiempo máximo de espera de una conversión */

/* Exported Types ------------------------------------------------------------*/

/**
 * @brief Estado de retorno de las funciones del driver ds18b20_STM32.
 */
typedef enum {
    DS18B20_OK = 0,         /*!< Operación completada con éxito */
    DS18B20_ERROR,          /*!< Error general de parámetros o punteros nulos */
    DS18B20_ERR_NO_DEVICE,  /*!< No se detectó presencia de dispositivo en el bus (ACK faltante) */
    DS18B20_ERR_CRC,        /*!< El CRC8 del Scratchpad o de la ROM no coincide */
    DS18B20_ERR_TIMEOUT,    /*!< La conversión no finalizó dentro del tiempo máximo */
    DS18B20_ERR_TIMER       /*!< Timer mal configurado: ARR < 0xFFFF o tick distinto de 1 us */
} DS18B20_Status_t;

/**
 * @brief Estructura de control para la instancia del bus DS18B20 / 1-Wire.
 * @details Permite instanciar múltiples buses independientes en pines GPIO y Timers distintos.
 */
typedef struct {
    GPIO_TypeDef* GPIOx;       /*!< Puerto GPIO donde está conectado el pin de datos (DQ) */
    uint16_t GPIO_Pin;         /*!< Pin GPIO correspondiente al bus 1-Wire */
    TIM_HandleTypeDef* htim;   /*!< Handle del Timer de la HAL configurado con tick de 1 us */
} DS18B20_NameTypedef;

/* Exported Functions Prototypes ---------------------------------------------*/

/**
 * @brief Inicializa la estructura del driver, el pin (Open-Drain) y arranca el Timer asignado.
 * @details El Timer debe venir configurado desde el .ioc con tick de 1 us y ARR >= 0xFFFF.
 *          Init lo arranca y verifica la base de tiempo midiendo 2 ms de SysTick (tolerancia +-5 %).
 *          El pin se configura una única vez como salida Open-Drain: escribir '1' libera la línea
 *          (la Pull-Up externa la sube) y la lectura se hace sobre IDR sin cambiar el modo del pin.
 * @param dev Puntero a la estructura de control del DS18B20.
 * @param GPIOx Puerto GPIO asignado al pin de datos.
 * @param GPIO_Pin Pin GPIO asignado al pin de datos.
 * @param htim Puntero al handle del Timer que generará los microsegundos.
 * @return DS18B20_OK si Timer y Test de Presencia son correctos, DS18B20_ERR_TIMER si el Timer está mal configurado.
 */
DS18B20_Status_t DS18B20_Init(DS18B20_NameTypedef* dev, GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin, TIM_HandleTypeDef* htim);

/**
 * @brief Envía la orden de conversión de temperatura a TODOS los dispositivos en el bus.
 * @details Utiliza el comando SKIP ROM (0xCC) seguido de CONVERT T (0x44). La conversión
 *          tarda hasta 750 ms a resolución de 12 bits. No bloquea: usar
 *          DS18B20_IsConversionDone() o esperar el tiempo necesario.
 * @param dev Puntero a la estructura de control del DS18B20.
 * @return DS18B20_OK si se transmitió la orden con éxito.
 */
DS18B20_Status_t DS18B20_StartAllConversion(DS18B20_NameTypedef* dev);

/**
 * @brief Consulta si la conversión iniciada con DS18B20_StartAllConversion() terminó.
 * @details Los sensores responden '0' a los time slots de lectura mientras convierten y '1'
 *          al terminar. No debe haber otra transacción en el bus entre el inicio y esta consulta.
 * @param dev Puntero a la estructura de control del DS18B20.
 * @param done Puntero donde se devuelve 1 si la conversión terminó, 0 si sigue en curso.
 * @return DS18B20_OK si la consulta se realizó.
 */
DS18B20_Status_t DS18B20_IsConversionDone(DS18B20_NameTypedef* dev, uint8_t* done);

/**
 * @brief Lee la temperatura de un sensor específico identificándolo por su código ROM de 8 bytes.
 * @details Verifica el CRC8 del Scratchpad y enmascara los bits no definidos según la
 *          resolución configurada (9 a 12 bits).
 * @param dev Puntero a la estructura de control del DS18B20.
 * @param rom_code Arreglo de 8 bytes con la ROM única del dispositivo. Si es NULL, envía SKIP ROM
 *                 (válido únicamente con UN sensor en el bus).
 * @param temperature Puntero a la variable float donde se guardará la temperatura medida en °C.
 * @return DS18B20_OK si la lectura fue exitosa, DS18B20_ERR_CRC si los datos están corruptos.
 */
DS18B20_Status_t DS18B20_ReadTempByROM(DS18B20_NameTypedef* dev, const uint8_t* rom_code, float* temperature);

/**
 * @brief Lee la temperatura simplificada cuando existe UN SOLO dispositivo en el bus.
 * @details Ejecuta la conversión, espera su finalización consultando al sensor (con timeout
 *          de DS18B20_CONVERSION_TIMEOUT_MS) y lee la temperatura. Es bloqueante.
 * @param dev Puntero a la estructura de control del DS18B20.
 * @param temperature Puntero a la variable float donde se guardará la temperatura en °C.
 * @return DS18B20_OK si la lectura finalizó correctamente.
 */
DS18B20_Status_t DS18B20_ReadSingleTemp(DS18B20_NameTypedef* dev, float* temperature);

/**
 * @brief Escanea el bus 1-Wire y obtiene las direcciones ROM de todos los dispositivos conectados.
 * @details Implementa el algoritmo estándar de búsqueda binaria "Search ROM" (0xF0) y valida
 *          el CRC8 de cada ROM encontrada.
 * @param dev Puntero a la estructura de control del DS18B20.
 * @param rom_table Matriz bidimensional donde se guardarán las direcciones ROM encontradas [max_sensors][8].
 * @param max_sensors Capacidad máxima de dispositivos a almacenar en la matriz.
 * @param found_count Puntero donde la función devolverá la cantidad real de sensores hallados.
 * @return DS18B20_OK si se encontró al menos un dispositivo en el bus.
 */
DS18B20_Status_t DS18B20_ScanBus(DS18B20_NameTypedef* dev, uint8_t rom_table[][DS18B20_ROM_CODE_SIZE], uint8_t max_sensors, uint8_t* found_count);

#ifdef __cplusplus
}
#endif

#endif /* DS18B20_STM32_H_ */

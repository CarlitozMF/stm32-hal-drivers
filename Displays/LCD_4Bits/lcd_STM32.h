/**
 * @file    lcd_STM32.h
 * @author  Mamani Flores Carlos (UTN FRT)
 * @brief   Driver de Capa 2 para pantallas LCD Hitachi HD44780 en modo 4 bits (STM32 HAL).
 * @details Permite el control de displays alfanuméricos 16x2 y 20x4 mediante una
 *          estructura de abstracción de pines (puerto + pin por cada línea), por lo que
 *          RS, EN y D4-D7 pueden conectarse a cualquier GPIO del microcontrolador.
 *
 * @note    Requisitos:
 *          - @c main.h debe incluir el HAL de la familia STM32 utilizada.
 *          - @c utils.h debe proveer @c Utils_DelayUs_Init() y @c Utils_DelayUs().
 *            Si la implementación usa el contador DWT, ésta NO está disponible en
 *            Cortex-M0/M0+ (STM32F0, G0, L0): en esos casos usar un timer de hardware.
 *          - Los pines deben estar configurados como salida push-pull (por ejemplo en CubeMX).
 *          - El pin R/W del LCD debe estar conectado a GND (solo escritura).
 *
 * @warning El driver mantiene una única instancia interna: solo se puede manejar
 *          un display por aplicación.
 *
 * @version 2.1
 * @date    2026
 */

#ifndef LCD_STM32_H_
#define LCD_STM32_H_

#include <stdint.h>
#include "main.h"
#include "utils.h"

/**
 * @defgroup LCD_STM32 Driver LCD HD44780 (STM32)
 * @brief    Funciones, tipos y configuración del driver LCD.
 * @{
 */

/** @brief Cantidad máxima de decimales soportados por LCD_PrintFloat(). */
#define LCD_FLOAT_MAX_DECIMALS  9U

/**
 * @brief Enumeración para definir la geometría del display LCD.
 */
typedef enum {
    LCD_16X2, ///< Display de 16 columnas por 2 filas
    LCD_20X4  ///< Display de 20 columnas por 4 filas
} lcd_type_t;

/**
 * @brief Enumeración para los modos de visualización del cursor.
 * @note  Todos los modos mantienen el display encendido.
 */
typedef enum {
    LCD_CURSOR_OFF   = 0x0C, ///< Cursor oculto, display encendido
    LCD_CURSOR_ON    = 0x0E, ///< Cursor fijo visible (subrayado)
    LCD_CURSOR_BLINK = 0x0F  ///< Cursor parpadeante (bloque)
} lcd_cursor_mode_t;

/**
 * @brief Enumeración para las direcciones de desplazamiento de la pantalla.
 */
typedef enum {
    LCD_SHIFT_LEFT  = 0x18, ///< Desplazar contenido completo hacia la izquierda
    LCD_SHIFT_RIGHT = 0x1C  ///< Desplazar contenido completo hacia la derecha
} lcd_shift_dir_t;

/**
 * @brief Estructura de configuración y abstracción de hardware del LCD para STM32.
 * @details Mapea los puertos GPIO y pines individuales de control (RS, EN) y datos (D4-D7),
 *          permitiendo conectar el driver a cualquier pin del microcontrolador.
 *
 * @code
 * LCD_Config_t cfg = {
 *     .port_rs = LCD_RS_GPIO_Port, .pin_rs = LCD_RS_Pin,
 *     .port_en = LCD_EN_GPIO_Port, .pin_en = LCD_EN_Pin,
 *     .port_d4 = LCD_D4_GPIO_Port, .pin_d4 = LCD_D4_Pin,
 *     .port_d5 = LCD_D5_GPIO_Port, .pin_d5 = LCD_D5_Pin,
 *     .port_d6 = LCD_D6_GPIO_Port, .pin_d6 = LCD_D6_Pin,
 *     .port_d7 = LCD_D7_GPIO_Port, .pin_d7 = LCD_D7_Pin,
 *     .type    = LCD_16X2
 * };
 * LCD_Init(&cfg);
 * @endcode
 */
typedef struct {
    GPIO_TypeDef* port_rs; uint16_t pin_rs; ///< Puerto y Pin para Register Select (RS)
    GPIO_TypeDef* port_en; uint16_t pin_en; ///< Puerto y Pin para Enable (EN)
    GPIO_TypeDef* port_d4; uint16_t pin_d4; ///< Puerto y Pin para Data Bit 4 (D4)
    GPIO_TypeDef* port_d5; uint16_t pin_d5; ///< Puerto y Pin para Data Bit 5 (D5)
    GPIO_TypeDef* port_d6; uint16_t pin_d6; ///< Puerto y Pin para Data Bit 6 (D6)
    GPIO_TypeDef* port_d7; uint16_t pin_d7; ///< Puerto y Pin para Data Bit 7 (D7)
    lcd_type_t    type;                     ///< Tipo de display (LCD_16X2 o LCD_20X4)
} LCD_Config_t;

/**
 * @brief Inicializa el hardware y la interfaz de 4 bits del LCD.
 * @details Ejecuta la secuencia de reseteo por software del HD44780, configura
 *          2 líneas / matriz 5x8, incremento automático de dirección (Entry Mode),
 *          display encendido y cursor oculto. Debe llamarse una sola vez, antes de
 *          cualquier otra función del driver.
 * @param[in] config Puntero a la estructura de configuración de pines @ref LCD_Config_t.
 *                   Si es NULL, la función retorna sin hacer nada.
 */
void LCD_Init(const LCD_Config_t *config);

/**
 * @brief Borra todo el contenido del display y retorna el cursor a la posición (0,0).
 * @note  Bloqueante: espera ~2 ms (tiempo de ejecución del comando).
 */
void LCD_Clear(void);

/**
 * @brief Posiciona el cursor en una fila y columna específica.
 * @details Los valores fuera de rango se saturan al máximo de la geometría configurada.
 * @param[in] row Índice de la fila (0 a 1 para 16x2; 0 a 3 para 20x4).
 * @param[in] col Índice de la columna (0 a 15 o 0 a 19 según la geometría).
 */
void LCD_SetCursor(uint8_t row, uint8_t col);

/**
 * @brief Imprime una cadena de caracteres en la posición actual del cursor.
 * @note  No hace salto de línea automático. No permite imprimir el carácter
 *        CGRAM 0 (equivale a '\0'); para ello usar LCD_WriteChar().
 * @param[in] str Puntero al string terminado en nulo. Si es NULL no hace nada.
 */
void LCD_Print(const char *str);

/**
 * @brief Escribe un único carácter en la posición actual del cursor.
 * @param[in] data Carácter ASCII a enviar, o índice 0-7 de un carácter personalizado.
 */
void LCD_WriteChar(char data);

/**
 * @brief Configura el comportamiento visual del cursor en pantalla.
 * @param[in] mode Modo seleccionado de la enumeración @ref lcd_cursor_mode_t.
 */
void LCD_SetCursorMode(lcd_cursor_mode_t mode);

/**
 * @brief Desplaza el contenido visible de la pantalla una posición hacia la izquierda o derecha.
 * @note  El contenido de la DDRAM no se modifica, solo la ventana visible.
 * @param[in] direction Dirección de desplazamiento de la enumeración @ref lcd_shift_dir_t.
 */
void LCD_Shift(lcd_shift_dir_t direction);

/**
 * @brief Crea o redefine un carácter personalizado en la memoria CGRAM del LCD.
 * @warning Al finalizar, el cursor queda en la posición (0,0) de la DDRAM; llamar a
 *          LCD_SetCursor() antes de continuar escribiendo.
 * @param[in] location Índice de la CGRAM a modificar (0 a 7).
 * @param[in] charmap  Arreglo de 8 bytes que define el carácter (5x8; solo se usan los 5 bits bajos).
 *                     Si es NULL no hace nada.
 */
void LCD_CreateCustomChar(uint8_t location, const uint8_t charmap[]);

/**
 * @brief Imprime un número entero con signo en la posición actual del cursor.
 * @param[in] value Valor de 32 bits con signo a mostrar (soporta INT32_MIN).
 */
void LCD_PrintInt(int32_t value);

/**
 * @brief Imprime un número flotante con cantidad fija de decimales.
 * @details El redondeo se aplica sobre el valor completo, por lo que 0.999 con
 *          2 decimales se muestra como "1.00". Casos especiales: NaN se muestra como
 *          "NaN" y valores con módulo mayor a 2.0e9 (incluye infinitos) como "Ovf".
 * @note  La precisión útil de un @c float es de ~7 dígitos significativos.
 * @param[in] value          Valor de tipo float a mostrar.
 * @param[in] decimal_places Cantidad de decimales (0 a @ref LCD_FLOAT_MAX_DECIMALS).
 *                           Con 0 no se imprime el punto decimal.
 */
void LCD_PrintFloat(float value, uint8_t decimal_places);

/**
 * @brief Dibuja una barra de progreso con caracteres estándar en una posición dada.
 * @details Formato: [>>>>      ]. Si @p width es menor a 2 no se dibuja nada.
 * @param[in] row        Fila donde se ubicará la barra.
 * @param[in] col        Columna inicial de la barra.
 * @param[in] width      Ancho total en caracteres (incluyendo los corchetes).
 * @param[in] percentage Porcentaje de llenado (0 a 100; valores mayores se saturan a 100).
 */
void LCD_DrawProgressBar(uint8_t row, uint8_t col, uint8_t width, uint8_t percentage);

/**
 * @brief Ejecuta una rutina de testeo y demostración de las funcionalidades del LCD.
 * @note  Bloqueante (~15 s). Debe llamarse luego de LCD_Init().
 * @param[in] type Geometría del display a testear (LCD_16X2 o LCD_20X4).
 */
void LCD_Test(lcd_type_t type);

/** @} */ // fin del grupo LCD_STM32

#endif /* LCD_STM32_H_ */

/**
 * @file lcd_i2c_STM32.h
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Driver de Capa 2 para pantallas LCD Hitachi HD44780 mediante interfaz I2C (PCF8574).
 * @details Permite el control completo de displays alfanuméricos 16x2 y 20x4 a través
 *          de un expansor I2C PCF8574, adaptándose a la HAL de STM32.
 * @version 2.1
 * @date 2026
 */

#ifndef LCD_I2C_STM32_H_
#define LCD_I2C_STM32_H_

#include <stdint.h>
#include "main.h"
#include "utils.h"

/** @brief Geometría del display LCD. */
typedef enum {
    LCD_I2C_16X2, ///< Display de 16 columnas por 2 filas
    LCD_I2C_20X4  ///< Display de 20 columnas por 4 filas
} lcd_i2c_type_t;

/** @brief Modos de visualización del cursor. */
typedef enum {
    LCD_I2C_CURSOR_OFF   = 0x0C, ///< Cursor oculto, display encendido
    LCD_I2C_CURSOR_ON    = 0x0E, ///< Cursor fijo visible (subrayado)
    LCD_I2C_CURSOR_BLINK = 0x0F  ///< Cursor parpadeante (bloque)
} lcd_i2c_cursor_mode_t;

/** @brief Direcciones de desplazamiento de la pantalla. */
typedef enum {
    LCD_I2C_SHIFT_LEFT  = 0x18, ///< Desplazar contenido completo hacia la izquierda
    LCD_I2C_SHIFT_RIGHT = 0x1C  ///< Desplazar contenido completo hacia la derecha
} lcd_i2c_shift_dir_t;

/**
 * @brief Configuración y abstracción de hardware del LCD por I2C.
 */
typedef struct {
    I2C_HandleTypeDef *hi2c;    ///< Puntero al handler de I2C de la HAL
    uint8_t           i2c_addr; ///< Dirección I2C del PCF8574 (ej: 0x27 << 1)
    lcd_i2c_type_t    type;     ///< Tipo de display (LCD_I2C_16X2 o LCD_I2C_20X4)
} LCD_I2C_Config_t;

/**
 * @brief Inicializa el hardware y la interfaz I2C del LCD.
 * @details Si @p config es NULL, el handler es NULL o el expansor no responde en el bus,
 *          el driver queda deshabilitado (las demás funciones no hacen nada) y se
 *          activa el flag de error consultable con @ref LCD_I2C_GetError.
 * @param config Puntero a la estructura de configuración @ref LCD_I2C_Config_t.
 */
void LCD_I2C_Init(const LCD_I2C_Config_t *config);

/**
 * @brief Devuelve y limpia el flag de error del driver.
 * @retval 1 Hubo al menos un error (init o transmisión I2C) desde la última consulta.
 * @retval 0 Sin errores.
 */
uint8_t LCD_I2C_GetError(void);

/**
 * @brief Enciende o apaga la retroiluminación.
 * @param on 0 = apagada, cualquier otro valor = encendida.
 */
void LCD_I2C_Backlight(uint8_t on);

/** @brief Borra el display y retorna el cursor a (0,0). */
void LCD_I2C_Clear(void);

/**
 * @brief Posiciona el cursor. Los valores fuera de rango se limitan (clamp).
 * @param row Fila (0 a 1 para 16x2; 0 a 3 para 20x4).
 * @param col Columna (0 a 15 o 0 a 19 según la geometría).
 */
void LCD_I2C_SetCursor(uint8_t row, uint8_t col);

/**
 * @brief Imprime un string terminado en nulo en la posición actual del cursor.
 * @param str String a imprimir. Si es NULL no hace nada.
 */
void LCD_I2C_Print(const char *str);

/**
 * @brief Escribe un único carácter en la posición actual del cursor.
 * @param data Carácter a enviar.
 */
void LCD_I2C_WriteChar(char data);

/**
 * @brief Configura el comportamiento visual del cursor.
 * @param mode Modo de @ref lcd_i2c_cursor_mode_t.
 */
void LCD_I2C_SetCursorMode(lcd_i2c_cursor_mode_t mode);

/**
 * @brief Desplaza el contenido visible de la pantalla.
 * @param direction Dirección de @ref lcd_i2c_shift_dir_t.
 */
void LCD_I2C_Shift(lcd_i2c_shift_dir_t direction);

/**
 * @brief Crea o redefine un carácter personalizado en la CGRAM.
 * @note Al finalizar vuelve a modo DDRAM y el cursor queda en (0,0);
 *       llamar a @ref LCD_I2C_SetCursor para continuar escribiendo.
 * @param location Índice de la CGRAM (0 a 7).
 * @param charmap Arreglo de 8 bytes con la matriz de píxeles (5x8).
 */
void LCD_I2C_CreateCustomChar(uint8_t location, const uint8_t charmap[]);

/**
 * @brief Imprime un entero con signo de 32 bits.
 * @param value Valor a mostrar.
 */
void LCD_I2C_PrintInt(int32_t value);

/**
 * @brief Imprime un float con cantidad fija de decimales.
 * @details Maneja el acarreo por redondeo (0.996 con 2 decimales -> 1.00).
 *          Los decimales se limitan a 9. NaN y valores fuera de rango de int32
 *          se muestran como "NaN" / "OVF".
 * @param value Valor a mostrar.
 * @param decimal_places Dígitos decimales (0 a 9).
 */
void LCD_I2C_PrintFloat(float value, uint8_t decimal_places);

/**
 * @brief Dibuja una barra de progreso con formato [>>>>    ].
 * @param row Fila de la barra.
 * @param col Columna inicial.
 * @param width Ancho total incluyendo corchetes (mínimo 3).
 * @param percentage Porcentaje de llenado (0 a 100).
 */
void LCD_I2C_DrawProgressBar(uint8_t row, uint8_t col, uint8_t width, uint8_t percentage);

/**
 * @brief Rutina completa de testeo y demostración del LCD.
 * @param type Geometría del display a testear.
 */
void LCD_I2C_Test(lcd_i2c_type_t type);

#endif /* LCD_I2C_STM32_H_ */

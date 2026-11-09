/**
 * @file    OLED_Fonts_STM32.h
 * @author  Mamani Flores Carlos (UTN FRT)
 * @brief   Cabecera de fuentes tipográficas para el driver OLED.
 * @details Declara las fuentes disponibles (6x8, 7x10, 11x18 y 16x26). Todas cubren
 *          el rango ASCII imprimible de 32 (' ') a 126 ('~'), es decir 95 caracteres.
 *
 * @par Cómo se ubica un carácter en la tabla
 * @code
 * indice_carácter = c - 32;
 * // Fuente 6x8 (por columnas):      data[indice_carácter * width  + columna]
 * // Resto (por filas de 16 bits):   data[indice_carácter * height + fila]
 * @endcode
 * Por lo tanto, cada tabla debe tener exactamente `95 * width` (6x8) o `95 * height`
 * (resto) elementos; si falta o sobra uno, todos los caracteres siguientes se
 * desplazan y se ven corruptos.
 *
 * @par Formatos de datos
 * - **6x8**: 1 elemento por columna. El bit 0 es la fila superior y el bit 7 la inferior.
 * - **7x10, 11x18 y 16x26**: 1 elemento de 16 bits por fila. El bit 15 es la columna
 *   izquierda; los bits no usados quedan en cero (a la derecha del glifo).
 *
 * @par Consumo de flash (aproximado, las tablas son `const`)
 * | Fuente | Elementos | Flash  |
 * |--------|-----------|--------|
 * | 6x8    | 570       | ~1.1 KB |
 * | 7x10   | 950       | ~1.9 KB |
 * | 11x18  | 1710      | ~3.4 KB |
 * | 16x26  | 2470      | ~4.9 KB |
 *
 * @par Cómo agregar una fuente nueva
 * 1. Generar la tabla de 95 caracteres (de ' ' a '~') en alguno de los dos formatos.
 * 2. En el .c, definir `static const uint16_t font_data_WxH[]` y su `const FontDef Font_WxH`.
 * 3. Declarar acá `extern const FontDef Font_WxH;`.
 * Para fuentes por filas, el ancho no puede superar 16 píxeles.
 *
 * @version 1.1
 * @date    2026
 */

#ifndef OLED_FONTS_STM32_H
#define OLED_FONTS_STM32_H

#include <stdint.h>
#include "OLED_Display_STM32.h"   /* Define el tipo FontDef */

/** @defgroup OLED_Fonts_Instances Instancias globales de tipografías
 *  @brief Pasar la dirección de estas variables a OLED_DrawChar() / OLED_DrawString().
 *         Ejemplo: `OLED_DrawString(&oled, 0, 0, "Hola", &Font_7x10, OLED_COLOR_WHITE);`
 *  @{
 */
extern const FontDef Font_6x8;   /**< 6x8 px, por columnas (8 bits por columna). Ideal para texto denso. */
extern const FontDef Font_7x10;  /**< 7x10 px, por filas de 16 bits. Buen equilibrio legibilidad/espacio. */
extern const FontDef Font_11x18; /**< 11x18 px, por filas de 16 bits. Títulos y valores destacados. */
extern const FontDef Font_16x26; /**< 16x26 px, por filas de 16 bits. Solo entra 1 línea de ~7 caracteres en 128x32. */
/** @} */

#endif /* OLED_FONTS_STM32_H */

/**
 * ******************************************************************************
 * @file          tft_fonts_STM32.h
 * @brief         Definiciones de estructuras de fuentes para el driver ST7735 y pantallas.
 * @author        Mamani Flores Carlos (UTN FRT)
 * @version       2.2
 * @date          2026
 * ******************************************************************************
 */

#ifndef TFT_FONTS_STM32_H
#define TFT_FONTS_STM32_H

#include <stdint.h>

/**
 * @brief Organización del mapa de bits de una fuente.
 */
typedef enum {
    FONT_TYPE_ROWS    = 0,  ///< Una palabra de 16 bits por fila; bit 15 = píxel más a la izquierda (7x10, 11x18, 16x26).
    FONT_TYPE_COLUMNS = 1   ///< Un byte por columna; bit 0 = píxel superior (6x8).
} FontType;

/**
 * @brief Estructura que define las propiedades de una fuente tipográfica.
 * @note  Los datos cubren los caracteres ASCII 32 (' ') a 126 ('~').
 *        Si se omite `.type` en un inicializador designado, el valor por defecto es FONT_TYPE_ROWS.
 */
typedef struct {
    uint8_t         width;   ///< Ancho de la fuente en píxeles
    uint8_t         height;  ///< Alto de la fuente en píxeles
    const uint16_t *data;    ///< Puntero al array de datos del mapa de bits
    FontType        type;    ///< Organización del mapa de bits (@ref FontType)
} FontDef;

/**
 * @brief Declaraciones externas de las fuentes disponibles (6x8, 7x10, 11x18 y 16x26).
 */
extern const FontDef Font_6x8;
extern const FontDef Font_7x10;
extern const FontDef Font_11x18;
extern const FontDef Font_16x26;

#endif /* TFT_FONTS_STM32_H */

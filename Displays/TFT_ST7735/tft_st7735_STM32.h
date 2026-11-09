/*******************************************************************************
 * @file          tft_st7735_STM32.h
 * @brief         Cabecera del driver para la pantalla LCD ST7735 (STM32 HAL).
 * @author        Mamani Flores Carlos (UTN FRT)
 * @version       2.3
 * @date          2026
 ******************************************************************************/

#ifndef TFT_ST7735_STM32_H
#define TFT_ST7735_STM32_H

#include "main.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tft_fonts_STM32.h>
#include <tft_graphics_STM32.h>

/**
 * @defgroup TFT_ST7735_CONFIG Definiciones de Configuración
 * @{
 */
#define TFT_ST7735_IS_128X128 1     /**< 1 = panel 128x128, 0 = panel 128x160 */

#if TFT_ST7735_IS_128X128
#define TFT_ST7735_WIDTH      128
#define TFT_ST7735_HEIGHT     128
#define TFT_ST7735_COLSTART   2     /**< Offset de columna de la RAM para este panel */
#define TFT_ST7735_ROWSTART   1     /**< Offset de fila de la RAM para este panel */
#else
#define TFT_ST7735_WIDTH      128
#define TFT_ST7735_HEIGHT     160
#define TFT_ST7735_COLSTART   0
#define TFT_ST7735_ROWSTART   0
#endif

#define TFT_ST7735_DELAY      0x80  /**< Bandera de retardo en las tablas de inicialización */

/** Poner en 1 si los arrays de imagen ya tienen el byte alto primero en memoria
 *  (big-endian, generados por conversores "pre-swapped"). En 0, DrawImage los intercambia. */
#ifndef TFT_IMAGE_PRESWAPPED
#define TFT_IMAGE_PRESWAPPED  0
#endif
/** @} */

/**
 * @defgroup TFT_ST7735_COMMANDS Comandos de Pantalla ST7735
 * @{
 */
#define TFT_ST7735_NOP      0x00
#define TFT_ST7735_SWRESET  0x01
#define TFT_ST7735_RDDID    0x04
#define TFT_ST7735_RDDST    0x09
#define TFT_ST7735_SLPIN    0x10
#define TFT_ST7735_SLPOUT   0x11
#define TFT_ST7735_PTLON    0x12
#define TFT_ST7735_NORON    0x13
#define TFT_ST7735_INVOFF   0x20
#define TFT_ST7735_INVON    0x21
#define TFT_ST7735_DISPOFF  0x28
#define TFT_ST7735_DISPON   0x29
#define TFT_ST7735_CASET    0x2A
#define TFT_ST7735_RASET    0x2B
#define TFT_ST7735_RAMWR    0x2C
#define TFT_ST7735_RAMRD    0x2E
#define TFT_ST7735_COLMOD   0x3A
#define TFT_ST7735_MADCTL   0x36
#define TFT_ST7735_FRMCTR1  0xB1
#define TFT_ST7735_FRMCTR2  0xB2
#define TFT_ST7735_FRMCTR3  0xB3
#define TFT_ST7735_INVCTR   0xB4
#define TFT_ST7735_PWCTR1   0xC0
#define TFT_ST7735_PWCTR2   0xC1
#define TFT_ST7735_PWCTR3   0xC2
#define TFT_ST7735_PWCTR4   0xC3
#define TFT_ST7735_PWCTR5   0xC4
#define TFT_ST7735_VMCTR1   0xC5
#define TFT_ST7735_GMCTRP1  0xE0
#define TFT_ST7735_GMCTRN1  0xE1
/** @} */

/**
 * @defgroup TFT_ST7735_MADCTL Banderas MADCTL
 * @{
 */
#define TFT_ST7735_MADCTL_MY  0x80
#define TFT_ST7735_MADCTL_MX  0x40
#define TFT_ST7735_MADCTL_MV  0x20
#define TFT_ST7735_MADCTL_ML  0x10
#define TFT_ST7735_MADCTL_RGB 0x00
#define TFT_ST7735_MADCTL_BGR 0x08
#define TFT_ST7735_MADCTL_MH  0x04
/** @} */

/**
 * @defgroup TFT_ST7735_COLORS Definiciones de Colores 5-6-5
 * @{
 */
#define TFT_BLACK   0x0000
#define TFT_BLUE    0x001F
#define TFT_RED     0xF800
#define TFT_GREEN   0x07E0
#define TFT_CYAN    0x07FF
#define TFT_MAGENTA 0xF81F
#define TFT_YELLOW  0xFFE0
#define TFT_WHITE   0xFFFF

/** Convierte r,g,b de 8 bits a RGB565. Los argumentos van entre paréntesis (seguro con expresiones). */
#define color565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

/* Nombres antiguos, por compatibilidad. Definir TFT_DISABLE_LEGACY_COLOR_NAMES
 * (en los símbolos del proyecto) si chocan con otra librería. */
#ifndef TFT_DISABLE_LEGACY_COLOR_NAMES
#define BLACK   TFT_BLACK
#define BLUE    TFT_BLUE
#define RED     TFT_RED
#define GREEN   TFT_GREEN
#define CYAN    TFT_CYAN
#define MAGENTA TFT_MAGENTA
#define YELLOW  TFT_YELLOW
#define WHITE   TFT_WHITE
#endif
/** @} */

/**
 * @brief Inicializa la pantalla ST7735 configurando el bus SPI, pines de control y registros internos.
 * @param hspi Puntero al handler del periférico SPI de la HAL (no puede ser NULL).
 * @param rotation_val Orientación inicial de la pantalla (0 a 3; se aplica módulo 4).
 */
void tft_st7735_Init(SPI_HandleTypeDef *hspi, uint8_t rotation_val);

/** @brief Selecciona el dispositivo poniendo el pin Chip Select (CS) en nivel bajo. */
void tft_st7735_Select(void);

/** @brief Deselecciona el dispositivo poniendo el pin Chip Select (CS) en nivel alto. */
void tft_st7735_Unselect(void);

/** @brief Ejecuta el ciclo completo de reseteo físico por hardware mediante el pin RST. */
void tft_st7735_Reset(void);

/**
 * @brief Configura la orientación y rotación de la pantalla mediante el registro MADCTL.
 * @param m Valor de rotación deseado (0 a 3).
 */
void tft_st7735_SetRotation(uint8_t m);

/**
 * @brief Define la ventana rectangular de memoria activa (CASET y RASET) y deja el controlador listo para RAMWR.
 * @warning Función de bajo nivel: el llamador debe haber puesto CS en bajo (tft_st7735_Select())
 *          y es responsable de liberarlo después.
 * @param x0 Columna inicial.
 * @param y0 Fila inicial.
 * @param x1 Columna final.
 * @param y1 Fila final.
 */
void tft_st7735_SetAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/** @brief Ancho activo actual (depende de la rotación). */
int16_t tft_st7735_GetWidth(void);

/** @brief Alto activo actual (depende de la rotación). */
int16_t tft_st7735_GetHeight(void);

/**
 * @brief Devuelve el último error SPI de la HAL y lo borra.
 * @return HAL_OK si no hubo errores desde la última consulta.
 */
HAL_StatusTypeDef tft_st7735_GetLastError(void);

/**
 * @brief Dibuja un píxel individual de 16 bits (RGB565) en coordenadas específicas.
 * @param x Coordenada X (columna).
 * @param y Coordenada Y (fila).
 * @param color Valor del color en formato RGB565.
 */
void tft_st7735_DrawPixel(uint16_t x, uint16_t y, uint16_t color);

/**
 * @brief Imprime una cadena de texto usando una fuente dada.
 * @details Salta de línea automáticamente al llegar al borde derecho y con '\n'
 *          (vuelve a la columna `x` inicial). Los caracteres fuera de ASCII 32..126
 *          (incluidos los bytes UTF-8 de ñ, á, etc.) se muestran como '?'.
 *          Se detiene si la siguiente línea no entra en pantalla.
 * @param x Coordenada X de inicio.
 * @param y Coordenada Y de inicio.
 * @param str Puntero al string terminado en nulo.
 * @param font Estructura de la fuente tipográfica (@ref FontDef).
 * @param color Color del texto en RGB565.
 * @param bgcolor Color de fondo del carácter en RGB565.
 */
void tft_st7735_WriteString(uint16_t x, uint16_t y, const char* str, FontDef font, uint16_t color, uint16_t bgcolor);

/**
 * @brief Rellena un área rectangular con un color sólido RGB565 (recorta a los límites de pantalla).
 * @param x Coordenada X superior izquierda.
 * @param y Coordenada Y superior izquierda.
 * @param w Ancho del rectángulo en píxeles.
 * @param h Alto del rectángulo en píxeles.
 * @param color Color RGB565 de relleno.
 */
void tft_st7735_FillRectangle(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/**
 * @brief Inunda toda la pantalla con un color uniforme dado.
 * @param color Color RGB565.
 */
void tft_st7735_FillScreen(uint16_t color);

/**
 * @brief Dibuja una imagen a color RGB565 en la posición indicada.
 * @details La imagen debe entrar completa en pantalla; de lo contrario no se dibuja.
 *          Por defecto el array es un `uint16_t` normal (valor nativo); ver TFT_IMAGE_PRESWAPPED.
 * @param x Coordenada X superior izquierda.
 * @param y Coordenada Y superior izquierda.
 * @param w Ancho de la imagen.
 * @param h Alto de la imagen.
 * @param data Puntero al array con los píxeles en formato RGB565 (w*h elementos).
 */
void tft_st7735_DrawImage(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t* data);

/**
 * @brief Activa o desactiva la inversión cromática del display por hardware (modo negativo).
 * @param invert true para invertir colores, false para operación normal.
 */
void tft_st7735_InvertColors(bool invert);

/** @brief Ejecuta una rutina integral de pruebas y diagnóstico gráfico para validar el hardware. */
void tft_st7735_Test(void);

#endif /* TFT_ST7735_STM32_H */

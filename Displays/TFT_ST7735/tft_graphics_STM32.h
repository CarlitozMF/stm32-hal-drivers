/*******************************************************************************
 * @file          tft_graphics_STM32.h
 * @brief         Declaraciones de funciones gráficas básicas para el driver ST7735.
 * @author        Mamani Flores Carlos (UTN FRT)
 * @details       Contiene las declaraciones para operaciones geométricas como líneas,
 *                rectángulos, círculos, triángulos y esquinas redondeadas.
 *                Todas las primitivas recortan automáticamente a los límites de pantalla
 *                (se admiten coordenadas negativas o fuera de rango).
 * @version       2.2
 * @date          2026
 ******************************************************************************/

#ifndef TFT_GRAPHICS_STM32_H
#define TFT_GRAPHICS_STM32_H

#include <stdint.h>

/**
 * @brief Dibuja un solo píxel en la pantalla (ignora coordenadas fuera de pantalla).
 * @param x Coordenada X (columna).
 * @param y Coordenada Y (fila).
 * @param color Valor de color RGB565 del píxel.
 */
void tft_drawPixel(int16_t x, int16_t y, uint16_t color);

/**
 * @brief Dibuja una línea genérica entre dos puntos usando Bresenham, agrupando tramos
 *        contiguos en rectángulos de 1 píxel de espesor para reducir transacciones SPI.
 * @param x0 Coordenada X inicial.
 * @param y0 Coordenada Y inicial.
 * @param x1 Coordenada X final.
 * @param y1 Coordenada Y final.
 * @param color Color RGB565 de la línea.
 */
void tft_writeLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);

/**
 * @brief Dibuja una línea vertical (una sola transacción de relleno).
 * @param x Coordenada X de origen.
 * @param y Coordenada Y inicial.
 * @param h Alto de la línea en píxeles (si es <= 0 no dibuja nada).
 * @param color Color RGB565.
 */
void tft_drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color);

/**
 * @brief Dibuja una línea horizontal (una sola transacción de relleno).
 * @param x Coordenada X inicial.
 * @param y Coordenada Y de origen.
 * @param w Ancho de la línea en píxeles (si es <= 0 no dibuja nada).
 * @param color Color RGB565.
 */
void tft_drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color);

/**
 * @brief Rellena un rectángulo recortándolo a la pantalla.
 * @param x Coordenada X superior izquierda.
 * @param y Coordenada Y superior izquierda.
 * @param w Ancho del rectángulo.
 * @param h Alto del rectángulo.
 * @param color Color RGB565 de relleno.
 */
void tft_fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

/**
 * @brief Dibuja una línea entre dos puntos seleccionando automáticamente el método óptimo.
 * @param x0 Coordenada X inicial.
 * @param y0 Coordenada Y inicial.
 * @param x1 Coordenada X final.
 * @param y1 Coordenada Y final.
 * @param color Color RGB565.
 */
void tft_drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);

/**
 * @brief Dibuja un círculo sin relleno utilizando simetría de octantes.
 * @param x0 Coordenada X del centro.
 * @param y0 Coordenada Y del centro.
 * @param r Radio del círculo en píxeles.
 * @param color Color RGB565 del contorno.
 */
void tft_drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);

/**
 * @brief Función auxiliar para dibujar arcos de círculos (esquinas específicas).
 * @param x0 Coordenada X del centro.
 * @param y0 Coordenada Y del centro.
 * @param r Radio del arco.
 * @param cornername Máscara de bits indicando las esquinas a dibujar.
 * @param color Color RGB565.
 */
void tft_drawCircleHelper(int16_t x0, int16_t y0, int16_t r, uint8_t cornername, uint16_t color);

/**
 * @brief Función auxiliar para rellenar arcos de círculos (utilizada en rectángulos redondeados).
 * @param x0 Coordenada X del centro.
 * @param y0 Coordenada Y del centro.
 * @param r Radio.
 * @param corners Máscara de esquinas.
 * @param delta Desplazamiento de ajuste de altura.
 * @param color Color RGB565.
 */
void tft_fillCircleHelper(int16_t x0, int16_t y0, int16_t r, uint8_t corners, int16_t delta, uint16_t color);

/**
 * @brief Rellena un círculo completo en la pantalla.
 * @param x0 Coordenada X del centro.
 * @param y0 Coordenada Y del centro.
 * @param r Radio.
 * @param color Color RGB565 de relleno.
 */
void tft_fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);

/**
 * @brief Dibuja un rectángulo sin relleno (solo contorno).
 * @param x Coordenada X superior izquierda.
 * @param y Coordenada Y superior izquierda.
 * @param w Ancho.
 * @param h Alto.
 * @param color Color RGB565.
 */
void tft_drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

/**
 * @brief Dibuja un rectángulo con esquinas redondeadas.
 * @param x Coordenada X.
 * @param y Coordenada Y.
 * @param w Ancho.
 * @param h Alto.
 * @param r Radio de las esquinas redondeadas.
 * @param color Color RGB565.
 */
void tft_drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);

/**
 * @brief Rellena un rectángulo con esquinas redondeadas.
 * @param x Coordenada X.
 * @param y Coordenada Y.
 * @param w Ancho.
 * @param h Alto.
 * @param r Radio de las esquinas.
 * @param color Color RGB565 de relleno.
 */
void tft_fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);

/**
 * @brief Dibuja un triángulo definido por tres puntos (vértices).
 * @param x0 Coordenada X del primer vértice.
 * @param y0 Coordenada Y del primer vértice.
 * @param x1 Coordenada X del segundo vértice.
 * @param y1 Coordenada Y del segundo vértice.
 * @param x2 Coordenada X del tercer vértice.
 * @param y2 Coordenada Y del tercer vértice.
 * @param color Color RGB565 del contorno.
 */
void tft_drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color);

/**
 * @brief Rellena un triángulo definido por tres vértices mediante líneas horizontales.
 * @param x0 Coordenada X del primer vértice.
 * @param y0 Coordenada Y del primer vértice.
 * @param x1 Coordenada X del segundo vértice.
 * @param y1 Coordenada Y del segundo vértice.
 * @param x2 Coordenada X del tercer vértice.
 * @param y2 Coordenada Y del tercer vértice.
 * @param color Color RGB565 de relleno.
 */
void tft_fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color);

/**
 * @brief Rellena toda la pantalla con un color uniforme dado.
 * @param color Color RGB565 con el que se inundará el display.
 */
void tft_fillScreen(uint16_t color);

#endif /* TFT_GRAPHICS_STM32_H */

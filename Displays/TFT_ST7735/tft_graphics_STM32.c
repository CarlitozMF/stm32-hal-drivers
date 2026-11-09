/*******************************************************************************
 * @file          tft_graphics_STM32.c
 * @brief         Implementación de funciones gráficas básicas para el driver ST7735.
 * @details       Proveen una capa de abstracción geométrica basada en tft_st7735_STM32.h.
 *                Líneas rápidas, rectángulos y rellenos usan tft_fillRect (una sola
 *                transacción SPI por tramo), que además recorta a los límites de pantalla.
 * @author        Mamani Flores Carlos (UTN FRT)
 * @version       2.2
 * @date          2026
 ******************************************************************************/

#include <stdlib.h>
#include <tft_graphics_STM32.h>
#include <tft_st7735_STM32.h>

/* Macro auxiliar para intercambiar dos valores enteros de 16 bits */
#define _swap_int16_t(a, b) \
  {                         \
    int16_t t = a;          \
    a = b;                  \
    b = t;                  \
  }

/*
 * Envía un píxel individual al controlador (ignora coordenadas negativas o fuera de pantalla).
 */
void tft_drawPixel(int16_t x, int16_t y, uint16_t color)
{
    if (x < 0 || y < 0) return;
    tft_st7735_DrawPixel((uint16_t)x, (uint16_t)y, color);
}

/*
 * Rellena un rectángulo recortando la parte con coordenadas negativas; el recorte por
 * derecha/abajo lo hace el driver de bajo nivel.
 */
void tft_fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    tft_st7735_FillRectangle((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, color);
}

/*
 * Bresenham con agrupación de tramos: en lugar de un píxel por transacción, acumula los
 * píxeles consecutivos de un mismo "escalón" y los envía como un rectángulo de 1 px de espesor.
 */
void tft_writeLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    int16_t steep = abs(y1 - y0) > abs(x1 - x0);
    if (steep) {
        _swap_int16_t(x0, y0);
        _swap_int16_t(x1, y1);
    }
    if (x0 > x1) {
        _swap_int16_t(x0, x1);
        _swap_int16_t(y0, y1);
    }
    int16_t dx = x1 - x0;
    int16_t dy = abs(y1 - y0);
    int16_t err = dx / 2;
    int16_t ystep = (y0 < y1) ? 1 : -1;

    int16_t xs = x0;     /* inicio del tramo actual */
    int16_t dlen = 0;    /* largo del tramo actual */

    for (; x0 <= x1; x0++) {
        dlen++;
        err -= dy;
        if (err < 0) {
            err += dx;
            if (steep) tft_fillRect(y0, xs, 1, dlen, color);
            else       tft_fillRect(xs, y0, dlen, 1, color);
            dlen = 0;
            y0 += ystep;
            xs = x0 + 1;
        }
    }
    if (dlen) {
        if (steep) tft_fillRect(y0, xs, 1, dlen, color);
        else       tft_fillRect(xs, y0, dlen, 1, color);
    }
}

/*
 * Línea vertical: un rectángulo de 1 píxel de ancho (w/h <= 0 no dibuja nada).
 */
void tft_drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color)
{
    tft_fillRect(x, y, 1, h, color);
}

/*
 * Línea horizontal: un rectángulo de 1 píxel de alto (w <= 0 no dibuja nada).
 */
void tft_drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color)
{
    tft_fillRect(x, y, w, 1, color);
}

/*
 * Usa líneas rápidas si la trayectoria es perfectamente horizontal o vertical,
 * de lo contrario recurre al algoritmo general de Bresenham (tft_writeLine).
 */
void tft_drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    if (x0 == x1) {
        if (y0 > y1) _swap_int16_t(y0, y1);
        tft_drawFastVLine(x0, y0, y1 - y0 + 1, color);
    } else if (y0 == y1) {
        if (x0 > x1) _swap_int16_t(x0, x1);
        tft_drawFastHLine(x0, y0, x1 - x0 + 1, color);
    } else {
        tft_writeLine(x0, y0, x1, y1, color);
    }
}

/*
 * Dibuja un círculo aprovechando la simetría de los 8 octantes (algoritmo de puntos medios).
 */
void tft_drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color)
{
    int16_t f = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t x = 0;
    int16_t y = r;

    tft_drawPixel(x0, y0 + r, color);
    tft_drawPixel(x0, y0 - r, color);
    tft_drawPixel(x0 + r, y0, color);
    tft_drawPixel(x0 - r, y0, color);

    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;

        tft_drawPixel(x0 + x, y0 + y, color);
        tft_drawPixel(x0 - x, y0 + y, color);
        tft_drawPixel(x0 + x, y0 - y, color);
        tft_drawPixel(x0 - x, y0 - y, color);
        tft_drawPixel(x0 + y, y0 + x, color);
        tft_drawPixel(x0 - y, y0 + x, color);
        tft_drawPixel(x0 + y, y0 - x, color);
        tft_drawPixel(x0 - y, y0 - x, color);
    }
}

/*
 * Renderiza arcos específicos correspondientes a esquinas en figuras redondeadas.
 */
void tft_drawCircleHelper(int16_t x0, int16_t y0, int16_t r, uint8_t cornername, uint16_t color)
{
    int16_t f = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t x = 0;
    int16_t y = r;

    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;
        if (cornername & 0x4) {
            tft_drawPixel(x0 + x, y0 + y, color);
            tft_drawPixel(x0 + y, y0 + x, color);
        }
        if (cornername & 0x2) {
            tft_drawPixel(x0 + x, y0 - y, color);
            tft_drawPixel(x0 + y, y0 - x, color);
        }
        if (cornername & 0x8) {
            tft_drawPixel(x0 - y, y0 + x, color);
            tft_drawPixel(x0 - x, y0 + y, color);
        }
        if (cornername & 0x1) {
            tft_drawPixel(x0 - y, y0 - x, color);
            tft_drawPixel(x0 - x, y0 - y, color);
        }
    }
}

/*
 * Rellena arcos circulares, ideal para construir los bordes de rectángulos redondeados sólidos.
 */
void tft_fillCircleHelper(int16_t x0, int16_t y0, int16_t r, uint8_t corners, int16_t delta, uint16_t color)
{
    int16_t f = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t x = 0;
    int16_t y = r;
    int16_t px = x;
    int16_t py = y;

    delta++;

    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;

        if (x < (y + 1)) {
            if (corners & 1) tft_drawFastVLine(x0 + x, y0 - y, 2 * y + delta, color);
            if (corners & 2) tft_drawFastVLine(x0 - x, y0 - y, 2 * y + delta, color);
        }
        if (y != py) {
            if (corners & 1) tft_drawFastVLine(x0 + py, y0 - px, 2 * px + delta, color);
            if (corners & 2) tft_drawFastVLine(x0 - py, y0 - px, 2 * px + delta, color);
            py = y;
        }
        px = x;
    }
}

/*
 * Rellena un círculo combinando una línea vertical central y arcos auxiliares.
 */
void tft_fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color)
{
    tft_drawFastVLine(x0, y0 - r, 2 * r + 1, color);
    tft_fillCircleHelper(x0, y0, r, 3, 0, color);
}

/*
 * Dibuja el contorno rectangular utilizando líneas rápidas horizontales y verticales.
 */
void tft_drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    tft_drawFastHLine(x, y, w, color);
    tft_drawFastHLine(x, y + h - 1, w, color);
    tft_drawFastVLine(x, y, h, color);
    tft_drawFastVLine(x + w - 1, y, h, color);
}

/*
 * Rectángulo con esquinas redondeadas, limitando el radio al máximo permitido.
 */
void tft_drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color)
{
    int16_t max_radius = ((w < h) ? w : h) / 2;
    if (r > max_radius) r = max_radius;

    tft_drawFastHLine(x + r, y, w - 2 * r, color);
    tft_drawFastHLine(x + r, y + h - 1, w - 2 * r, color);
    tft_drawFastVLine(x, y + r, h - 2 * r, color);
    tft_drawFastVLine(x + w - 1, y + r, h - 2 * r, color);

    tft_drawCircleHelper(x + r, y + r, r, 1, color);
    tft_drawCircleHelper(x + w - r - 1, y + r, r, 2, color);
    tft_drawCircleHelper(x + w - r - 1, y + h - r - 1, r, 4, color);
    tft_drawCircleHelper(x + r, y + h - r - 1, r, 8, color);
}

/*
 * Rellena un rectángulo con esquinas redondeadas combinando un rectángulo central y helpers circulares.
 */
void tft_fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color)
{
    int16_t max_radius = ((w < h) ? w : h) / 2;
    if (r > max_radius) r = max_radius;

    tft_fillRect(x + r, y, w - 2 * r, h, color);
    tft_fillCircleHelper(x + w - r - 1, y + r, r, 1, h - 2 * r - 1, color);
    tft_fillCircleHelper(x + r, y + r, r, 2, h - 2 * r - 1, color);
}

/*
 * Dibuja un triángulo uniendo sus tres vértices mediante líneas.
 */
void tft_drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color)
{
    tft_drawLine(x0, y0, x1, y1, color);
    tft_drawLine(x1, y1, x2, y2, color);
    tft_drawLine(x2, y2, x0, y0, color);
}

/*
 * Rellena un triángulo ordenando sus vértices por Y y trazando líneas horizontales
 * sucesivas mediante interpolación lineal de pendientes.
 */
void tft_fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color)
{
    int16_t a, b, y, last;

    // Ordenar vértices por coordenada Y ascendente (y0 <= y1 <= y2)
    if (y0 > y1) { _swap_int16_t(y0, y1); _swap_int16_t(x0, x1); }
    if (y1 > y2) { _swap_int16_t(y2, y1); _swap_int16_t(x2, x1); }
    if (y0 > y1) { _swap_int16_t(y0, y1); _swap_int16_t(x0, x1); }

    if (y0 == y2) { // Triángulo degenerado en una línea horizontal
        a = b = x0;
        if (x1 < a) a = x1;
        else if (x1 > b) b = x1;
        if (x2 < a) a = x2;
        else if (x2 > b) b = x2;
        tft_drawFastHLine(a, y0, b - a + 1, color);
        return;
    }

    int16_t dx01 = x1 - x0, dy01 = y1 - y0;
    int16_t dx02 = x2 - x0, dy02 = y2 - y0;
    int16_t dx12 = x2 - x1, dy12 = y2 - y1;
    int32_t sa = 0, sb = 0;

    if (y1 == y2) last = y1;
    else last = y1 - 1;

    // Sección superior del triángulo
    for (y = y0; y <= last; y++) {
        a = x0 + sa / dy01;
        b = x0 + sb / dy02;
        sa += dx01;
        sb += dx02;
        if (a > b) _swap_int16_t(a, b);
        tft_drawFastHLine(a, y, b - a + 1, color);
    }

    sa = (int32_t)dx12 * (y - y1);
    sb = (int32_t)dx02 * (y - y0);

    // Sección inferior del triángulo
    for (; y <= y2; y++) {
        a = x1 + sa / dy12;
        b = x0 + sb / dy02;
        sa += dx12;
        sb += dx02;
        if (a > b) _swap_int16_t(a, b);
        tft_drawFastHLine(a, y, b - a + 1, color);
    }
}

/*
 * Inunda toda la pantalla con un color sólido.
 */
void tft_fillScreen(uint16_t color)
{
    tft_st7735_FillScreen(color);
}

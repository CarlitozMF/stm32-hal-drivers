/*******************************************************************************
 * @file          tft_st7735_STM32.c
 * @brief         Implementación del driver para la pantalla LCD ST7735.
 * @details       Contiene la lógica de control SPI, comandos de inicialización por lotes,
 *                gestión de ventanas de memoria y renderizado de texto y gráficos mediante la HAL.
 * @author        Mamani Flores Carlos (UTN FRT)
 * @version       2.3
 * @date          2026
 ******************************************************************************/

#include <stdlib.h>
#include <tft_st7735_STM32.h>

/* ============================================================================
 * CONFIGURACIÓN INTERNA
 * ============================================================================ */
#define TFT_SPI_CHUNK_BYTES  4096u   /* Máximo de bytes por llamada a HAL_SPI_Transmit */
#define TFT_SPI_TIMEOUT_MS   1000u   /* Timeout por bloque */
#define TFT_FILL_CHUNK_PX    128u    /* Píxeles por bloque en relleno e imágenes */
#define TFT_FONT_MAX_W       16u     /* Mayor ancho de fuente (Font_16x26) */
#define TFT_FONT_MAX_H       26u     /* Mayor alto de fuente (Font_16x26) */

/* ============================================================================
 * VARIABLES PRIVADAS Y ESTADOS DEL DRIVER
 * ============================================================================ */
static SPI_HandleTypeDef *p_st_hspi = NULL; /* Puntero al bus SPI configurado en la HAL */
static HAL_StatusTypeDef  s_last_error = HAL_OK;

static int16_t s_width;       /* Ancho activo actual según la orientación */
static int16_t s_height;      /* Alto activo actual según la orientación */
static uint8_t s_rotation;    /* Orientación actual (0 al 3) */
static uint8_t s_colstart;    /* Offset de columna inicial según el tipo de panel */
static uint8_t s_rowstart;    /* Offset de fila inicial según el tipo de panel */
static uint8_t s_xstart;      /* Margen X efectivo aplicado en las ventanas */
static uint8_t s_ystart;      /* Margen Y efectivo aplicado en las ventanas */

/* ============================================================================
 * SECUENCIAS DE INICIALIZACIÓN POR LOTES (REGISTROS DEL CONTROLADOR)
 * ============================================================================ */

/* Bloque 1: Reseteo por software, salida de sleep, frecuencias de cuadro y voltajes.
 * (MADCTL no está aquí: lo configura SetRotation) */
static const uint8_t init_cmds1[] = {
		14, // Cantidad de comandos en el bloque (¡mantener sincronizado!)
		TFT_ST7735_SWRESET,   TFT_ST7735_DELAY,  150, // Software Reset + Retardo de 150ms
		TFT_ST7735_SLPOUT ,   TFT_ST7735_DELAY,  255, // Salida de Sleep Out + Retardo largo
		TFT_ST7735_FRMCTR1, 3      ,  0x01, 0x2C, 0x2D, // Frecuencia de refresco (modo normal)
		TFT_ST7735_FRMCTR2, 3      ,  0x01, 0x2C, 0x2D, // Frecuencia de refresco (modo inactivo)
		TFT_ST7735_FRMCTR3, 6      ,  0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D, // Frecuencia en modo partial/full
		TFT_ST7735_INVCTR , 1      ,  0x07, // Control de inversión de línea
		TFT_ST7735_PWCTR1 , 3      ,  0xA2, 0x02, 0x84, // Fuentes de poder (AVDD, VCL, VGH, VGL)
		TFT_ST7735_PWCTR2 , 1      ,  0xC5, // Control de corriente de los amplificadores operacionales
		TFT_ST7735_PWCTR3 , 2      ,  0x0A, 0x00, // Ajuste de corriente en modo booster
		TFT_ST7735_PWCTR4 , 2      ,  0x8A, 0x2A, // Ajuste de polaridad de voltaje
		TFT_ST7735_PWCTR5 , 2      ,  0x8A, 0xEE,
		TFT_ST7735_VMCTR1 , 1      ,  0x0E, // Control de voltaje VCOM
		TFT_ST7735_INVOFF , 0      ,     // Desactiva inversión de color inicial
		TFT_ST7735_COLMOD , 1      ,  0x05  // Formato de color: 0x05 = 16 bits por píxel (RGB565)
};

/* Bloque 2: Límites de memoria RAM (CASET y RASET) según el tamaño del panel */
static const uint8_t init_cmds2[] = {
		2,
		TFT_ST7735_CASET  , 4      ,  0x00, 0x00, 0x00, (TFT_ST7735_WIDTH  - 1), // Column Address Set
		TFT_ST7735_RASET  , 4      ,  0x00, 0x00, 0x00, (TFT_ST7735_HEIGHT - 1)  // Row Address Set
};

/* Bloque 3: Curvas de gamma y encendido final de la pantalla */
static const uint8_t init_cmds3[] = {
		4,
		TFT_ST7735_GMCTRP1, 16     ,  0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d,
		0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10, // Gamma positiva
		TFT_ST7735_GMCTRN1, 16     ,  0x03, 0x1d, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
		0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10, // Gamma negativa
		TFT_ST7735_NORON  ,    TFT_ST7735_DELAY, 10,   // Modo de visualización normal
		TFT_ST7735_DISPON ,    TFT_ST7735_DELAY, 100   // Display ON
};

/* ============================================================================
 * FUNCIONES DE BAJO NIVEL (CONTROL DE PINES Y BUS SPI)
 * ============================================================================ */

/* Pone el pin Chip Select (CS) en nivel bajo para habilitar la comunicación SPI con el ST7735 */
void tft_st7735_Select(void)
{
	HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_RESET);
}

/* Pone el pin Chip Select (CS) en nivel alto para deshabilitar y liberar el bus SPI */
void tft_st7735_Unselect(void)
{
	HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET);
}

/* Genera un ciclo completo de reseteo físico por hardware utilizando el pin de Reset (RST) */
void tft_st7735_Reset(void)
{
	HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_SET);
	HAL_Delay(10);

	HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_RESET);
	HAL_Delay(20);

	HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_SET);
	HAL_Delay(200); // Estabilización crítica, sobre todo en displays clónicos económicos
}

/* Transmite un buffer por SPI en bloques (HAL usa tamaño uint16_t), registrando errores */
static void spi_tx(const uint8_t *buf, size_t len)
{
	if (p_st_hspi == NULL) return;

	while (len) {
		uint16_t n = (len > TFT_SPI_CHUNK_BYTES) ? (uint16_t)TFT_SPI_CHUNK_BYTES : (uint16_t)len;
		HAL_StatusTypeDef st = HAL_SPI_Transmit(p_st_hspi, (uint8_t *)buf, n, TFT_SPI_TIMEOUT_MS);
		if (st != HAL_OK) {
			s_last_error = st;
			return;
		}
		buf += n;
		len -= n;
	}
}

/* Envía un byte de comando al ST7735 poniendo el pin Data/Command (D/C) en nivel BAJO */
static void tft_st7735_WriteCommand(uint8_t cmd)
{
	HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_RESET); // Modo Comando (D/C = 0)
	spi_tx(&cmd, 1);
}

/* Envía un bloque de datos por SPI al ST7735 poniendo el pin Data/Command (D/C) en nivel ALTO */
static void tft_st7735_WriteData(const uint8_t *buff, size_t buff_size)
{
	HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_SET); // Modo Datos (D/C = 1)
	spi_tx(buff, buff_size);
}

/* Intérprete privado para procesar las tablas de comandos comprimidas de inicialización */
static void DisplayInit(const uint8_t *addr)
{
	uint8_t numCommands, numArgs;
	uint16_t ms;

	numCommands = *addr++;
	while(numCommands--) {
		uint8_t cmd = *addr++;
		tft_st7735_WriteCommand(cmd);

		numArgs = *addr++;
		ms = numArgs & TFT_ST7735_DELAY; // Extrae la bandera de retardo si está presente
		numArgs &= ~TFT_ST7735_DELAY;

		if(numArgs) {
			tft_st7735_WriteData(addr, numArgs);
			addr += numArgs;
		}

		if(ms) {
			ms = *addr++;
			if(ms == 255) ms = 500;
			HAL_Delay(ms);
		}
	}
}

/* Configura los límites de columnas (CASET) y filas (RASET). Requiere CS en bajo. */
void tft_st7735_SetAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
	// Comando CASET (Column Address Set)
	tft_st7735_WriteCommand(TFT_ST7735_CASET);
	uint8_t data[] = { 0x00, (uint8_t)(x0 + s_xstart), 0x00, (uint8_t)(x1 + s_xstart) };
	tft_st7735_WriteData(data, sizeof(data));

	// Comando RASET (Row Address Set)
	tft_st7735_WriteCommand(TFT_ST7735_RASET);
	data[1] = (uint8_t)(y0 + s_ystart);
	data[3] = (uint8_t)(y1 + s_ystart);
	tft_st7735_WriteData(data, sizeof(data));

	// Comando RAMWR (Memory Write): prepara la escritura de píxeles consecutivos
	tft_st7735_WriteCommand(TFT_ST7735_RAMWR);
}

/* ============================================================================
 * FUNCIONES PÚBLICAS DEL DRIVER
 * ============================================================================ */

int16_t tft_st7735_GetWidth(void)  { return s_width;  }
int16_t tft_st7735_GetHeight(void) { return s_height; }

HAL_StatusTypeDef tft_st7735_GetLastError(void)
{
	HAL_StatusTypeDef e = s_last_error;
	s_last_error = HAL_OK;
	return e;
}

/* Inicializa el controlador enviando secuencias de arranque y configurando la orientación inicial */
void tft_st7735_Init(SPI_HandleTypeDef *hspi, uint8_t rotation_val)
{
	if (hspi == NULL) return;
	p_st_hspi = hspi;
	s_last_error = HAL_OK;

	s_colstart = TFT_ST7735_COLSTART;
	s_rowstart = TFT_ST7735_ROWSTART;

	tft_st7735_Unselect();
	HAL_Delay(50);
	tft_st7735_Reset();
	tft_st7735_Select();

	// Apaga el display al iniciar para evitar parpadeos o basura gráfica
	tft_st7735_WriteCommand(TFT_ST7735_DISPOFF);

	// Carga secuencial de las tres tablas de inicialización (la última enciende el display)
	DisplayInit(init_cmds1);
	DisplayInit(init_cmds2);
	DisplayInit(init_cmds3);

	tft_st7735_SetRotation(rotation_val);
	tft_st7735_Unselect();
}

/* Configura la orientación (0 al 3) actualizando el registro MADCTL */
void tft_st7735_SetRotation(uint8_t m)
{
	uint8_t madctl = 0;
	s_rotation = m % 4;

	switch (s_rotation) {
	case 0: // Vertical estándar (Portrait)
		madctl = TFT_ST7735_MADCTL_MX | TFT_ST7735_MADCTL_MY | TFT_ST7735_MADCTL_BGR;
		s_height = TFT_ST7735_HEIGHT;
		s_width  = TFT_ST7735_WIDTH;
		s_xstart = s_colstart;
		s_ystart = s_rowstart;
		break;
	case 1: // Horizontal girada 90° (Landscape)
		madctl = TFT_ST7735_MADCTL_MY | TFT_ST7735_MADCTL_MV | TFT_ST7735_MADCTL_BGR;
		s_width  = TFT_ST7735_HEIGHT;
		s_height = TFT_ST7735_WIDTH;
		s_ystart = s_colstart;
		s_xstart = s_rowstart;
		break;
	case 2: // Vertical invertida (180°)
		madctl = TFT_ST7735_MADCTL_BGR;
		s_height = TFT_ST7735_HEIGHT;
		s_width  = TFT_ST7735_WIDTH;
		s_xstart = s_colstart;
		s_ystart = s_rowstart;
		break;
	case 3: // Horizontal girada 270°
		madctl = TFT_ST7735_MADCTL_MX | TFT_ST7735_MADCTL_MV | TFT_ST7735_MADCTL_BGR;
		s_width  = TFT_ST7735_HEIGHT;
		s_height = TFT_ST7735_WIDTH;
		s_ystart = s_colstart;
		s_xstart = s_rowstart;
		break;
	}
	tft_st7735_Select();
	tft_st7735_WriteCommand(TFT_ST7735_MADCTL);
	tft_st7735_WriteData(&madctl, 1);
	tft_st7735_Unselect();
}

/* Dibuja un píxel individual de 16 bits (RGB565) validando los límites lógicos */
void tft_st7735_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
	if((x >= s_width) || (y >= s_height)) return;

	tft_st7735_Select();
	tft_st7735_SetAddressWindow(x, y, x, y);
	uint8_t data[] = { (uint8_t)(color >> 8), (uint8_t)(color & 0xFF) };
	tft_st7735_WriteData(data, sizeof(data));
	tft_st7735_Unselect();
}

/* Renderiza un carácter armando el bloque completo en RAM y enviándolo de una sola vez.
 * Requiere CS en bajo. Soporta fuentes por filas (16 bits) y por columnas (8 bits). */
static void tft_st7735_WriteChar(uint16_t x, uint16_t y, char ch, FontDef font, uint16_t color, uint16_t bgcolor)
{
	uint8_t buf[TFT_FONT_MAX_W * TFT_FONT_MAX_H * 2];
	uint32_t k = 0;

	if (font.width == 0 || font.height == 0) return;
	if (font.width > TFT_FONT_MAX_W || font.height > TFT_FONT_MAX_H) return;
	if ((x + font.width > s_width) || (y + font.height > s_height)) return;

	// Caracteres fuera de ASCII imprimible (incluye bytes UTF-8 / valores negativos) -> '?'
	if (ch < 32 || ch > 126) ch = '?';
	uint32_t idx = (uint32_t)(ch - 32);

	for (uint32_t i = 0; i < font.height; i++) {
		uint16_t row = 0;
		if (font.type == FONT_TYPE_ROWS) {
			row = font.data[idx * font.height + i];
		}
		for (uint32_t j = 0; j < font.width; j++) {
			bool on;
			if (font.type == FONT_TYPE_COLUMNS) {
				on = (((uint8_t)font.data[idx * font.width + j] >> i) & 1u) != 0;
			} else {
				on = ((row << j) & 0x8000u) != 0;
			}
			uint16_t c = on ? color : bgcolor;
			buf[k++] = (uint8_t)(c >> 8);
			buf[k++] = (uint8_t)(c & 0xFF);
		}
	}

	tft_st7735_SetAddressWindow(x, y, x + font.width - 1, y + font.height - 1);
	tft_st7735_WriteData(buf, k);
}

/* Imprime cadenas de texto con salto de línea automático y soporte de '\n' */
void tft_st7735_WriteString(uint16_t x, uint16_t y, const char* str, FontDef font, uint16_t color, uint16_t bgcolor)
{
	const uint16_t x_start = x;

	tft_st7735_Select();

	while(*str) {
		if (*str == '\n') {
			x = x_start;
			y += font.height;
			if (y + font.height > s_height) break;
			str++;
			continue;
		}
		if (*str == '\r') {
			str++;
			continue;
		}

		// Salto de línea automático si el carácter no entra en el ancho de pantalla
		if (x + font.width > s_width) {
			x = x_start;
			y += font.height;
			if (y + font.height > s_height) break;
			if (*str == ' ') {
				str++;
				continue;
			}
		}

		tft_st7735_WriteChar(x, y, *str, font, color, bgcolor);
		x += font.width;
		str++;
	}

	tft_st7735_Unselect();
}

/* Rellena un área rectangular con un color sólido, transmitiendo por bloques */
void tft_st7735_FillRectangle(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
	if ((w == 0) || (h == 0)) return;
	if((x >= s_width) || (y >= s_height)) return;
	if((x + w) > s_width)  w = s_width - x;
	if((y + h) > s_height) h = s_height - y;

	uint8_t buf[TFT_FILL_CHUNK_PX * 2];
	uint32_t n = (uint32_t)w * h;
	uint32_t fill = (n < TFT_FILL_CHUNK_PX) ? n : TFT_FILL_CHUNK_PX;
	for (uint32_t i = 0; i < fill; i++) {
		buf[2*i]     = (uint8_t)(color >> 8);
		buf[2*i + 1] = (uint8_t)(color & 0xFF);
	}

	tft_st7735_Select();
	tft_st7735_SetAddressWindow(x, y, x + w - 1, y + h - 1);
	HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_SET);
	while (n) {
		uint32_t c = (n < TFT_FILL_CHUNK_PX) ? n : TFT_FILL_CHUNK_PX;
		spi_tx(buf, c * 2);
		n -= c;
	}
	tft_st7735_Unselect();
}

/* Inunda la pantalla entera con un color uniforme */
void tft_st7735_FillScreen(uint16_t color)
{
	tft_st7735_FillRectangle(0, 0, (uint16_t)s_width, (uint16_t)s_height, color);
}

/* Dibuja una imagen RGB565 volcando los datos a la ventana (con orden de bytes correcto) */
void tft_st7735_DrawImage(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t* data)
{
	if ((data == NULL) || (w == 0) || (h == 0)) return;
	if((x >= s_width) || (y >= s_height)) return;
	if((x + w) > s_width)  return;
	if((y + h) > s_height) return;

	tft_st7735_Select();
	tft_st7735_SetAddressWindow(x, y, x + w - 1, y + h - 1);
	HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_SET);

#if TFT_IMAGE_PRESWAPPED
	spi_tx((const uint8_t*)data, sizeof(uint16_t) * (size_t)w * h);
#else
	uint8_t buf[TFT_FILL_CHUNK_PX * 2];
	uint32_t n = (uint32_t)w * h;
	uint32_t pos = 0;
	while (n) {
		uint32_t c = (n < TFT_FILL_CHUNK_PX) ? n : TFT_FILL_CHUNK_PX;
		for (uint32_t i = 0; i < c; i++) {
			uint16_t px = data[pos + i];
			buf[2*i]     = (uint8_t)(px >> 8);   // El ST7735 espera el byte alto primero
			buf[2*i + 1] = (uint8_t)(px & 0xFF);
		}
		spi_tx(buf, c * 2);
		pos += c;
		n   -= c;
	}
#endif
	tft_st7735_Unselect();
}

/* Activa o desactiva por hardware la inversión de colores del controlador (modo negativo) */
void tft_st7735_InvertColors(bool invert)
{
	tft_st7735_Select();
	tft_st7735_WriteCommand(invert ? TFT_ST7735_INVON : TFT_ST7735_INVOFF);
	tft_st7735_Unselect();
}

/* Rutina integral de pruebas para validar colores planos, fuentes, inversión y primitivas */
void tft_st7735_Test(void)
{
	// 1. Prueba de colores planos y texto institucional
	tft_fillScreen(TFT_BLACK);
	tft_st7735_WriteString(10, 10, "UTN FRT - TD2", Font_7x10, TFT_YELLOW, TFT_BLACK);
	tft_st7735_WriteString(10, 30, "STM32 HAL OK", Font_7x10, TFT_CYAN, TFT_BLACK);
	HAL_Delay(1500);

	tft_fillScreen(TFT_RED);
	HAL_Delay(500);
	tft_fillScreen(TFT_GREEN);
	HAL_Delay(500);
	tft_fillScreen(TFT_BLUE);
	HAL_Delay(500);
	tft_fillScreen(TFT_BLACK);

	// 2. Prueba de inversión de colores por hardware
	tft_st7735_WriteString(10, 10, "Modo Normal", Font_7x10, TFT_WHITE, TFT_BLACK);
	HAL_Delay(1000);

	tft_st7735_InvertColors(true);
	tft_st7735_WriteString(10, 30, "Invertido!", Font_7x10, TFT_WHITE, TFT_BLACK);
	HAL_Delay(1500);

	tft_st7735_InvertColors(false);
	tft_fillScreen(TFT_BLACK);

	// 3. Prueba de primitivas gráficas (rectángulos)
	tft_st7735_WriteString(10, 5, "Primitivas GFX", Font_6x8, TFT_MAGENTA, TFT_BLACK);

	tft_drawRect(10, 20, 108, 90, TFT_RED);
	tft_drawRect(15, 25, 98, 80, TFT_GREEN);
	tft_fillRect(30, 40, 68, 50, TFT_BLUE);
	HAL_Delay(1500);

	// 4. Prueba de círculos
	tft_fillScreen(TFT_BLACK);
	tft_drawCircle(64, 64, 40, TFT_YELLOW);
	tft_fillCircle(64, 64, 20, TFT_CYAN);
	HAL_Delay(1500);

	// 5. Catálogo de fuentes
	tft_fillScreen(TFT_BLACK);
	tft_st7735_WriteString(5, 5,   "6x8", Font_6x8, TFT_WHITE, TFT_BLACK);
	tft_st7735_WriteString(5, 20,  "7x10", Font_7x10, TFT_YELLOW, TFT_BLACK);
	tft_st7735_WriteString(5, 35,  "11x18", Font_11x18, TFT_GREEN, TFT_BLACK);
	tft_st7735_WriteString(5, 65,  "16x26", Font_16x26, TFT_CYAN, TFT_BLACK);
	HAL_Delay(3000);

	// 6. Cierre con banner centrado
	tft_fillScreen(TFT_BLACK);
	tft_fillRect(0, 50, 128, 30, TFT_BLUE);
	tft_st7735_WriteString(14, 58, "Test Completo!", Font_7x10, TFT_WHITE, TFT_BLUE);
	HAL_Delay(2000);
}

# Categoría: Sensores

Esta carpeta contiene la colección de drivers y módulos de adquisición para diversos **sensores** en microcontroladores STM32 utilizando la capa **ST HAL**.

Todos los drivers están optimizados para operar sin bloquear el bucle principal (`while(1)`), utilizando adquisición por interrupción en ADC, buses digitales estándar (`I2C`) o temporizaciones de alta precisión en protocolos `1-Wire`.

## 📦 Drivers Disponibles en esta Categoría

| Driver / Módulo | Interfaz / Periférico | Dependencia `Utils` | Versión | Estado | Descripción Principal | 
| ----- | ----- | ----- | ----- | ----- | ----- | 
| [**`BH1750`**](BH1750/) | I2C | No | `v1.3` | **Estable** | Sensor digital de luz ambiental (luxómetro) de alta resolución ($1\text{ lx}$) con modos de medición continua y *one-shot*. | 
| [**`BMP180`**](BMP180/) | I2C | No | `v2.0` | **Estable** | Sensor barométrico de presión atmosférica y temperatura con calibración de fábrica EEPROM y cálculo de altitud. | 
| [**`DHT11`**](DHT11/) | 1-Wire (GPIO Open-Drain) | No | `v2.1` | **Estable** | Sensor digital de humedad relativa y temperatura con lectura por temporización de pulsos y verificación Checksum. | 
| [**`DS18B20`**](DS18B20/) | 1-Wire (Timer $1\mu\text{s}$ HW) | No | `v1.1` | **Estable** | Sensor de temperatura digital de precisión (9 a 12 bits) con FSM no bloqueante, cálculo CRC8 y soporte Monopunto/Multipunto. | 
| [**`HTU2X`**](HTU2X/) | I2C | No | `v2.2` | **Estable** | Sensor de humedad relativa y temperatura de alta precisión (HTU21D / HTU20D) con cálculo de suma de comprobación CRC-8. | 
| [**`LM35`**](LM35/) | ADC (Interrupción `IT`) | No | `v1.2` | **Estable** | Sensor de temperatura analógico ($10\text{ mV}/^\circ\text{C}$). Filtro de media móvil circular $O(1)$ en ISR y calibración a $3.3\text{V}$. | 
| [**`HC-SR04`**](HCSR04/) | TIM Input Capture (IT) | No | `v1.2` | **Estable** | Sensor de distancia ultrasónico ($2\dots400\text{ cm}$). Medición asíncrona no bloqueante, soporte multi-sensor y filtro de mediana. |


## 🚀 Estructura de las Carpetas

Cada sensor cuenta con su propio subdirectorio autocontenido:

```
Sensores/
├── BH1750/
│   ├── bh1750_STM32.h
│   ├── bh1750_STM32.c
│   └── README.md
├── BMP180/
│   ├── bmp180_STM32.h
│   ├── bmp180_STM32.c
│   └── README.md
├── DHT11/
│   ├── dht11_STM32.h
│   ├── dht11_STM32.c
│   └── README.md
├── DS18B20/
│   ├── ds18b20_STM32.h
│   ├── ds18b20_STM32.c
│   └── README.md
├── HTU2X/
│   ├── htu2x_STM32.h
│   ├── htu2x_STM32.c
│   └── README.md
├── LM35/
│   ├── lm35_STM32.h
│   ├── lm35_STM32.c
│   └── README.md
├── HCSR04/
│   ├── hcsr04_STM32.h
│   ├── hcsr04_STM32.c
│   └── README.md
└── README.md  <-- (Este archivo índice)

```

## 📜 Licencia y Soporte Académico

* **Autor:** Mamani Flores Carlos (UTN FRT)

* **Materia:** Técnicas Digitales 2 — Universidad Tecnológica Nacional (FRT)

* **Licencia:** **MIT** (Consúltese el archivo [LICENSE](../../LICENSE) en la raíz del repositorio).
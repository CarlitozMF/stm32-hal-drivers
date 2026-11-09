# Categoría: Módulos

Esta carpeta contiene la colección de drivers y componentes de extensión para **comunicación inalámbrica, registro de datos (datalogging), temporización en tiempo real e interfaces de entrada digital** en microcontroladores STM32 utilizando la capa **ST HAL**.

Todos los drivers están desarrollados bajo una **arquitectura no bloqueante**, empleando máquinas de estados finitos (FSM), interrupciones de hardware (`EXTI` / `USART IT`) y buses serie estándar (`I2C`, `SPI`, `UART`).


## 📦 Drivers Disponibles en esta Categoría

| Driver / Módulo | Interfaz / Periférico | Dependencia `Utils` | Versión | Estado | Descripción Principal | 
| ----- | ----- | ----- | ----- | ----- | ----- | 
| [**`Bluetooth`**](Bluetooth/) | UART (HC-05 / HC-06 / HM-10) | No | `v2.0` | **Estable** | Comunicación inalámbrica serie con buffer circular por interrupciones, parsing de tramas no bloqueante y reconexión automática. | 
| [**`Encoder-KY040`**](Encoder-KY040/) | GPIO (EXTI / Timer Encoder) | No | `v1.3` | **Estable** | Decodificación de encoders rotativos en cuadratura con algoritmo antirrebote por software y gestión de pulsador integrado. | 
| [**`RTC-DS3231`**](RTC-DS3231/) | I2C | No | `v1.2` | **Estable** | Reloj en tiempo real de alta precisión con oscilador TCXO, detección de falla de batería (CH flag) y alarmas de hardware. | 
| [**`SPI-MicroSD`**](SPI-MicroSD/) | SPI + Stack FatFs | No | `v1.2` | **Estable** | Interfaz de almacenamiento masivo en tarjetas MicroSD mediante comandos SPI directos y compatibilidad con sistema de archivos FAT/FatFs. | 


## 🚀 Estructura de las Carpetas

Cada módulo de expansión cuenta con su propio subdirectorio autocontenido:

```
Modulos/
├── Bluetooth/
│   ├── bluetooth_STM32.h
│   ├── bluetooth_STM32.c
│   └── README.md
├── Encoder-KY040/
│   ├── encoder_ky040_STM32.h
│   ├── encoder_ky040_STM32.c
│   └── README.md
├── RTC-DS3231/
│   ├── rtc_ds3231_STM32.h
│   ├── rtc_ds3231_STM32.c
│   └── README.md
├── SPI-MicroSD/
│   ├── spi_microsd_STM32.h
│   ├── spi_microsd_STM32.c
│   └── README.md
└── README.md  <-- (Este archivo índice)

```

## 📜 Licencia y Soporte Académico

* **Autor:** Mamani Flores Carlos (UTN FRT)

* **Materia:** Técnicas Digitales 2 — Universidad Tecnológica Nacional (FRT)

* **Licencia:** **MIT** (Consúltese el archivo [LICENSE](../../LICENSE) en la raíz del repositorio).
# Categoría: Periféricos-BSP (Board Support Package)

Esta carpeta contiene la colección de **abstracciones de periféricos internos del microcontrolador y drivers de soporte de placa (BSP)** para la familia STM32 bajo la capa **ST HAL**.

El objetivo de esta categoría es proveer módulos de comunicación y control de hardware interno de alto rendimiento, optimizados mediante **interrupciones (IT), acceso directo a memoria (DMA) y buffers circulares (Ring Buffers)** para garantizar transmisiones asíncronas de cero bloqueo.

## 📦 Drivers Disponibles en esta Categoría

| Driver / Módulo | Periférico Interno | Dependencia `Utils` | Versión | Estado | Descripción Principal | 
| ----- | ----- | ----- | ----- | ----- | ----- | 
| [**`UART`**](UART/) | USART / UART (Hardware) | No | `v1.4` | **Estable** | Abstracción BSP para comunicación serie asíncrona con Ring Buffers en RX/TX, callbacks sin bloqueo, redirección de `printf` y parsing por carácter/trama. | 


## 🚀 Estructura del Directorio

```
Perifericos-BSP/
├── UART
│   ├── uart_STM32.c
│   ├── uart_STM32.h
│   └── README.md
└── README.md  <-- (Este archivo índice)

```

## 📜 Licencia y Soporte Académico

* **Autor:** Mamani Flores Carlos (UTN FRT)

* **Materia:** Técnicas Digitales 2 — Universidad Tecnológica Nacional (FRT)

* **Licencia:** **MIT** (Consúltese el archivo [LICENSE](../LICENSE) en la raíz del repositorio).
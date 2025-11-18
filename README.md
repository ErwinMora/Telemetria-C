# Servidor Backend para Recepción de Datos desde ESP32
## Descripción del Proyecto
Este proyecto implementa un servidor backend encargado de recibir datos enviados por un dispositivo ESP32, incluyendo:

- Telemetría
- Temperatura
- Sensores adicionales disponibles
- Timestamp del momento del envío

El servidor almacena todos los datos recibidos en una base de datos MongoDB.
Además, se debe garantizar que existan al menos 1200 registros almacenados, generados por el ESP32 con un intervalo aproximado de 3 minutos por dato.

## Tecnologías Utilizadas

- Node.js — Entorno de ejecución
- Express.js — Framework para creación de APIs
- MongoDB — Base de datos NoSQL
- Mongoose — ODM para modelado de datos
- ESP32 (C/C++) — Envío de datos mediante WiFi

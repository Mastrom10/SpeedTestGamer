# AGENTS.md - SpeedTestGamer

## Scope
- Este repo contiene solo componentes C++ (`src/server.cpp`, `src/client.cpp`) y `Makefile`.
- No hay proyecto Android en este repositorio.

## Build y verificacion
- Ejecutar `make` cuando se modifique código C++.
- Validar que se generen `dist/server` y `dist/client`.
- Si no hay tests automatizados para el cambio, al menos verificar compilación limpia sin errores.

## Convenciones
- Mantener compatibilidad Linux en arquitecturas `x86_64` y `arm64`.
- Evitar dependencias no estándar de C++.
- Mantener protocolos de red documentados en `README.md` cuando cambien mensajes o versiones.

## Colaboracion con HomeScan2
- `HomeScan2` consume este backend por red (UDP v2 y TCP throughput).
- Si cambia el protocolo, actualizar:
  - `README.md` de este repo.
  - documentación/contratos de SpeedTest en `HomeScan2`.

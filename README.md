# SpeedTestGamer (C++ Client + Server)

Repositorio C++ para pruebas de red orientadas a HomeScan:

- `UDP v2` para calidad gamer: latencia por tick, on-time/late, pérdida y out-of-order.
- `TCP throughput` para ancho de banda (download/upload) con framing binario.
- Binarios `server` y `client` en `dist/`.
- Logs JSONL simples por sesión.

Estado actual (2026-03-16):
- Este repo ya no incluye proyecto Android.
- La app Android vive en `HomeScan2` y consume este backend vía red.

## Build

```sh
make -B
```

Genera:

- `dist/server`
- `dist/client` (cliente CLI C++ para pruebas locales)

## Run

```sh
./dist/server -p 9000 -t 0 --max-sessions 50 --log-dir . --log-level summary
```

Cliente CLI de ejemplo (UDP):

```sh
./dist/client -h 127.0.0.1 -p 9000 -n 300 -s 256 -t 16
```

Opciones:

- `-p, --port`: puerto UDP/TCP (default `9000`)
- `-t, --tick`: override global de tick UDP (si `>0`)
- `--max-sessions`: sesiones concurrentes máximas (default `50`)
- `--log-dir`: directorio de logs (default `.`)
- `--log-level`: `summary|events|verbose` (default `summary`)

## Logs

Se rota por día en:

- `server_YYYYMMDD.jsonl`

Eventos principales:

- `server_start`
- `session_start`
- `session_end`
- `session_error`
- `session_rejected`
- `server_stats` (cada 10s)

## Protocolos

### UDP v2 (HomeScan)

Tipos de mensaje:

- `SYNC_REQ` / `SYNC_RESP`
- `TEST_START_REQ` / `TEST_START_ACK`
- `UP_TICK` / `DOWN_TICK`
- `TEST_END_REQ` / `TEST_END_SUMMARY`

Reglas:

- Header little-endian de 12 bytes (`type`, `version`, `sessionId`, `seq`)
- Versión `2`
- Payload max por datagrama: `1024` bytes

### TCP Throughput

Framing binario little-endian:

- Header de 16 bytes: `magic`, `version`, `type`, `sessionId`, `length`
- Versión `1`
- Mensajes:
  - `START_REQ`
  - `START_ACK`
  - `DATA`
  - `STOP`
  - `RESULT`
  - `BUSY`

Flujo:

- Download: cliente inicia -> servidor envía `DATA` por duración -> `RESULT`
- Upload: cliente inicia -> cliente envía `DATA` -> `STOP` -> servidor devuelve `RESULT`

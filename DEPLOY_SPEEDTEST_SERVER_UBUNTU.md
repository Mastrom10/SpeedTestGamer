# Deploy del servidor C++ de SpeedTest en Ubuntu (runbook replicable)

Fecha de referencia: 2026-03-17

Este documento deja registrado, paso a paso, el despliegue real hecho sobre un Ubuntu remoto para que se pueda repetir en un entorno productivo.

## 1) Datos del despliegue realizado

- Host origen (cliente VPN + build): `u606098@<equipo-local>`
- Host destino Ubuntu: `10.254.145.100`
- Usuario remoto: `nmastromarino`
- Repositorio local: `/home/u606098/Codigo/SpeedTestGamer`
- Binario desplegado: `dist/server`
- Ruta final del binario en servidor: `/home/nmastromarino/speedtest/server`
- Servicio systemd: `speedtest-server.service`

Nota de seguridad: no guardar contraseñas en texto plano en documentación productiva. Usar llaves SSH o vault.

## 2) Pre-check de conectividad por VPN

Antes de desplegar, validar que el destino salga por túnel VPN.

Comando:

```bash
ip route get 10.254.145.100
```

En el caso real inicialmente salía por Wi-Fi (`wlp0s20f3`) y no por `tun0`, porque faltaba ruta para `10.254.145.0/24`.

Ruta agregada (temporal) para el host destino:

```bash
sudo ip route add 10.254.145.100/32 via 172.20.247.1 dev tun0 metric 50
```

Validaciones usadas:

```bash
ping -c 2 -W 2 10.254.145.100
nc -zv -w 3 10.254.145.100 22
```

## 3) Build local del servidor C++

Desde `SpeedTestGamer`:

```bash
cd /home/u606098/Codigo/SpeedTestGamer
make -B
```

Resultado esperado:

- `dist/server`
- `dist/client`

## 4) Subida del binario al servidor remoto

```bash
scp -o StrictHostKeyChecking=accept-new /home/u606098/Codigo/SpeedTestGamer/dist/server nmastromarino@10.254.145.100:/home/nmastromarino/
```

## 5) Preparar estructura en el servidor

Entrar por SSH:

```bash
ssh -tt nmastromarino@10.254.145.100
```

Crear estructura y mover binario:

```bash
mkdir -p /home/nmastromarino/speedtest/logs
mv -f /home/nmastromarino/server /home/nmastromarino/speedtest/server
chmod 755 /home/nmastromarino/speedtest/server
```

Se dejó además un symlink de compatibilidad:

```bash
ln -sfn /home/nmastromarino/speedtest/server /home/nmastromarino/server
```

## 6) Crear servicio systemd

Archivo: `/etc/systemd/system/speedtest-server.service`

Contenido final aplicado:

```ini
[Unit]
Description=SpeedTestGamer C++ Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=nmastromarino
Group=nmastromarino
WorkingDirectory=/home/nmastromarino/speedtest
ExecStart=/home/nmastromarino/speedtest/server -p 9010 -t 0 --max-sessions 50 --log-dir /home/nmastromarino/speedtest/logs --log-level summary
Restart=always
RestartSec=3
KillSignal=SIGINT
NoNewPrivileges=true
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
```

Aplicar y arrancar:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now speedtest-server.service
```

## 7) Incidencia encontrada y resolución

Al intentar usar puerto `9000`, el servicio entraba en restart loop.

Error observado en logs:

```text
bind TCP: Address already in use
```

Diagnóstico:

```bash
sudo ss -luntp | grep ':9000'
sudo docker ps --format 'table {{.ID}}\t{{.Names}}\t{{.Image}}\t{{.Ports}}'
```

Resultado real: `portainer` (docker-proxy) ocupando `0.0.0.0:9000`.

Resolución aplicada: cambiar SpeedTest a `-p 9010` en `ExecStart` para evitar colisión.

## 8) Verificación de estado final

Comandos usados:

```bash
systemctl is-enabled speedtest-server.service
systemctl is-active speedtest-server.service
systemctl status speedtest-server.service --no-pager -l
sudo ss -luntp | grep ':9010'
ls -l /home/nmastromarino/speedtest/logs
```

Estado final del despliegue:

- Servicio: `enabled` y `active (running)`
- Puerto operativo: `9010` (TCP y UDP)
- Bind observado: `10.254.145.100:9010` y otras interfaces locales
- Log creado: `/home/nmastromarino/speedtest/logs/server_20260317.jsonl`

## 9) Checklist para productivo

1. Confirmar conectividad VPN y rutas al host destino.
2. Confirmar que el puerto elegido no esté ocupado (`ss -luntp`).
3. Compilar con `make -B` y subir `dist/server`.
4. Crear estructura `/home/<user>/speedtest` y permisos.
5. Crear `speedtest-server.service` con `Restart=always`.
6. Ejecutar `systemctl daemon-reload` + `enable --now`.
7. Validar `is-enabled`, `is-active`, `status` y sockets en escucha.
8. Validar creación de logs JSONL.

## 10) Comandos de operación diaria

Ver estado:

```bash
sudo systemctl status speedtest-server.service --no-pager -l
```

Reiniciar:

```bash
sudo systemctl restart speedtest-server.service
```

Parar/iniciar:

```bash
sudo systemctl stop speedtest-server.service
sudo systemctl start speedtest-server.service
```

Ver logs del servicio:

```bash
journalctl -u speedtest-server.service -f
```

Ver logs de aplicación:

```bash
tail -f /home/nmastromarino/speedtest/logs/server_$(date +%Y%m%d).jsonl
```

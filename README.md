# SpeedTestGamer

This project provides a simple UDP based "speedtest" tool intended for gaming scenarios. It consists of a server and a client written in C++. The client requests a number of packets and the server sends them back at the configured tick interval while each packet carries a timestamp to measure latency.

## Building

Use the provided `Makefile` to build both the server and client. Binaries are placed in `dist/`:

```sh
make
```

Cross compiling for other architectures can be done by overriding the `CXX` variable, for example:

```sh
make CXX=aarch64-linux-gnu-g++
```

## Usage

Start the server (default port 9000). You can optionally set the tick interval in milliseconds:

```sh
./dist/server [-p port] [-t tick_ms]
```

Run the client specifying server IP and port. An optional parameter controls how many packets will be sent by the server. Results are also written to a log file:

```sh
./dist/client -a <server_ip> -p <server_port> [-n count]
```

The client displays the last packets in the terminal while logging all measurements to a timestamped file.

## Packet formats

* **Request** (client ➜ server):
  * `uint32_t count` – number of `Packet` messages the client wants to receive.
  * `uint64_t client_time_ns` – local time when sending the request.
  * `uint32_t client_id` – identifier of the client session.
  * `uint32_t payload_size` – desired payload size in bytes.
  * `uint32_t tick_request_ms` – desired tick interval.
* **Sync** (server ➜ client):
  * `uint64_t server_time_ns` – server timestamp when the request was received.
  * `uint32_t server_id` – identifier of the server.
  * `uint32_t tick_ms` – tick interval that will actually be used. Used to estimate clock offset.
* **Packet** (server ➜ client):
  * `uint32_t seq` – sequence number.
  * `uint64_t timestamp_ns` – server send timestamp in nanoseconds.
  * `uint32_t server_id` – same identifier as in `Sync`.
  * `uint32_t tick_ms` – tick interval used.
  * payload – byte array of length `payload_size`.

## Latency calculation with unsynchronized clocks

When the client sends the `Request` it notes the local send time `t_send`. The server immediately replies with a `Sync` message containing its own timestamp `t_srv`. Upon reception the client records `t_recv` and computes the clock offset as:

```
offset_ns = t_srv - (t_send + (t_recv - t_send)/2)
```

This offset is subtracted from the timestamps of every `Packet` before computing latency. The adjusted latency becomes:

```
latency_ms = (client_now - (packet.timestamp_ns - offset_ns)) / 1e6
```

Assuming roughly symmetric network delay, this compensates for differences between the client and server clocks so that the measured latency reflects only the network delay.

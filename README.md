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

Start the server (default port 9000). You can optionally set the tick interval in milliseconds. On startup the server lists all local IP addresses:

```sh
./dist/server [-p port] [-t tick_ms]
```

Run the client specifying server IP and port. An optional parameter controls how many packets will be sent by the server. Results are also written to a log file:

```sh
./dist/client -a <server_ip> -p <server_port> [-n count]
```

The client displays the last packets in the terminal while logging all measurements to a timestamped file.

### Android client

An Android front end is provided in `android-client`. It mirrors the C++ client functionality but with a simple user interface. All parameters are entered manually and each field is labeled for clarity. Results are drawn on a bar chart that updates as packets arrive.

## Packet formats

* **Request** (client ➜ server):
  * `uint32_t count` – number of `Packet` messages the client wants to receive.
  * `uint64_t client_time_ns` – local time when sending the request.
  * `uint32_t client_id` – identifier of the client session.
  * `uint32_t payload_size` – desired payload size in bytes.
  * `uint32_t tick_request_ms` – desired tick interval.
* **SyncRequest** (client ➜ server):
  * `uint64_t client_time_ns` – timestamp when the request was sent.
* **SyncResponse** (server ➜ client):
  * `uint64_t recv_time_ns` – time the request was received on the server.
  * `uint64_t send_time_ns` – timestamp just before the response was sent.
* **Packet** (server ➜ client):
  * `uint32_t seq` – sequence number.
  * `uint64_t timestamp_ns` – server send timestamp in nanoseconds.
  * `uint32_t server_id` – same identifier as in `Sync`.
  * `uint32_t tick_ms` – tick interval used.
  * payload – byte array of length `payload_size`.

## Latency calculation with unsynchronized clocks

During startup the client exchanges synchronization messages with the server. For
each pair the client notes the send time `t0` and on reception obtains `t1` and
`t2` from the server together with the local receive time `t3`. Round trip time
and clock offset are calculated as:

```
RTT = (t3 - t0) - (t2 - t1)
offset_ns = ((t1 - t0) + (t2 - t3)) / 2
```

The offset corresponding to the smallest RTT is kept and used to adjust
timestamps of all data packets before computing latency:

```
latency_ms = (client_now - (packet.timestamp_ns - offset_ns)) / 1e6
```

Assuming roughly symmetric network delay, this compensates for differences between the client and server clocks so that the measured latency reflects only the network delay. The client continues to send a new synchronization message every five seconds and updates the offset whenever a lower RTT is observed.

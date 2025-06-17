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

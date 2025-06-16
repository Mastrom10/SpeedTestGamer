# SpeedTestGamer

This project provides a simple UDP based "speedtest" tool intended for gaming scenarios. It consists of a server and a client written in C++. The client sends packets at a configurable rate and size and measures round-trip latency for each packet individually.

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

Run the client specifying server IP and port. Additional parameters control packet count and size (in bytes). Results are also written to a log file:

```sh
./dist/client -a <server_ip> -p <server_port> [-n count] [-s size]
```

The client displays the last packets in the terminal while logging all measurements to a timestamped file.

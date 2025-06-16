# SpeedTestGamer

This project provides a simple UDP based "speedtest" tool intended for gaming scenarios. It consists of a server and a client written in C++. The client sends packets at a configurable rate and size and measures round-trip latency for each packet individually.

## Building

Use the provided `Makefile` to build both the server and client:

```sh
make
```

Cross compiling for other architectures can be done by overriding the `CXX` variable, for example:

```sh
make CXX=aarch64-linux-gnu-g++
```

## Usage

Start the server (default port 9000):

```sh
./server [port]
```

Run the client specifying server IP, port, and optional packet count and packet size:

```sh
./client <server_ip> <server_port> [count] [size]
```

Each packet will be echoed by the server and the client will print the latency in milliseconds.

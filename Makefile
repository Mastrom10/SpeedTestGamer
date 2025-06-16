CXX?=g++
CXXFLAGS?=-O2 -std=c++17

all: server client

server: src/server.cpp
	$(CXX) $(CXXFLAGS) -o server src/server.cpp

client: src/client.cpp
	$(CXX) $(CXXFLAGS) -o client src/client.cpp

clean:
	rm -f server client

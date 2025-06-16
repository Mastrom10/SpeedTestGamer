CXX?=g++
CXXFLAGS?=-O2 -std=c++17

DIST?=dist

all: $(DIST)/server $(DIST)/client

$(DIST)/server: src/server.cpp
	mkdir -p $(DIST)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(DIST)/client: src/client.cpp
	mkdir -p $(DIST)
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(DIST)/server $(DIST)/client

.PHONY: all clean

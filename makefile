CXX := g++
CXXFLAGS := -g -O0 -Wall -Wextra -std=c++17
STTNET_SRC := /home/stephen/STTNet/src/sttnet.cpp

SERVER_LDFLAGS := -L/home/stephen
SERVER_LDLIBS := -lmysqlclient -ljsoncpp -lssl -lcrypto -lpthread -ldl

MAIN_LDLIBS := -ljsoncpp -lssl -lcrypto -lpthread

all: server

server: server.cpp $(STTNET_SRC)
	$(CXX) $(CXXFLAGS) -o $@ server.cpp $(STTNET_SRC) $(SERVER_LDFLAGS) $(SERVER_LDLIBS)

clean:
	rm -f server


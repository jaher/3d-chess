CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
CXXFLAGS += $(shell pkg-config --cflags gtk+-3.0 epoxy)
CXXFLAGS += $(shell pkg-config --cflags libcurl)
LDFLAGS  := $(shell pkg-config --libs gtk+-3.0 epoxy libcurl) -lm

TARGET   := chess
SRCS     := main.cpp chess_types.cpp chess_rules.cpp game_state.cpp board_renderer.cpp \
            challenge.cpp linalg.cpp stl_model.cpp shader.cpp ai_player.cpp
OBJS     := $(SRCS:.cpp=.o)
HEADERS  := chess_types.h chess_rules.h game_state.h board_renderer.h challenge.h \
            linalg.h shader.h stl_model.h ai_player.h

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean

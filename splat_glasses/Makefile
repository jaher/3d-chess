# splat_glasses — server-side Gaussian-splat renderer for the Ray-Ban Display.
# Reuses the repo's SPZ decoder (../splat.cpp) and gunzip (../compression.cpp)
# unmodified; everything new lives in this directory.

CXX      := g++
CXXFLAGS := -std=c++17 -O2 -fopenmp -Wall -Wextra -I..
LDLIBS   := -lz

splat_render: renderer.cpp ../splat.cpp ../compression.cpp \
              ../splat.h ../compression.h third_party/stb_image_write.h
	$(CXX) $(CXXFLAGS) -o $@ renderer.cpp ../splat.cpp ../compression.cpp $(LDLIBS)

.PHONY: clean test
clean:
	rm -f splat_render

test: splat_render
	bash smoke_test.sh

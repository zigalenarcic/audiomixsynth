
CFLAGS = `pkg-config --cflags freetype2` -g
OBJS = audio.o main.o

audiomixsynth: $(OBJS)
	gcc -o $@ $(OBJS) -g `pkg-config --libs freetype2 opengl jack` -lglfw -lm

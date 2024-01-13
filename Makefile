
PROGRAM_NAME=audiostudio

.PHONY: $(PROGRAM_NAME)
$(PROGRAM_NAME):
	gcc -o $@ audiostudio.c -g -lm -pthread `pkg-config --cflags --libs freetype2 opengl glfw3 jack`

.PHONY: clean
clean:
	rm -rf $(PROGRAM_NAME)


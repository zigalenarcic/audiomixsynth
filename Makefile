
PROGRAM_NAME=audiostudio

.PHONY: $(PROGRAM_NAME)
$(PROGRAM_NAME):
	$(CC) -o $@ audiostudio.c -g -lm -pthread `pkg-config --cflags --libs freetype2 gl glfw3 jack`

.PHONY: clean
clean:
	rm -rf $(PROGRAM_NAME)


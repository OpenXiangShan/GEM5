#include <stdint.h>
#include <stdio.h>

enum
{
    LINE_SIZE = 64,
    NUM_LINES = 2048,
    STORE_OFFSET = 7
};

static volatile uint8_t data[NUM_LINES * LINE_SIZE]
    __attribute__((aligned(LINE_SIZE)));

static unsigned
line_index(unsigned i)
{
    return (i * 2053U) & (NUM_LINES - 1);
}

int
main(void)
{
    for (unsigned i = 0; i < NUM_LINES; ++i) {
        const unsigned line = line_index(i);
        data[line * LINE_SIZE + STORE_OFFSET] = (uint8_t)(line + 1);
    }

    for (unsigned i = 0; i < NUM_LINES; ++i) {
        const unsigned line = line_index(i);
        const uint8_t expected = (uint8_t)(line + 1);
        if (data[line * LINE_SIZE + STORE_OFFSET] != expected ||
            data[line * LINE_SIZE + STORE_OFFSET + 1] != 0) {
            return 1;
        }
    }

    puts("partial-store: PASS");
    return 0;
}

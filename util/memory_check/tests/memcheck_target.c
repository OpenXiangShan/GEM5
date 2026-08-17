#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *reachable_memory;

static void __attribute__((noinline))
leak_memory(void)
{
    void *memory = malloc(32);

    if (memory == NULL)
        exit(4);
    memset(memory, 0x5a, 32);
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "missing test mode\n");
        return 2;
    }

    if (strcmp(argv[1], "success") == 0) {
        if (argc != 3 || strcmp(argv[2], "argument with spaces") != 0) {
            fprintf(stderr, "arguments were not preserved\n");
            return 3;
        }
        return 0;
    }

    if (strcmp(argv[1], "invalid-read") == 0) {
        volatile char *memory = malloc(1);
        volatile char value;

        if (memory == NULL)
            return 4;
        free((void *)memory);
        value = memory[0];
        (void)value;
        return 0;
    }

    if (strcmp(argv[1], "invalid-write") == 0) {
        volatile char *memory = malloc(1);

        if (memory == NULL)
            return 4;
        memory[1] = 1;
        free((void *)memory);
        return 0;
    }

    if (strcmp(argv[1], "definite-leak") == 0) {
        leak_memory();
        return 0;
    }

    if (strcmp(argv[1], "reachable-leak") == 0) {
        reachable_memory = malloc(16);
        if (reachable_memory == NULL)
            return 4;
        return 0;
    }

    if (strcmp(argv[1], "abnormal-exit") == 0)
        return 42;

    fprintf(stderr, "unknown test mode: %s\n", argv[1]);
    return 2;
}

// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
    char line[64] = {0};
    const char *env = getenv("SE_SMOKE");

    if (argc != 2) {
        fprintf(stderr, "expected one input path, got %d arguments\n", argc - 1);
        return 2;
    }

    FILE *input = fopen(argv[1], "r");
    if (input == NULL || fgets(line, sizeof(line), input) == NULL) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return 3;
    }
    fclose(input);
    line[strcspn(line, "\r\n")] = '\0';

    if (env == NULL || strcmp(env, "works") != 0) {
        fprintf(stderr, "unexpected SE_SMOKE value\n");
        return 4;
    }
    if (strcmp(line, "read-ok") != 0) {
        fprintf(stderr, "unexpected input contents: %s\n", line);
        return 5;
    }

    printf("SE smoke passed: argc=%d env=%s file=%s\n", argc, env, line);
    return 0;
}

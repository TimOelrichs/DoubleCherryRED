#include <stdio.h>
#include <stdlib.h>
#include "libretro.h"

char* read_file_to_buffer(const char* filename, size_t* file_size) {
    FILE* file = fopen(filename, "rb");
    if (file == NULL) {
        printf("Failed to open file: %s\n", filename);
        return NULL;
    }

    // Determine the size of the file
    fseek(file, 0, SEEK_END);
    *file_size = ftell(file);
    rewind(file);

    // Allocate memory for the buffer to hold the file contents
    char* buffer = (char*)malloc(*file_size);
    if (buffer == NULL) {
        printf("Memory allocation failed\n");
        fclose(file);
        return NULL;
    }

    // Read the file into the buffer
    size_t bytes_read = fread(buffer, 1, *file_size, file);

    // Close the file
    fclose(file);

    if (bytes_read != *file_size) {
        printf("Failed to read the entire file\n");
        free(buffer);
        return NULL;
    }

    return buffer;
}


char* read_savestate(const char* filename, size_t* state_size) {
    FILE* file = fopen(filename, "rb");
    if (file == NULL) {
        printf("Failed to open file: %s\n", filename);
        return NULL;
    }

    // Determine the size of the savestate
    fseek(file, 0, SEEK_END);
    *state_size = ftell(file);
    rewind(file);

    // Allocate memory for the buffer to hold the savestate contents
    char* buffer = (char*)malloc(*state_size);
    if (buffer == NULL) {
        printf("Memory allocation failed\n");
        fclose(file);
        return NULL;
    }

    // Read the savestate into the buffer
    size_t bytes_read = fread(buffer, 1, *state_size, file);

    // Close the file
    fclose(file);

    if (bytes_read != *state_size) {
        printf("Failed to read the entire savestate\n");
        free(buffer);
        return NULL;
    }

    return buffer;
}


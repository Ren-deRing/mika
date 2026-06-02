#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHUNK_SIZE (5 * 1024 * 1024) // 5 MB
#define NUM_CHUNKS 10                // 50 MB total

int main(int argc, char *argv[], char *envp[]) {
    printf("\n");
    printf("==================================================\n");
    printf("[STRESS] HEHE LET'S BULLYING ALLOCATOR!\n");
    printf("[STRESS] TARGET: Allocate %d MB in %d chunks of %d MB\n", 
           (CHUNK_SIZE * NUM_CHUNKS) / (1024 * 1024), NUM_CHUNKS, CHUNK_SIZE / (1024 * 1024));
    printf("==================================================\n");

    void *chunks[NUM_CHUNKS];

    for (int i = 0; i < NUM_CHUNKS; i++) {
        printf("[STRESS] Chunk %2d: Allocating %d bytes... ", i + 1, CHUNK_SIZE);
        chunks[i] = malloc(CHUNK_SIZE);
        if (!chunks[i]) {
            printf("FAILED! malloc returned NULL.\n");
            return 1;
        }
        printf("Allocated at %p. Writing to memory... ", chunks[i]);

        char *ptr = (char *)chunks[i];
        for (size_t j = 0; j < CHUNK_SIZE; j += 4096) {
            ptr[j] = (char)(i + (j % 256));
        }
        printf("Touch OK. Verifying... ");

        for (size_t j = 0; j < CHUNK_SIZE; j += 4096) {
            if (ptr[j] != (char)(i + (j % 256))) {
                printf("VERIFICATION FAILED at offset %zu!\n", j);
                return 1;
            }
        }
        printf("Verification SUCCESS.\n");
    }

    printf("[STRESS] Phase 1 complete\n");

    printf("==================================================\n");
    printf("[STRESS] Phase 2: Freeing all chunks to verify page release...\n");
    for (int i = 0; i < NUM_CHUNKS; i++) {
        printf("[STRESS] Freeing chunk %d at %p...\n", i + 1, chunks[i]);
        free(chunks[i]);
    }
    printf("[STRESS] Phase 2 complete.\n");

    printf("==================================================\n");
    printf("[STRESS] Phase 3: Re-allocating memory to verify page recycling...\n");
    for (int i = 0; i < NUM_CHUNKS; i++) {
        printf("[STRESS] Chunk %2d: Re-allocating... ", i + 1);
        chunks[i] = malloc(CHUNK_SIZE);
        if (!chunks[i]) {
            printf("FAILED!\n");
            return 1;
        }
        printf("Allocated at %p. Touching... ", chunks[i]);
        char *ptr = (char *)chunks[i];
        for (size_t j = 0; j < CHUNK_SIZE; j += 4096) {
            ptr[j] = 0xAA;
        }
        printf("OK.\n");
    }

    for (int i = 0; i < NUM_CHUNKS; i++) {
        free(chunks[i]);
    }

    printf("==================================================\n");
    printf("[STRESS] WOW GOOD BOY ALLOCATOR\n");
    printf("[STRESS] BYEBYE SEE YA\n");
    printf("==================================================\n");
    printf("\n");

    return 0xAB;
}

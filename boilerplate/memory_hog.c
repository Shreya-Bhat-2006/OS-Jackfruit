/*
 * memory_hog.c - Memory pressure workload for soft / hard limit testing.
 * 
 * FIXED: This version GUARANTEES RSS increases and stays in memory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    // Default: allocate 10MB every second, up to 200MB total
    size_t chunk_mb = (argc > 1) ? atoi(argv[1]) : 10;
    size_t target_mb = (argc > 2) ? atoi(argv[2]) : 200;
    int sleep_sec = (argc > 3) ? atoi(argv[3]) : 1;
    
    size_t chunk_bytes = chunk_mb * 1024 * 1024;
    size_t total_bytes = 0;
    char **allocated_memory = NULL;
    int count = 0;
    int max_allocations = target_mb / chunk_mb;
    
    printf("Starting memory hog: allocating %zu MB every %d second(s), target %zu MB total\n", 
           chunk_mb, sleep_sec, target_mb);
    fflush(stdout);
    
    // Allocate memory in chunks and KEEP it
    allocated_memory = malloc(sizeof(char*) * max_allocations);
    if (!allocated_memory) {
        printf("Failed to allocate tracking array\n");
        return 1;
    }
    
    while (total_bytes < target_mb * 1024 * 1024) {
        char *mem = malloc(chunk_bytes);
        if (!mem) {
            printf("malloc failed after %d allocations, total=%zu MB\n", 
                   count, total_bytes / (1024 * 1024));
            break;
        }
        
        // CRITICAL: Touch every page to force physical allocation
        memset(mem, 0xFF, chunk_bytes);
        
        allocated_memory[count] = mem;
        total_bytes += chunk_bytes;
        count++;
        
        printf("allocation=%d chunk=%zu MB total=%zu MB\n", 
               count, chunk_mb, total_bytes / (1024 * 1024));
        fflush(stdout);
        
        sleep(sleep_sec);
    }
    
    printf("Memory hog allocated %zu MB total. Keeping memory allocated...\n", 
           total_bytes / (1024 * 1024));
    fflush(stdout);
    
    // Keep memory allocated forever (or until killed)
    while (1) {
        // Periodically touch memory to keep it resident
        for (int i = 0; i < count; i++) {
            if (allocated_memory[i]) {
                memset(allocated_memory[i], 0xAA, chunk_mb * 1024 * 1024);
            }
        }
        printf("Memory hog still alive, holding %zu MB\n", total_bytes / (1024 * 1024));
        fflush(stdout);
        sleep(5);
    }
    
    return 0;
}
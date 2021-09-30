
#ifndef MEMORY_PAHOMQTT_H
#define MEMORY_PAHOMQTT_H

#include <stdio.h>
#include <sys/types.h>

#if defined(WITH_MEMORY_TRACKING) && defined(WITH_BROKER)
#  if defined(__APPLE__) || defined(__FreeBSD__) || defined(__GLIBC__)
#    define REAL_WITH_MEMORY_TRACKING
#  endif
#endif

void *pahomqtt__calloc(size_t nmemb, size_t size);
void pahomqtt__free(void *mem);
void *pahomqtt__malloc(size_t size);
#ifdef REAL_WITH_MEMORY_TRACKING
unsigned long pahomqtt__memory_used(void);
unsigned long pahomqtt__max_memory_used(void);
#endif
void *pahomqtt__realloc(void *ptr, size_t size);
char *pahomqtt__strdup(const char *s);

#ifdef WITH_BROKER
void memory__set_limit(size_t lim);
#endif

#endif

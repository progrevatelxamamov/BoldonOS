#ifndef STRING_H
#define STRING_H

#include <stddef.h>
#include <memory.h>

size_t strlen(const char *s);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strcat(char *dest, const char *src);
const char* strchr(const char* str, int c);
char *strstr(const char *haystack, const char *needle);

#endif
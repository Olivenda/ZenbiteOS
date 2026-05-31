#ifndef ZENBITE_TYPES_H
#define ZENBITE_TYPES_H

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;

typedef signed char         i8;
typedef signed short        i16;
typedef signed int          i32;
typedef signed long long    i64;

typedef u32                 size_t;
typedef i32                 ssize_t;
typedef u32                 uintptr_t;

#define NULL ((void *)0)

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#endif

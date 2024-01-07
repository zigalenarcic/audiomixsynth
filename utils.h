#ifndef __UTILS_H
#define __UTILS_H

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#define container_of(ptr, type, member) ({  \
    const typeof( ((type *)0)->member ) *__member = (ptr); \
    (type *)( (char *)__member - offsetof(type, member));})

#define FREE_IF_NOT_NULL(x) if ((x)) free((x))

#endif // __UTILS_H

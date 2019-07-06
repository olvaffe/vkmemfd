#ifndef UDMABUF_H
#define UDMABUF_H

#include <stddef.h>

int udmabuf_init(void);
int udmabuf_create(int fd, int memfd, size_t offset, size_t size);

#endif /* UDMABUF_H */

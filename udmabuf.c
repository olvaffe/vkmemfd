#include "udmabuf.h"

#include <stdint.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#define UDMABUF_FLAGS_CLOEXEC 0x01
struct udmabuf_create {
	uint32_t memfd;
	uint32_t flags;
	uint64_t offset;
	uint64_t size;
};

#define UDMABUF_CREATE _IOW('u', 0x42, struct udmabuf_create)

int udmabuf_init(void)
{
	return open("/dev/udmabuf", O_WRONLY);
}

int udmabuf_create(int fd, int memfd, size_t offset, size_t size)
{
	struct udmabuf_create create = {
		.memfd = memfd,
		.flags = UDMABUF_FLAGS_CLOEXEC,
		.offset = offset,
		.size = size,
	};

	return ioctl(fd, UDMABUF_CREATE, &create);
}

#ifndef RENDERER_H
#define RENDERER_H

#include <stdbool.h>

int renderer(int width, int height, int output_count, int ctrl_in,
		int ctrl_out, int memfd, bool use_udmabuf);

#endif /* RENDERER_H */

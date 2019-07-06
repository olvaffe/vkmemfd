#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include "renderer.h"

struct app {
	struct {
		const char *name;
		const char *argv0;
		int width;
		int height;
		int output_count;
		size_t heap_size;
		bool is_coherent;
		bool use_udmabuf;
	} config;

	struct {
		int memfd;
		void *base;
	} heap;

	struct {
		int in;
		int out;
	} renderer;

	struct {
		xcb_connection_t *conn;
		xcb_window_t win;
		xcb_gcontext_t gc;
		size_t img_size;
	} xcb;

	/* pointers into the heap */
	struct {
		float *ubo;
		const void **outputs;
	} mems;
};

static void app_fatal(const char *msg)
{
	printf("APP-FATAL: %s\n", msg);
	abort();
}

static void app_init_heap(struct app *app)
{
	app->heap.memfd = memfd_create(app->config.name,
			MFD_CLOEXEC | MFD_ALLOW_SEALING);

	if (app->heap.memfd < 0)
		app_fatal("failed to create memfd");

	if (ftruncate(app->heap.memfd, app->config.heap_size) < 0)
		app_fatal("failed to set memfd size");

	if (fcntl(app->heap.memfd, F_ADD_SEALS, F_SEAL_SEAL |
				F_SEAL_SHRINK |
				F_SEAL_GROW) < 0)
		app_fatal("failed to seal memfd");

	app->heap.base = mmap(NULL, app->config.heap_size,
			PROT_READ | PROT_WRITE, MAP_SHARED,
			app->heap.memfd, 0);
	if (app->heap.base == MAP_FAILED)
		app_fatal("failed to map memfd");
}

static void app_init_renderer(struct app *app)
{
	int pipes[2][2];
	pid_t pid;
	int child_in;
	int child_out;

	if (pipe(pipes[0]) < 0 || pipe(pipes[1]) < 0)
		app_fatal("failed to create pipes");

	app->renderer.in = pipes[0][0];
	app->renderer.out = pipes[1][1];
	child_in = pipes[1][0];
	child_out = pipes[0][1];

	pid = fork();
	if (pid < 0)
		app_fatal("failed to fork the renderer");

	if (pid > 0) {
		close(child_in);
		close(child_out);
		return;
	}

	/* in the child now */

	close(app->renderer.in);
	close(app->renderer.out);

	int child_memfd;
	child_memfd = dup(app->heap.memfd);
	if (child_memfd < 0)
		app_fatal("failed to dup memfd");

	char child_renderer[32];
	if (snprintf(child_renderer, sizeof(child_renderer),
				"renderer-%d-%d-%d", child_in, child_out,
				child_memfd) >= sizeof(child_renderer))
		app_fatal("failed to format the renderer string");

	const char *child_argv[] = {
		app->config.argv0,
		child_renderer,
		app->config.use_udmabuf ? "udmabuf" : "memfd",
		NULL,
	};

	if (execv(app->config.argv0, (char **) child_argv) < 0)
		app_fatal("failed to exec the renderer");
}

static void app_init_xcb(struct app *app)
{
	const xcb_screen_t *screen;

	app->xcb.conn = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(app->xcb.conn))
		app_fatal("failed to connect to X");

	screen = xcb_setup_roots_iterator(xcb_get_setup(app->xcb.conn)).data;

	app->xcb.win = xcb_generate_id(app->xcb.conn);
	xcb_create_window(app->xcb.conn, XCB_COPY_FROM_PARENT, app->xcb.win,
			screen->root, 0, 0, app->config.width,
			app->config.height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			screen->root_visual, 0, NULL);

	app->xcb.gc = xcb_generate_id(app->xcb.conn);
	xcb_create_gc(app->xcb.conn, app->xcb.gc, app->xcb.win, 0, NULL);

	xcb_flush(app->xcb.conn);

	/* B8G8R8A8 */
	app->xcb.img_size = app->config.width * app->config.height * 4;
	if (app->xcb.img_size >
			xcb_get_maximum_request_length(app->xcb.conn) / 2)
		app_fatal("image size too big");
}

static void app_init_memories(struct app *app, size_t heap_skip,
		size_t ubo_size, size_t output_size)
{
	void *ptr = app->heap.base + heap_skip;

	app->mems.ubo = ptr;
	ptr += ubo_size;

	app->mems.outputs = malloc(sizeof(app->mems.outputs[0]) *
			app->config.output_count);
	if (!app->mems.outputs)
		app_fatal("failed to allocate output pointers");

	for (int i = 0; i < app->config.output_count; i++) {
		app->mems.outputs[i] = ptr;
		ptr += output_size;
	}

	if (ubo_size < sizeof(float[4]))
		app_fatal("invalid ubo size");
	if (output_size < app->xcb.img_size)
		app_fatal("invalid output size");
	if (ptr - app->heap.base > app->config.heap_size)
		app_fatal("heap size too small");
}

static uint32_t app_recv(const struct app *app)
{
	uint32_t val;
	if (read(app->renderer.in, &val, sizeof(val)) != sizeof(val))
		app_fatal("failed to receive a value");

	return val;
}

static void app_send(const struct app *app, uint32_t val)
{
	if (write(app->renderer.out, &val, sizeof(val)) != sizeof(val))
		app_fatal("failed to send a value");

}

static void app_render_frame(const struct app *app, int output,
		const float rgba[4])
{
	memcpy(app->mems.ubo, rgba, sizeof(float) * 4);

	/* The heap coherency is platform-defined.  When it is incoherent, we
	 * need to simulate vkFlushMappedMemoryRanges
	 *
	 * This needs a platform requirement and/or a Vulkan exntesion to be
	 * properly handled.
	 */
	if (!app->config.is_coherent) {
		__builtin_ia32_mfence();
		__builtin_ia32_clflush(app->mems.ubo);
	}

	app_send(app, output);
	if (app_recv(app) != output)
		app_fatal("unexpected renderer output");
}

static void app_present_frame(const struct app *app, int output)
{
	/* The heap coherency is platform-defined.  When it is incoherent, we
	 * need to simulate vkInvalidateMappedMemoryRanges.
	 *
	 * This needs a platform requirement and/or a Vulkan exntesion to be
	 * properly handled.
	 */
	if (!app->config.is_coherent) {
		const void *ptr = app->mems.outputs[output];
		const void *end = ptr + app->xcb.img_size;
		while (ptr < end) {
			__builtin_ia32_clflush(ptr);
			ptr += 64;
		}
		__builtin_ia32_mfence();
	}

	/* We could use udmabuf/DRI3/Present to avoid CPU access.  But we
	 * _want_ CPU access such that we can notice incoherency.
	 */
	xcb_put_image(app->xcb.conn, XCB_IMAGE_FORMAT_Z_PIXMAP, app->xcb.win,
			app->xcb.gc, app->config.width, app->config.height,
			0, 0, 0, 24, app->xcb.img_size,
			app->mems.outputs[output]);
	xcb_flush(app->xcb.conn);

	usleep(1000 * 1000 / 60);
}

static void app_mainloop(const struct app *app)
{
	xcb_map_window(app->xcb.conn, app->xcb.win);

	int output = 0;
	int output_inc = 1;
	int channel = 0;
	while (true) {
		if (xcb_poll_for_event(app->xcb.conn))
			app_fatal("unexpected XCB event");

		float rgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		rgba[channel] = (float) output /
			(app->config.output_count - 1);

		app_render_frame(app, output, rgba);
		app_present_frame(app, output);

		/* next value/channel */
		output += output_inc;
		if (output >= app->config.output_count)  {
			output = app->config.output_count - 1;
			output_inc = -1;
		} else if (output < 0) {
			output = 1;
			output_inc = 1;

			channel = (channel + 1) % 3;
		}
	}
}

static void app_usage(const struct app *app)
{
	printf("Usage: %s [udmabuf] [incoherent]\n", app->config.argv0);
	exit(1);
}

int main(int argc, char **argv)
{
	struct app app = {
		.config = {
			.name = "vkmemfd",
			.argv0 = argv[0],
			.width = 600,
			.height = 600,
			.output_count = 64,
			/* huge heap to demonstrate on-demand paging */
			.heap_size = (size_t) 8 * 1024 * 1024 * 1024,
			/* the memory type of the mmapped memfd is
			 * platform-defined
			 */
			.is_coherent = true,
			.use_udmabuf = false,
		},
	};
	struct {
		bool valid;
		int width;
		int height;
		int output_count;
		int ctrl_in;
		int ctrl_out;
		int memfd;
		bool use_udmabuf;
	} renderer_args = {
		.valid = false,
		.width = app.config.width,
		.height = app.config.height,
		.output_count = app.config.output_count,
		.use_udmabuf = app.config.use_udmabuf,
	};

	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "renderer-", 9)) {
			renderer_args.valid = true;
			if (sscanf(argv[i] + 9, "%d-%d-%d",
						&renderer_args.ctrl_in,
						&renderer_args.ctrl_out,
						&renderer_args.memfd) != 3)
				app_fatal("invalid renderer args");
		} else if (!strcmp(argv[i], "udmabuf")) {
			app.config.use_udmabuf = true;
			renderer_args.use_udmabuf = true;
		} else if (!strcmp(argv[i], "memfd")) {
			app.config.use_udmabuf = false;
			renderer_args.use_udmabuf = false;
		} else if (!strcmp(argv[i], "coherent")) {
			app.config.is_coherent = true;
		} else if (!strcmp(argv[i], "incoherent")) {
			app.config.is_coherent = false;
		} else {
			app_usage(&app);
		}
	}

	if (renderer_args.valid) {
		printf("renderer uses %s\n", renderer_args.use_udmabuf ?
				"udmabuf" : "memfd");
		return renderer(renderer_args.width, renderer_args.height,
				renderer_args.output_count,
				renderer_args.ctrl_in, renderer_args.ctrl_out,
				renderer_args.memfd,
				renderer_args.use_udmabuf);
	}

	printf("memfd heap is assumed %s\n", app.config.is_coherent ?
			"coherent" : "incoherent");

	app_init_heap(&app);
	app_init_renderer(&app);
	app_init_xcb(&app);

	/* get the heap layout from the renderer */
	const size_t heap_skip = app_recv(&app);
	const size_t ubo_size = app_recv(&app);
	const size_t output_size = app_recv(&app);
	app_init_memories(&app, heap_skip, ubo_size, output_size);

	app_mainloop(&app);

	return 0;
}

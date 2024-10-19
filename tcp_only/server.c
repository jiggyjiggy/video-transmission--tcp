#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define VIDEODEV "/dev/video0"
#define WIDTH 800
#define HEIGHT 600
#define PORT 12343 // TCP 포트

struct buffer
{
	void *start;
	size_t length;
};

static struct buffer *buffers = NULL;
static unsigned int n_buffers = 0;

static void mesg_exit(const char *s)
{
	fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
	exit(EXIT_FAILURE);
}

static int xioctl(int fd, int request, void *arg)
{
	int r;
	do
		r = ioctl(fd, request, arg);
	while (-1 == r && EINTR == errno);
	return r;
}

static void process_image(const void *p, int socket)
{
	// Send image data to client
	if (send(socket, p, WIDTH * HEIGHT * 2, 0) < 0)
	{
		perror("send");
	}
	// printf("qweqwewqe\n");
}

static int read_frame(int fd, int socket)
{
	// printf("zxcvxzcvxzcv\n");
	struct v4l2_buffer buf;
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf))
	{
		switch (errno)
		{
		case EAGAIN:
			return 0;
		case EIO:
		default:
			mesg_exit("VIDIOC_DQBUF");
		}
	}

	process_image(buffers[buf.index].start, socket);

	if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
		mesg_exit("VIDIOC_QBUF");
	// printf("pojpojpojpoj\n");

	return 1;
}

static void start_capturing(int fd)
{
	for (int i = 0; i < n_buffers; ++i)
	{
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
			mesg_exit("VIDIOC_QBUF");
	}

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
		mesg_exit("VIDIOC_STREAMON");
}

static void mainloop(int fd, int socket)
{
	while (1)
	{

		// printf("asdfasdfsdf\n");
		fd_set fds;
		struct timeval tv;

		FD_ZERO(&fds);
		FD_SET(fd, &fds);

		tv.tv_sec = 2;
		tv.tv_usec = 0;

		int r = select(fd + 1, &fds, NULL, NULL, &tv);
		if (-1 == r)
		{
			if (EINTR == errno)
				continue;
			mesg_exit("select");
		}
		else if (0 == r)
		{
			fprintf(stderr, "select timeout\n");
			exit(EXIT_FAILURE);
		}

		if (read_frame(fd, socket));
			// break;

		// printf("oihsadfoihasdofihadhfi\n");
	}
}

static void init_mmap(int fd)
{
	struct v4l2_requestbuffers req;
	memset(&req, 0, sizeof(req));
	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req))
		mesg_exit("VIDIOC_REQBUFS");

	buffers = calloc(req.count, sizeof(*buffers));
	for (n_buffers = 0; n_buffers < req.count; ++n_buffers)
	{
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = n_buffers;
		if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
			mesg_exit("VIDIOC_QUERYBUF");

		buffers[n_buffers].length = buf.length;
		buffers[n_buffers].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
		if (MAP_FAILED == buffers[n_buffers].start)
			mesg_exit("mmap");
	}
}

static void init_device(int fd)
{
	struct v4l2_capability cap;
	if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap))
	{
		mesg_exit("VIDIOC_QUERYCAP");
	}

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
	{
		fprintf(stderr, "%s is no video capture device\n", VIDEODEV);
		exit(EXIT_FAILURE);
	}

	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = WIDTH;
	fmt.fmt.pix.height = HEIGHT;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
		mesg_exit("VIDIOC_S_FMT");

	init_mmap(fd);
}

int main(int argc, char **argv)
{
	int camfd = -1;

	// Open camera device
	camfd = open(VIDEODEV, O_RDWR | O_NONBLOCK, 0);
	if (-1 == camfd)
	{
		fprintf(stderr, "Cannot open '%s': %d, %s\n", VIDEODEV, errno, strerror(errno));
		return EXIT_FAILURE;
	}

	init_device(camfd);
	start_capturing(camfd);

	// Setup TCP socket
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
	{
		perror("socket");
		exit(EXIT_FAILURE);
	}

	struct sockaddr_in server_addr, client_addr;
	socklen_t client_len = sizeof(client_addr);

	// Bind socket
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = INADDR_ANY;
	server_addr.sin_port = htons(PORT);
	if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
	{
		perror("bind");
		exit(EXIT_FAILURE);
	}

	// Listen for incoming connections
	listen(sockfd, 5);
	printf("Waiting for a client to connect...\n");

	// Accept a connection from client
	int newsockfd = accept(sockfd, (struct sockaddr *)&client_addr, &client_len);
	if (newsockfd < 0)
	{
		perror("accept");
		exit(EXIT_FAILURE);
	}
	printf("Client connected!\n");

	mainloop(camfd, newsockfd);

	// Cleanup
	close(newsockfd);
	close(sockfd);
	close(camfd);
	return 0;
}

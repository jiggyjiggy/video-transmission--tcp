#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/fb.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>

#define FBDEV "/dev/fb0"
#define PORT 12343 // TCP 포트
#define WIDTH 800
#define HEIGHT 600

struct fb_var_screeninfo vinfo;

static short *fbp = NULL;

// YUYV 데이터를 RGB565로 변환하는 함수
void yuyv_to_rgb565(unsigned char *yuyv, unsigned short *rgb565, int width, int height)
{
	int i, j;

	for (i = 0; i < height; i++)
	{
		for (j = 0; j < width; j += 2)
		{
			unsigned char y0 = yuyv[i * width * 2 + j * 2 + 0]; // 첫 번째 픽셀의 Y
			unsigned char u = yuyv[i * width * 2 + j * 2 + 1];	// 공유 U
			unsigned char y1 = yuyv[i * width * 2 + j * 2 + 2]; // 두 번째 픽셀의 Y
			unsigned char v = yuyv[i * width * 2 + j * 2 + 3];	// 공유 V

			// YUV to RGB 변환
			int r0 = y0 + 1.402 * (v - 128);
			int g0 = y0 - 0.344136 * (u - 128) - 0.714136 * (v - 128);
			int b0 = y0 + 1.772 * (u - 128);

			int r1 = y1 + 1.402 * (v - 128);
			int g1 = y1 - 0.344136 * (u - 128) - 0.714136 * (v - 128);
			int b1 = y1 + 1.772 * (u - 128);

			// RGB565 형식으로 변환 (5비트 R, 6비트 G, 5비트 B)
			r0 = (r0 > 255) ? 255 : (r0 < 0) ? 0 : r0;
			g0 = (g0 > 255) ? 255 : (g0 < 0) ? 0 : g0;
			b0 = (b0 > 255) ? 255 : (b0 < 0) ? 0 : b0;

			r1 = (r1 > 255) ? 255 : (r1 < 0) ? 0 : r1;
			g1 = (g1 > 255) ? 255 : (g1 < 0) ? 0 : g1;
			b1 = (b1 > 255) ? 255 : (b1 < 0) ? 0 : b1;

			rgb565[i * width + j] = ((r0 >> 3) << 11) | ((g0 >> 2) << 5) | (b0 >> 3);
			rgb565[i * width + j + 1] = ((r1 >> 3) << 11) | ((g1 >> 2) << 5) | (b1 >> 3);
		}
	}
}

static void mesg_exit(const char *s)
{
	perror(s);
	exit(EXIT_FAILURE);
}

int main()
{
	int fbfd = -1;

	// 프레임버퍼 디바이스 열기
	fbfd = open(FBDEV, O_RDWR);
	if (fbfd == -1)
	{
		perror("open( ) : framebuffer device");
		return EXIT_FAILURE;
	}

	// 프레임버퍼 정보 가져오기
	if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == -1)
	{
		perror("Error reading variable information.");
		close(fbfd);
		return EXIT_FAILURE;
	}

	// 화면 크기 계산 (비트 당 픽셀 수에 맞게)
	long screensize = vinfo.xres * vinfo.yres * (vinfo.bits_per_pixel / 8);

	// 프레임버퍼 메모리 매핑
	fbp = (short *)mmap(NULL, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fbp == MAP_FAILED)
	{
		perror("mmap() : framebuffer device to memory");
		close(fbfd);
		return EXIT_FAILURE;
	}

	// TCP 소켓 설정
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
	{
		perror("socket");
		munmap(fbp, screensize);
		close(fbfd);
		exit(EXIT_FAILURE);
	}

	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = inet_addr("127.0.0.1"); // 서버 IP 주소
	server_addr.sin_port = htons(PORT);

	// 서버에 연결 시도
	if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
	{
		perror("connect");
		munmap(fbp, screensize);
		close(fbfd);
		close(sockfd);
		exit(EXIT_FAILURE);
	}

	// 서버로부터 YUYV 데이터를 수신하고 변환하여 프레임버퍼에 복사
	int total_bytes_received = 0;
	int bytes_to_receive = WIDTH * HEIGHT * 2; // YUYV 데이터 크기 (2바이트 per 픽셀)

	unsigned char *yuyv_buffer = (unsigned char *)malloc(bytes_to_receive);
	unsigned short *rgb_buffer = (unsigned short *)malloc(WIDTH * HEIGHT * sizeof(unsigned short));

	while (1)
	{
		// YUYV 데이터 수신
		int bytes_received = recv(sockfd, yuyv_buffer + total_bytes_received, bytes_to_receive - total_bytes_received, 0);
		if (bytes_received < 0)
		{
			if (errno == EINTR)
				continue; // 인터럽트 발생 시 재시도
			perror("recv");
			break;
		}
		else if (bytes_received == 0)
		{
			// 서버가 연결을 종료함
			printf("Connection closed by server.\n");
			break;
		}

		total_bytes_received += bytes_received;

		// 전체 데이터를 수신했는지 확인
		if (total_bytes_received >= bytes_to_receive)
		{
			// YUYV 데이터를 RGB565로 변환
			yuyv_to_rgb565(yuyv_buffer, rgb_buffer, WIDTH, HEIGHT);

			// // 변환된 RGB565 데이터를 프레임버퍼에 복사
			memcpy(fbp, rgb_buffer, WIDTH * HEIGHT * sizeof(unsigned short));

			// // 변환된 RGB565 데이터를 프레임버퍼에 복사
            // int location = 0; // 프레임버퍼에 출력할 위치 초기화
            // for (int y = 0; y < vinfo.yres; y++)
            // {
            //     for (int x = 0; x < vinfo.xres; x++)
            //     {
            //         // 화면에서 이미지를 넘어서는 빈 공간을 처리한다.
            //         if (x >= WIDTH)
            //         {
            //             location += 2; // 빈 공간을 넘어서는 경우 위치 증가
            //             continue;
            //         }

            //         unsigned short pixel = rgb_buffer[y * WIDTH + x];
            //         *((unsigned short *)&fbp[location]) = pixel;
            //         location += 2; // 다음 픽셀로 이동
            //     }
            // }

			// 다음 프레임 수신을 위해 초기화
			total_bytes_received = 0; // 초기화
		}
	}

	// 정리 작업
	free(yuyv_buffer);
	free(rgb_buffer);
	munmap(fbp, screensize);
	close(sockfd);
	close(fbfd);
	return 0;
}

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

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#define FBDEV "/dev/fb0"
#define PORT 12345 // 서버 포트 번호 (서버 코드와 일치시켜야 함)
#define SERVER_IP "127.0.0.1" // 서버 IP 주소 변경 가능
#define WIDTH 800
#define HEIGHT 600

struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo; // 추가 선언
static short *fbp = NULL;

// 유틸리티 함수: 오류 메시지 출력 후 종료
static void mesg_exit(const char *s)
{
    perror(s);
    exit(EXIT_FAILURE);
}

// 모든 데이터를 수신하는 유틸리티 함수
ssize_t recv_all(int sockfd, void *buf, size_t len) {
    size_t total = 0;
    char *p = buf;
    while (total < len) {
        ssize_t received = recv(sockfd, p + total, len - total, 0);
        if (received <= 0) {
            if (received < 0 && errno == EINTR)
                continue;
            return -1;
        }
        total += received;
    }
    return total;
}

// YUV420P 데이터를 RGB565로 변환하는 함수
void yuv420p_to_rgb565(AVFrame *frame, unsigned short *rgb565, int width, int height, struct SwsContext *sws_ctx_conv)
{
    // 변환할 프레임을 위한 버퍼 설정
    AVFrame *rgb_frame = av_frame_alloc();
    if (!rgb_frame) {
        fprintf(stderr, "Could not allocate RGB frame\n");
        exit(1);
    }

    rgb_frame->format = AV_PIX_FMT_RGB565;
    rgb_frame->width  = width;
    rgb_frame->height = height;

    if (av_frame_get_buffer(rgb_frame, 32) < 0) {
        fprintf(stderr, "Could not allocate the RGB frame data\n");
        exit(1);
    }

    // YUV420P -> RGB565 변환
    sws_scale(sws_ctx_conv,
              (const uint8_t * const*)frame->data,
              frame->linesize,
              0,
              height,
              rgb_frame->data,
              rgb_frame->linesize);

    // RGB565 데이터를 프레임버퍼에 복사
    memcpy(rgb565, rgb_frame->data[0], width * height * sizeof(unsigned short));

    // 자원 해제
    av_frame_free(&rgb_frame);
}

// FFmpeg 디코더 초기화 함수
void initialize_ffmpeg_decoder(AVCodecParserContext **parser_ctx, AVCodecContext **codec_ctx)
{
    const AVCodec *codec;
    AVCodecParserContext *parser = NULL;
    AVCodecContext *c = NULL;

    // 디코더 찾기 (H.264)
    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "Codec H.264 not found\n");
        exit(1);
    }

    // 파서 초기화
    parser = av_parser_init(codec->id);
    if (!parser) {
        fprintf(stderr, "Parser not found for codec H.264\n");
        exit(1);
    }

    // 코덱 컨텍스트 초기화
    c = avcodec_alloc_context3(codec);
    if (!c) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }

    // 코덱 열기
    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }

    *parser_ctx = parser;
    *codec_ctx = c;
}

int main()
{
    int fbfd = -1;
    int sockfd = -1;

    // FFmpeg 변수
    AVCodecParserContext *parser = NULL;
    AVCodecContext *codec_ctx = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    struct SwsContext *sws_ctx_conv = NULL;

    // FFmpeg 디코더 초기화
    initialize_ffmpeg_decoder(&parser, &codec_ctx);
    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        exit(1);
    }

    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate AVFrame\n");
        exit(1);
    }

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

    // 프레임버퍼 고정 정보 가져오기
    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) == -1)
    {
        perror("Error reading fixed information.");
        close(fbfd);
        return EXIT_FAILURE;
    }

    // 프레임버퍼 정보 출력 (디버깅용)
    printf("Framebuffer resolution: %dx%d\n", vinfo.xres, vinfo.yres);
    printf("Framebuffer bits_per_pixel: %d\n", vinfo.bits_per_pixel);
    printf("Framebuffer line_length (stride): %d\n", finfo.line_length);

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

    // 프레임버퍼 초기화 (검은색)
    memset(fbp, 0, screensize);

    // TCP 소켓 설정
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
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
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP); // 서버 IP 주소
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

    printf("Connected to server %s:%d\n", SERVER_IP, PORT);

    // YUV420P -> RGB565 변환을 위한 스케일러 초기화
    sws_ctx_conv = sws_getContext(
        WIDTH, HEIGHT, AV_PIX_FMT_YUV420P, // 입력 해상도 및 포맷
        WIDTH, HEIGHT, AV_PIX_FMT_RGB565,  // 출력 해상도 및 포맷
        SWS_BILINEAR, NULL, NULL, NULL
    );
    if (!sws_ctx_conv) {
        fprintf(stderr, "Could not initialize the conversion context\n");
        munmap(fbp, screensize);
        close(fbfd);
        close(sockfd);
        exit(1);
    }

    // 수신 버퍼 설정
    unsigned char *recv_buffer = NULL;
    size_t recv_buffer_size = 0;

    // RGB 버퍼 미리 할당 (성능 최적화)
    unsigned short *rgb_buffer = malloc(WIDTH * HEIGHT * sizeof(unsigned short));
    if (!rgb_buffer) {
        fprintf(stderr, "Could not allocate RGB buffer\n");
        munmap(fbp, screensize);
        close(fbfd);
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // 메인 루프: H.264 패킷 수신 및 디코딩
    while (1)
    {
        uint32_t pkt_size_net;
        // 패킷 크기 수신
        ssize_t ret = recv_all(sockfd, &pkt_size_net, sizeof(pkt_size_net));
        if (ret != sizeof(pkt_size_net))
        {
            if (ret == 0) {
                printf("Connection closed by server.\n");
            } else {
                perror("recv size");
            }
            break;
        }

        uint32_t pkt_size = ntohl(pkt_size_net);
        if (pkt_size == 0) {
            fprintf(stderr, "Received packet size 0\n");
            continue;
        }

        // 패킷 데이터 수신
        if (recv_buffer_size < pkt_size) {
            free(recv_buffer);
            recv_buffer = (unsigned char *)malloc(pkt_size);
            if (!recv_buffer) {
                fprintf(stderr, "Could not allocate receive buffer\n");
                break;
            }
            recv_buffer_size = pkt_size;
        }

        ret = recv_all(sockfd, recv_buffer, pkt_size);
        if (ret != pkt_size)
        {
            if (ret == 0) {
                printf("Connection closed by server.\n");
            } else {
                perror("recv data");
            }
            break;
        }

        // 패킷 데이터를 FFmpeg 디코더에 전달
        uint8_t *data = recv_buffer;
        int data_size = pkt_size;
        while (data_size > 0)
        {
            int consumed = av_parser_parse2(parser, codec_ctx, &pkt->data, &pkt->size,
                                             data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (consumed < 0) {
                fprintf(stderr, "Error while parsing\n");
                break;
            }

            data += consumed;
            data_size -= consumed;

            if (pkt->size > 0) {
                if (avcodec_send_packet(codec_ctx, pkt) < 0) {
                    fprintf(stderr, "Error sending packet to decoder\n");
                    break;
                }

                while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                    // YUV420P -> RGB565 변환
                    yuv420p_to_rgb565(frame, rgb_buffer, WIDTH, HEIGHT, sws_ctx_conv);

                    // 프레임버퍼의 스트라이드 계산
                    int fb_stride = finfo.line_length / (vinfo.bits_per_pixel / 8); // 라인 당 픽셀 수

                    // RGB565 데이터를 프레임버퍼에 복사 (스트라이드 고려)
                    for (int y = 0; y < HEIGHT; y++) {
                        memcpy(fbp + (y * fb_stride),
                               rgb_buffer + (y * WIDTH),
                               WIDTH * sizeof(unsigned short));
                    }

                    // 추가 디버깅 로그 (옵션)
                    // printf("Decoded and copied frame %lld\n", frame->pts);
                }
                av_packet_unref(pkt);
            }
        }
    }

    // 자원 해제
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    av_parser_close(parser);
    sws_freeContext(sws_ctx_conv);
    free(recv_buffer);
    free(rgb_buffer);
    munmap(fbp, screensize);
    close(fbfd);
    close(sockfd);
    return 0;
}

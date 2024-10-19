#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>                  /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <malloc.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <linux/fb.h>
#include <linux/videodev2.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#define FBDEV       "/dev/fb0"      /* 프레임 버퍼를 위한 디바이스 파일 */
#define VIDEODEV    "/dev/video0"
#define WIDTH       800             /* 캡쳐받을 영상의 크기 */
#define HEIGHT      600

#define SERVER_IP   "0.0.0.0"       /* 모든 인터페이스에서 수신 */
#define SERVER_PORT 12345           /* 서버 포트 번호 */

struct buffer {
    void *start;
    size_t length;
};

static short *fbp = NULL;              /* 프레임버퍼의 MMAP를 위한 변수 */
struct buffer *buffers = NULL;
static unsigned int n_buffers = 0;
static struct fb_var_screeninfo vinfo; /* 프레임버퍼의 정보 저장을 위한 구조체 */

int listen_sockfd;                     /* 서버 소켓 파일 디스크립터 */
int client_sockfd = -1;                /* 클라이언트 소켓 파일 디스크립터 */

AVFormatContext *fmt_ctx = NULL;
AVStream *video_st = NULL;
AVCodecContext *codec_ctx = NULL;
const AVCodec *codec = NULL;
AVFrame *frame = NULL;
AVPacket *pkt = NULL;
struct SwsContext *sws_ctx = NULL;

/* 유틸리티 함수: 오류 메시지 출력 후 종료 */
static void mesg_exit(const char *s)
{
    fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
    exit(EXIT_FAILURE);
}

static int xioctl(int fd, int request, void *arg)
{
    int r;
    do r = ioctl(fd, request, arg); while(-1 == r && EINTR == errno);
    return r;
}

/* unsigned char의 범위를 넘어가지 않도록 경계 검사를 수행 */
static inline int clip(int value, int min, int max)
{
    return(value > max ? max : value < min ? min : value);
}

/* FFmpeg 초기화 함수 */
void ffmpeg_init()
{
    /* FFmpeg 라이브러리 초기화 */
    // avcodec_register_all(); // 이 줄을 삭제하거나 주석 처리
    avformat_network_init();

    /* 출력 포맷을 raw H.264로 설정 */
    avformat_alloc_output_context2(&fmt_ctx, NULL, "h264", NULL);
    if (!fmt_ctx) {
        fprintf(stderr, "Could not deduce output format.\n");
        exit(1);
    }

    /* 비디오 스트림 추가 */
    codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "H.264 codec not found\n");
        exit(1);
    }

    video_st = avformat_new_stream(fmt_ctx, codec);
    if (!video_st) {
        fprintf(stderr, "Could not allocate stream\n");
        exit(1);
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        fprintf(stderr, "Could not alloc an encoding context\n");
        exit(1);
    }

    /* 코덱 파라미터 설정 */
    codec_ctx->codec_id = AV_CODEC_ID_H264;
    codec_ctx->bit_rate = 1000000; // 네트워크 환경에 따라 올리고 내리고(동적으로)
    codec_ctx->width = WIDTH;
    codec_ctx->height = HEIGHT;
    video_st->time_base = (AVRational){1, 25};
    codec_ctx->time_base = video_st->time_base;
    codec_ctx->gop_size = 10;
    codec_ctx->max_b_frames = 1;
    codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    /* 코덱 열기 */
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }

    /* 스트림에 코덱 컨텍스트 연결 */
    if (avcodec_parameters_from_context(video_st->codecpar, codec_ctx) < 0) {
        fprintf(stderr, "Could not copy the stream parameters\n");
        exit(1);
    }

    /* 패킷 할당 */
    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        exit(1);
    }

    /* 프레임 할당 */
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }
    frame->format = codec_ctx->pix_fmt;
    frame->width  = codec_ctx->width;
    frame->height = codec_ctx->height;

    if (av_frame_get_buffer(frame, 32) < 0) {
        fprintf(stderr, "Could not allocate the video frame data\n");
        exit(1);
    }

    /* 스케일러 초기화 (YUYV -> YUV420P) */
    sws_ctx = sws_getContext(WIDTH, HEIGHT, AV_PIX_FMT_YUYV422,
                             WIDTH, HEIGHT, AV_PIX_FMT_YUV420P,
                             SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) {
        fprintf(stderr, "Could not initialize the conversion context\n");
        exit(1);
    }
}
/* TCP 서버 초기화 및 클라이언트 연결 수락 함수 */
void init_tcp_server()
{
    struct sockaddr_in server_addr;

    // 서버 소켓 생성
    listen_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sockfd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // 소켓 옵션 설정 (재사용 가능하도록)
    int opt = 1;
    if (setsockopt(listen_sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(listen_sockfd);
        exit(EXIT_FAILURE);
    }

    // 서버 주소 설정
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP); // "0.0.0.0"으로 모든 인터페이스에서 수신

    // 소켓 바인딩
    if (bind(listen_sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(listen_sockfd);
        exit(EXIT_FAILURE);
    }

    // 클라이언트의 연결을 대기
    if (listen(listen_sockfd, 1) < 0) {
        perror("listen");
        close(listen_sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on %s:%d\n", SERVER_IP, SERVER_PORT);

    // 클라이언트 연결 수락
    client_sockfd = accept(listen_sockfd, NULL, NULL);
    if (client_sockfd < 0) {
        perror("accept");
        close(listen_sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Client connected.\n");
}

/* 모든 데이터를 전송하는 유틸리티 함수 */
ssize_t send_all(int sockfd, const void *buf, size_t len) {
    size_t total = 0;
    const char *p = buf;
    while (total < len) {
        ssize_t sent = send(sockfd, p + total, len - total, 0);
        if (sent <= 0) {
            if (sent < 0 && errno == EINTR)
                continue;
            return -1;
        }
        total += sent;
    }
    return total;
}

/* 프레임을 인코딩하고 TCP로 전송하는 함수 */
void encode_frame(unsigned char *yuyv_data, int frame_index)
{
    /* YUYV 데이터를 AVFrame의 YUV420P로 변환 */
    const uint8_t *inData[1] = { yuyv_data };
    int inLinesize[1] = { 2 * WIDTH };

    if (sws_scale(sws_ctx, inData, inLinesize, 0, HEIGHT, frame->data, frame->linesize) < 0) {
        fprintf(stderr, "sws_scale failed\n");
        exit(1);
    }

    frame->pts = frame_index;

    /* 인코딩 */
    if (avcodec_send_frame(codec_ctx, frame) < 0) {
        fprintf(stderr, "Error sending a frame for encoding\n");
        exit(1);
    }

    while (avcodec_receive_packet(codec_ctx, pkt) == 0) {
        /* 패킷 타임스탬프 설정 */
        av_packet_rescale_ts(pkt, codec_ctx->time_base, video_st->time_base);
        pkt->stream_index = video_st->index;

        /* 패킷을 TCP 소켓으로 전송 */
        uint32_t pkt_size = htonl(pkt->size); // 네트워크 바이트 순서로 변환
        if (send_all(client_sockfd, &pkt_size, sizeof(pkt_size)) != sizeof(pkt_size)) {
            perror("send size");
            exit(EXIT_FAILURE);
        }

        if (send_all(client_sockfd, pkt->data, pkt->size) != pkt->size) {
            perror("send data");
            exit(EXIT_FAILURE);
        }

        av_packet_unref(pkt);
    }
}

/* FFmpeg 종료 함수 */
void ffmpeg_cleanup()
{
    /* 인코더 플러시 */
    avcodec_send_frame(codec_ctx, NULL);
    while (avcodec_receive_packet(codec_ctx, pkt) == 0) {
        /* 패킷을 클라이언트로 전송 */
        uint32_t pkt_size = htonl(pkt->size); // 네트워크 바이트 순서로 변환
        if (send_all(client_sockfd, &pkt_size, sizeof(pkt_size)) != sizeof(pkt_size)) {
            perror("send size");
            break;
        }

        if (send_all(client_sockfd, pkt->data, pkt->size) != pkt->size) {
            perror("send data");
            break;
        }

        av_packet_unref(pkt);
    }

    /* 자원 해제 */
    sws_freeContext(sws_ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    if (fmt_ctx && !(fmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&fmt_ctx->pb);
    avformat_free_context(fmt_ctx);
    avformat_network_deinit();
}

/* V4L2 관련 함수들 */

/* 버퍼에서 프레임을 읽어 인코딩 및 전송 */
static int read_frame(int fd, int frame_index)
{
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if(-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
        switch(errno) {
        case EAGAIN: return 0;
        case EIO:
            /* EIO는 드라이버에 따라 무시 가능 */
        default: mesg_exit("VIDIOC_DQBUF");
        }
    }

    /* 인코딩 및 TCP 전송 */
    encode_frame(buffers[buf.index].start, frame_index);

    if(-1 == xioctl(fd, VIDIOC_QBUF, &buf))
        mesg_exit("VIDIOC_QBUF");

    return 1;
}

/* 메인 루프: 프레임 캡처 및 전송 */
static void mainloop(int fd)
{
    unsigned int count = 120; // 예: 5초 동안 (25fps 기준)
    int frame_index = 0;
    // while(count-- > 0) {
	while (1) {
        for(;;) {
            fd_set fds;
            struct timeval tv;

            FD_ZERO(&fds);
            FD_SET(fd, &fds);

            /* Timeout 설정 */
            tv.tv_sec = 2;
            tv.tv_usec = 0;

            int r = select(fd + 1, &fds, NULL, NULL, &tv);
            if(-1 == r) {
                if(EINTR == errno) continue;
                mesg_exit("select");
            } else if(0 == r) {
                fprintf(stderr, "select timeout\n");
                exit(EXIT_FAILURE);
            }

            if(read_frame(fd, frame_index)) {
                frame_index++;
                break;
            }
        }
    }
}

/* 캡처 시작: 버퍼를 큐에 추가하고 스트리밍 시작 */
static void start_capturing(int fd)
{
    for(int i = 0; i < n_buffers; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory      = V4L2_MEMORY_MMAP;
        buf.index       = i;
        if(-1 == xioctl(fd, VIDIOC_QBUF, &buf))
            mesg_exit("VIDIOC_QBUF");
    }
    
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if(-1 == xioctl(fd, VIDIOC_STREAMON, &type))
        mesg_exit("VIDIOC_STREAMON");
}

/* 메모리 매핑 초기화 */
static void init_mmap(int fd)
{
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count       = 4;
    req.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory      = V4L2_MEMORY_MMAP;
    if(-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
        if(EINVAL == errno) {
            fprintf(stderr, "%s does not support memory mapping\n", VIDEODEV);
            exit(EXIT_FAILURE);
        } else {
            mesg_exit("VIDIOC_REQBUFS");
        }
    }

    if(req.count < 2) {
        fprintf(stderr, "Insufficient buffer memory on %s\n", VIDEODEV);
        exit(EXIT_FAILURE);
    }

    buffers = calloc(req.count, sizeof(*buffers));
    if(!buffers) {
        fprintf(stderr, "Out of memory\n");
        exit(EXIT_FAILURE);
    }

    for(n_buffers = 0; n_buffers < req.count; ++n_buffers) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory      = V4L2_MEMORY_MMAP;
        buf.index       = n_buffers;
        if(-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
            mesg_exit("VIDIOC_QUERYBUF");

        buffers[n_buffers].length = buf.length;
        buffers[n_buffers].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                        MAP_SHARED, fd, buf.m.offset);
        if(MAP_FAILED == buffers[n_buffers].start)
            mesg_exit("mmap");
    }
}

/* 디바이스 초기화 */
static void init_device(int fd)
{
    struct v4l2_capability cap;
    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;
    struct v4l2_format fmt;
    unsigned int min;

    if(-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
        if(EINVAL == errno) {
            fprintf(stderr, "%s is no V4L2 device\n", VIDEODEV);
            exit(EXIT_FAILURE);
        } else {
            mesg_exit("VIDIOC_QUERYCAP");
        }
    }

    if(!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "%s is no video capture device\n",
                         VIDEODEV);
        exit(EXIT_FAILURE);
    }

    if(!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "%s does not support streaming i/o\n", VIDEODEV);
        exit(EXIT_FAILURE);
    }

    /* Crop 설정 */
    memset(&cropcap, 0, sizeof(cropcap));
    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if(0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap)) {
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c = cropcap.defrect; /* 기본 설정으로 리셋 */
        xioctl(fd, VIDIOC_S_CROP, &crop);
    }

    /* 포맷 설정 */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = WIDTH;
    fmt.fmt.pix.height      = HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    if(-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
        mesg_exit("VIDIOC_S_FMT");

    /* 버그 패러노이아 */
    min = fmt.fmt.pix.width * 2;
    if(fmt.fmt.pix.bytesperline < min)
        fmt.fmt.pix.bytesperline = min;
    min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
    if(fmt.fmt.pix.sizeimage < min)
        fmt.fmt.pix.sizeimage = min;

    init_mmap(fd);
}

/* 메인 함수 */
int main(int argc, char **argv)
{
    int camfd = -1;        /* 카메라의 파일 디스크립터 */
    int fbfd = -1;         /* 프레임버퍼의 파일 디스크립터 */

    /* FFmpeg 초기화 */
    ffmpeg_init();

    /* TCP 서버 초기화 및 클라이언트 연결 수락 */
    init_tcp_server();

    /* 프레임버퍼 열기 (필요 없는 경우 제거 가능) */
    fbfd = open(FBDEV, O_RDWR);
    if(-1 == fbfd) {
        perror("open( ) : framebuffer device");
        return EXIT_FAILURE;
    }

    /* 프레임버퍼의 정보 가져오기 */
    if(-1 == ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo)) {
        perror("Error reading variable information.");
        return EXIT_FAILURE;
    }

    /* mmap( ) : 프레임버퍼를 위한 메모리 공간 확보 */
    long screensize = vinfo.xres * vinfo.yres * (vinfo.bits_per_pixel / 8);
    fbp = (short *)mmap(NULL, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if(fbp == MAP_FAILED) {
        perror("mmap() : framebuffer device to memory");
        return EXIT_FAILURE;
    }

    memset(fbp, 0, screensize);

    /* 카메라 장치 열기 */
    camfd = open(VIDEODEV, O_RDWR | O_NONBLOCK, 0);
    if(-1 == camfd) {
        fprintf(stderr, "Cannot open '%s': %d, %s\n",
                         VIDEODEV, errno, strerror(errno));
        return EXIT_FAILURE;
    }

    init_device(camfd);

    start_capturing(camfd);

    /* 메인 루프 실행 */
    mainloop(camfd);

    /* 캡쳐 중단 */
    enum v4l2_buf_type type;
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if(-1 == xioctl(camfd, VIDIOC_STREAMOFF, &type))
        mesg_exit("VIDIOC_STREAMOFF");

    /* 메모리 정리 */
    for(int i = 0; i < n_buffers; ++i)
        if(-1 == munmap(buffers[i].start, buffers[i].length))
            mesg_exit("munmap");
    free(buffers);

    munmap(fbp, screensize);

    /* 장치 닫기 */
    if(-1 == close(camfd) || -1 == close(fbfd))
        mesg_exit("close");

    /* FFmpeg 종료 및 파일 정리 */
    ffmpeg_cleanup();

    /* TCP 소켓 닫기 */
    close(client_sockfd);
    close(listen_sockfd);

    return EXIT_SUCCESS; 
}
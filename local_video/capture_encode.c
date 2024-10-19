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

#include <linux/fb.h>
#include <linux/videodev2.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#define FBDEV       "/dev/fb0"      /* 프레임 버퍼를 위한 디바이스 파일 */
#define VIDEODEV    "/dev/video0"
#define WIDTH       800               /* 캡쳐받을 영상의 크기 */
#define HEIGHT      600

/* Video4Linux에서 사용할 영상 저장을 위한 버퍼 */
struct buffer {
    void * start;
    size_t length;
};

static short *fbp               = NULL;         /* 프레임버퍼의 MMAP를 위한 변수 */
struct buffer *buffers         = NULL;
static unsigned int n_buffers   = 0;
static struct fb_var_screeninfo vinfo;        /* 프레임버퍼의 정보 저장을 위한 구조체 */

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

static void process_image(const void *p, short *fbp, struct fb_var_screeninfo vinfo)
{
    unsigned char* in = (unsigned char*)p;
    int width = WIDTH;
    int height = HEIGHT;
    int istride = WIDTH*2;          /* 이미지의 폭을 넘어가면 다음 라인으로 내려가도록 설정 */
    int x, y, j;
    int y0, u, y1, v, r, g, b;
    unsigned short pixel;
    long location = 0;
    for(y = 0; y < height; ++y) {
        for(j = 0, x = 0; j < vinfo.xres * 2; j += 4, x += 2) {
            if(j >= width*2) {                 /* 현재의 화면에서 이미지를 넘어서는 빈 공간을 처리 */
                 location++; location++;
                 continue;
            }
            /* YUYV 성분을 분리 */
            y0 = in[j];
            u = in[j + 1] - 128;
            y1 = in[j + 2];
            v = in[j + 3] - 128;

            /* YUV를 RGB로 전환 */
            r = clip((298 * y0 + 409 * v + 128) >> 8, 0, 255);
            g = clip((298 * y0 - 100 * u - 208 * v + 128) >> 8, 0, 255);
            b = clip((298 * y0 + 516 * u + 128) >> 8, 0, 255);
            pixel =((r>>3)<<11)|((g>>2)<<5)|(b>>3);                 /* 16비트 컬러로 전환 */
            fbp[location++] = pixel;

            /* YUV를 RGB로 전환 */
            r = clip((298 * y1 + 409 * v + 128) >> 8, 0, 255);
            g = clip((298 * y1 - 100 * u - 208 * v + 128) >> 8, 0, 255);
            b = clip((298 * y1 + 516 * u + 128) >> 8, 0, 255);
            pixel =((r>>3)<<11)|((g>>2)<<5)|(b>>3);                 /* 16비트 컬러로 전환 */
            fbp[location++] = pixel;
        }
        in += istride;
    }
}

/* FFmpeg 관련 변수 */
AVFormatContext *fmt_ctx = NULL;
AVStream *video_st = NULL;
AVCodecContext *codec_ctx = NULL;
const AVCodec *codec = NULL;
AVFrame *frame = NULL;
AVPacket *pkt = NULL;
struct SwsContext *sws_ctx = NULL;

/* FFmpeg 초기화 함수 */
void ffmpeg_init(const char *filename)
{
    /* 출력 포맷을 추측 */
    avformat_alloc_output_context2(&fmt_ctx, NULL, NULL, filename);
    if (!fmt_ctx) {
        fprintf(stderr, "Could not deduce output format from file extension.\n");
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
    codec_ctx->bit_rate = 400000;
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

    /* 출력 파일 열기 */
    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmt_ctx->pb, filename, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "Could not open '%s'\n", filename);
            exit(1);
        }
    }

    /* 파일 헤더 작성 */
    if (avformat_write_header(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
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

    /* 패킷 할당 */
    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Could not allocate AVPacket\n");
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

/* 프레임을 인코딩하고 파일에 저장하는 함수 */
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

        /* 패킷 기록 */
        if (av_interleaved_write_frame(fmt_ctx, pkt) < 0) {
            fprintf(stderr, "Error while writing video frame\n");
            exit(1);
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
        av_packet_rescale_ts(pkt, codec_ctx->time_base, video_st->time_base);
        pkt->stream_index = video_st->index;
        av_interleaved_write_frame(fmt_ctx, pkt);
        av_packet_unref(pkt);
    }

    /* 파일 트레일러 작성 */
    av_write_trailer(fmt_ctx);

    /* 자원 해제 */
    sws_freeContext(sws_ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&fmt_ctx->pb);
    avformat_free_context(fmt_ctx);
}

/* V4L2 관련 함수들 (기존 코드와 유사) */

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
            /* Could ignore EIO, see spec. */
        default: mesg_exit("VIDIOC_DQBUF");
        }
    }

    /* 인코딩 및 파일 저장 */
    encode_frame(buffers[buf.index].start, frame_index);

    if(-1 == xioctl(fd, VIDIOC_QBUF, &buf))
        mesg_exit("VIDIOC_QBUF");

    return 1;
}

static void mainloop(int fd)
{
    unsigned int count = 120; // 예: 5초 동안 (25fps 기준)
    int frame_index = 0;
    while(count-- > 0) {
        for(;;) {
            fd_set fds;
            struct timeval tv;

            FD_ZERO(&fds);
            FD_SET(fd, &fds);

            /* Timeout. */
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

    /* Select video input, video standard and tune here. */
    memset(&cropcap, 0, sizeof(cropcap));
    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if(0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap)) {
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c = cropcap.defrect; /* reset to default */
        xioctl(fd, VIDIOC_S_CROP, &crop);
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = WIDTH;
    fmt.fmt.pix.height      = HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    if(-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
        mesg_exit("VIDIOC_S_FMT");

    /* Buggy driver paranoia. */
    min = fmt.fmt.pix.width * 2;
    if(fmt.fmt.pix.bytesperline < min)
        fmt.fmt.pix.bytesperline = min;
    min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
    if(fmt.fmt.pix.sizeimage < min)
        fmt.fmt.pix.sizeimage = min;

    init_mmap(fd);
}

int main(int argc, char **argv)
{
    int fbfd = -1;              /* 프레임버퍼의 파일 디스크립터 */
    int camfd = -1;		/* 카메라의 파일 디스크립터 */
    const char *output_filename = "output.mp4"; /* 출력 파일 이름 */

    /* FFmpeg 초기화 */
    ffmpeg_init(output_filename);

    /* 프레임버퍼 열기 */
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
    long screensize = vinfo.xres * vinfo.yres * 2;
    fbp = (short *)mmap(NULL, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if(fbp == (short*)-1) {
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

    return EXIT_SUCCESS; 
}

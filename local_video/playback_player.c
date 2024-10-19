#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>                  /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <sys/time.h>               /* gettimeofday 함수 포함 */

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#define FBDEV "/dev/fb0"             /* 프레임 버퍼 디바이스 파일 */

/* 오류 메시지 출력 후 종료 */
static void error_exit(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "사용법: %s <video_file>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *video_filename = argv[1];

    /* 프레임버퍼 열기 */
    int fb_fd = open(FBDEV, O_RDWR);
    if (fb_fd == -1) {
        error_exit("프레임버퍼 열기 실패");
    }

    /* 프레임버퍼 정보 가져오기 */
    struct fb_var_screeninfo vinfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == -1) {
        close(fb_fd);
        error_exit("FBIOGET_VSCREENINFO 실패");
    }

    /* 프레임버퍼 메모리 맵핑 */
    size_t screensize = vinfo.xres * vinfo.yres * (vinfo.bits_per_pixel / 8);
    char *fbp = (char *)mmap(NULL, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fbp == MAP_FAILED) {
        close(fb_fd);
        error_exit("프레임버퍼 맵핑 실패");
    }

    /* FFmpeg 초기화 */
    // av_register_all();  /* 제거됨 */

    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, video_filename, NULL, NULL) != 0) {
        munmap(fbp, screensize);
        close(fb_fd);
        fprintf(stderr, "영상 파일을 열 수 없습니다: %s\n", video_filename);
        exit(EXIT_FAILURE);
    }

    /* 스트림 정보 검색 */
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        munmap(fbp, screensize);
        close(fb_fd);
        fprintf(stderr, "스트림 정보를 찾을 수 없습니다.\n");
        exit(EXIT_FAILURE);
    }

    /* 비디오 스트림 찾기 */
    int video_stream_index = -1;
    AVCodecParameters *codec_par = NULL;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            codec_par = fmt_ctx->streams[i]->codecpar;
            break;
        }
    }

    if (video_stream_index == -1) {
        avformat_close_input(&fmt_ctx);
        munmap(fbp, screensize);
        close(fb_fd);
        fprintf(stderr, "비디오 스트림을 찾을 수 없습니다.\n");
        exit(EXIT_FAILURE);
    }

    /* 디코더 찾기 */
    const AVCodec *codec = avcodec_find_decoder(codec_par->codec_id);  /* const 추가 */
    if (!codec) {
        avformat_close_input(&fmt_ctx);
        munmap(fbp, screensize);
        close(fb_fd);
        fprintf(stderr, "디코더를 찾을 수 없습니다.\n");
        exit(EXIT_FAILURE);
    }

    /* 디코더 컨텍스트 초기화 */
    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        avformat_close_input(&fmt_ctx);
        munmap(fbp, screensize);
        close(fb_fd);
        fprintf(stderr, "디코더 컨텍스트를 할당할 수 없습니다.\n");
        exit(EXIT_FAILURE);
    }

    if (avcodec_parameters_to_context(codec_ctx, codec_par) < 0) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        munmap(fbp, screensize);
        close(fb_fd);
        fprintf(stderr, "디코더 컨텍스트에 파라미터를 설정할 수 없습니다.\n");
        exit(EXIT_FAILURE);
    }

    /* 디코더 열기 */
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        munmap(fbp, screensize);
        close(fb_fd);
        fprintf(stderr, "디코더를 열 수 없습니다.\n");
        exit(EXIT_FAILURE);
    }

    /* 원본 영상의 가로세로 비율 계산 */
    double video_aspect = (double)codec_ctx->width / codec_ctx->height;
    double fb_aspect = (double)vinfo.xres / vinfo.yres;

    int scaled_width, scaled_height;
    int x_offset = 0, y_offset = 0;

    if (video_aspect > fb_aspect) {
        // 영상이 프레임버퍼보다 가로가 더 넓은 경우
        scaled_width = vinfo.xres;
        scaled_height = (int)(vinfo.xres / video_aspect);
        y_offset = (vinfo.yres - scaled_height) / 2;
    } else {
        // 영상이 프레임버퍼보다 세로가 더 높은 경우
        scaled_height = vinfo.yres;
        scaled_width = (int)(vinfo.yres * video_aspect);
        x_offset = (vinfo.xres - scaled_width) / 2;
    }

    /* 스케일러 초기화: 원본 해상도에서 스케일링된 해상도로 변환 */
    struct SwsContext *sws_ctx = sws_getContext(
        codec_ctx->width,
        codec_ctx->height,
        codec_ctx->pix_fmt,
        scaled_width,
        scaled_height,
        AV_PIX_FMT_RGB565,  /* 프레임버퍼의 픽셀 포맷에 맞게 설정 */
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL
    );

    if (!sws_ctx) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        munmap(fbp, screensize);
        close(fb_fd);
        fprintf(stderr, "스케일러를 초기화할 수 없습니다.\n");
        exit(EXIT_FAILURE);
    }

    /* 스케일링된 프레임을 저장할 프레임 생성 */
    AVFrame *frame = av_frame_alloc();
    AVFrame *rgb_frame = av_frame_alloc();
    if (!frame || !rgb_frame) {
        sws_freeContext(sws_ctx);
        av_frame_free(&frame);
        av_frame_free(&rgb_frame);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        munmap(fbp, screensize);
        close(fb_fd);
        fprintf(stderr, "프레임을 할당할 수 없습니다.\n");
        exit(EXIT_FAILURE);
    }

    rgb_frame->format = AV_PIX_FMT_RGB565;
    rgb_frame->width = scaled_width;
    rgb_frame->height = scaled_height;

    if (av_frame_get_buffer(rgb_frame, 32) < 0) {
        sws_freeContext(sws_ctx);
        av_frame_free(&frame);
        av_frame_free(&rgb_frame);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        munmap(fbp, screensize);
        close(fb_fd);
        fprintf(stderr, "RGB 프레임 버퍼를 할당할 수 없습니다.\n");
        exit(EXIT_FAILURE);
    }

    /* 패킷 및 프레임 읽기 */
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        sws_freeContext(sws_ctx);
        av_frame_free(&frame);
        av_frame_free(&rgb_frame);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        munmap(fbp, screensize);
        close(fb_fd);
        fprintf(stderr, "패킷을 할당할 수 없습니다.\n");
        exit(EXIT_FAILURE);
    }

    /* 프레임 디스플레이를 위한 변수 */
    int ret;

    /* 프레임 수 */
    int frame_count = 0;

    /* 동기화를 위한 타이머 설정 */
    struct timeval start_time, current_time;
    gettimeofday(&start_time, NULL);

    /* 디코딩 루프 */
    while (av_read_frame(fmt_ctx, packet) >= 0) {
        if (packet->stream_index == video_stream_index) {
            /* 패킷 디코딩 */
            ret = avcodec_send_packet(codec_ctx, packet);
            if (ret < 0) {
                fprintf(stderr, "패킷을 디코더로 보낼 수 없습니다: %s\n", av_err2str(ret));
                break;
            }

            /* 프레임 수신 */
            while (ret >= 0) {
                ret = avcodec_receive_frame(codec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                else if (ret < 0) {
                    fprintf(stderr, "프레임을 디코딩할 수 없습니다: %s\n", av_err2str(ret));
                    goto cleanup;
                }

                /* 프레임 스케일링 및 포맷 변환 */
                sws_scale(
                    sws_ctx,
                    (const uint8_t * const *)frame->data,
                    frame->linesize,
                    0,
                    codec_ctx->height,
                    rgb_frame->data,
                    rgb_frame->linesize
                );

                /* 프레임 데이터를 프레임버퍼에 복사 */
                for (int y = 0; y < scaled_height; y++) {
                    memcpy(
                        fbp + (y + y_offset) * vinfo.xres * (vinfo.bits_per_pixel / 8) + (x_offset * (vinfo.bits_per_pixel / 8)),
                        rgb_frame->data[0] + y * rgb_frame->linesize[0],
                        scaled_width * (vinfo.bits_per_pixel / 8)
                    );
                }

                /* 프레임 수 증가 */
                frame_count++;

                /* 프레임 속도 동기화 */
                gettimeofday(&current_time, NULL);
                double elapsed = (current_time.tv_sec - start_time.tv_sec) +
                                 (current_time.tv_usec - start_time.tv_usec) / 1000000.0;

                /* 실제 영상의 FPS 가져오기 */
                double fps = 25.0;  // 기본값 설정
                if (fmt_ctx->streams[video_stream_index]->avg_frame_rate.num != 0 &&
                    fmt_ctx->streams[video_stream_index]->avg_frame_rate.den != 0) {
                    fps = av_q2d(fmt_ctx->streams[video_stream_index]->avg_frame_rate);
                }

                double expected = frame_count / fps;
                if (expected > elapsed)
                    usleep((useconds_t)((expected - elapsed) * 1000000));
            }
        }
        av_packet_unref(packet);
    }

cleanup:
    /* 리소스 정리 */
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&rgb_frame);
    sws_freeContext(sws_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    munmap(fbp, screensize);
    close(fb_fd);

    return 0;
}

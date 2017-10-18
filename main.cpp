#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <errno.h>
#include <sys/time.h>
#include <setjmp.h>
#include <signal.h>

#include "ge2d.h"

// RGBA colors
const int BLUE = 0x0000ffff;
const int RED = 0xff0000ff;
const int BLACK = 0x000000ff;

struct fb_var_screeninfo fb_vinfo;
struct fb_fix_screeninfo fb_finfo;

// Test pattern sizes
const int testWidth = 1920;
const int testHeight = 1080;
const int testLength = (testWidth * testHeight * 3) / 2; // YUV size
const char testYUVFile[] = "sampleFHD.yuv";

// Global variable(s)
bool isRunning;
//int dmabuf_fd = -1;
timeval startTime;
timeval endTime;

// /dev/fb1 memory location from dmesg output
// mesonfb1(low)           : 0x7e000000
const unsigned long physicalAddress = 0x7e000000;

void ResetTime()
{
    gettimeofday(&startTime, NULL);
    endTime = startTime;
}

float GetTime()
{
    gettimeofday(&endTime, NULL);
    float seconds = (endTime.tv_sec - startTime.tv_sec);
    float milliseconds = (float(endTime.tv_usec - startTime.tv_usec)) / 1000000.0f;

    startTime = endTime;

    return seconds + milliseconds;
}

// Signal handler for Ctrl-C
void SignalHandler(int s)
{
    isRunning = false;
}

void CreateTestPattern()
{
    // The GE2D hardware only works with physically contiguous bus addresses.
    // Only the kernel or a driver can provide this type of memory.  Instead of
    // including a kernel driver, this code borrows memory from /dev/fb1.  This
    // makes the code fragile and prone to breakage should the kernel change.
    // Production code should use the kernel CMA allocator instead.


    // Borrow physical memory from /dev/fb1
    // Must be root
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
    {
        printf("Can't open /dev/mem (%x)\n", fd);
        exit(1);
    }

    int* data = (int*) mmap(0, 0x01000000, PROT_READ|PROT_WRITE, MAP_SHARED, fd, physicalAddress);
    if (data == NULL)
    {
        printf("Can't mmap\n");
        exit(1);
    }

    printf("virtual address = %x\n", data);

    // Read YUV file into memory
    FILE *fp = fopen(testYUVFile, "rb");
    int tmp, total = 0;

    // first zone
    char *dataptr = (char *)data;
    do {
        tmp = fread(dataptr + total, 1, testLength - total, fp);
        if (tmp > 0) {
            total += tmp;
        } else {
            if (tmp < 0) {
                fprintf(stderr, "ERR: read file %s failed, err %s\n", testYUVFile, strerror(errno));
            }
            break;
        }
    } while (true);

    fclose(fp);
    fprintf(stderr, "READ %dbytes\n", total);

    memcpy(dataptr + testLength, dataptr, testLength);
    memcpy(dataptr + testLength * 2, dataptr, testLength);
    memcpy(dataptr + testLength * 3, dataptr, testLength);

    // Clean up
    munmap(data, 0x01000000);
    close(fd);
}



void FillRectangle(int fd_ge2d, int x, int y, int width, int height, int color)
{
    // Tell the hardware the destination is /dev/fb0
    config_para_t config;
    memset(&config, 0x00, sizeof(config));

    config.src_dst_type = OSD0_OSD0;

    int ret = ioctl(fd_ge2d, GE2D_CONFIG, &config);
    printf("GE2D_CONFIG ret: %x\n", ret);


    // Perform a fill operation;
    ge2d_para_t fillRectParam;
    fillRectParam.src1_rect.x = x;
    fillRectParam.src1_rect.y = y;
    fillRectParam.src1_rect.w = width;
    fillRectParam.src1_rect.h = height;
    fillRectParam.color = color; // R G B A

    ret = ioctl(fd_ge2d, GE2D_FILLRECTANGLE, &fillRectParam);
    printf("GE2D_FILLRECTANGLE ret: %x\n", ret);
}



void BlitTestPattern(void *addr, int fd_ge2d, int dstX, int dstY, int screenWidth, int screenHeight)
{
    // Tell the hardware we will source memory (that we borrowed)
    // and write to /dev/fb0

    // This shows the expanded form of config.  Using this expanded
    // form allows the blit source x and y read directions to be specified
    // as well as the destination x and y write directions.  Together
    // they allow overlapping blit operations to be performed.
    config_para_s config = { 0 };

    config.src_dst_type = ALLOC_OSD0; //ALLOC_OSD0;
    config.alu_const_color = 0xffffffff;
    //GE2D_FORMAT_S16_YUV422T, GE2D_FORMAT_S16_YUV422B kernel panics
    config.src_format = GE2D_LITTLE_ENDIAN | GE2D_FORMAT_M24_NV12; //GE2D_LITTLE_ENDIAN | GE2D_FORMAT_S8_Y;
    // Plane 0 contains Y data
    config.src_planes[0].addr = (unsigned long)addr;
    config.src_planes[0].w = testWidth;
    config.src_planes[0].h = testHeight;
    // Plane 1 contains UV data
    config.src_planes[1].addr = config.src_planes[0].addr + (testWidth * testHeight);
    config.src_planes[1].w = testWidth;
    config.src_planes[1].h = testHeight / 2;

    config.dst_format = GE2D_LITTLE_ENDIAN | GE2D_FORMAT_S32_ARGB; //GE2D_FORMAT_S32_ARGB;

    int ret = ioctl(fd_ge2d, GE2D_CONFIG, &config);
    if (ret < 0) {
        perror("GE2D_CONFIG");
    }

    // Perform the blit operation
    ge2d_para_t blitRectParam2;
    memset(&blitRectParam2, 0, sizeof(blitRectParam2));

    blitRectParam2.src1_rect.x = 0;
    blitRectParam2.src1_rect.y = 0;
    blitRectParam2.src1_rect.w = testWidth;
    blitRectParam2.src1_rect.h = testHeight;
    blitRectParam2.dst_rect.x = dstX;
    blitRectParam2.dst_rect.y = dstY;
    blitRectParam2.dst_rect.w = screenWidth;
    blitRectParam2.dst_rect.h = screenHeight;

    ret = ioctl(fd_ge2d, GE2D_STRETCHBLIT_NOALPHA, &blitRectParam2);
    if (ret < 0) {
        perror("GE2D_STRETCHBLIT_NOALPHA");
    }
}

int main()
{
    int fd_ge2d = open("/dev/ge2d", O_RDWR);
    if (fd_ge2d < 0)
    {
        printf("Can't open /dev/ge2d\n");
        exit(1);
    }

    // Get the screen height and width
    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd == -1) {
        printf("Can't open framebuffer device\n");
        exit(1);
    }

    memset(&fb_vinfo, 0, sizeof(fb_vinfo));
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &fb_vinfo) == -1) {
        perror("Error reading FBIOGET_VSCREENINFO");
        exit(1);
    }

    memset(&fb_finfo, 0, sizeof(fb_finfo));
    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &fb_finfo) < 0){
        perror("Error reading FBIOGET_FSCREENINFO");
        exit(1);
    }

    close(fbfd);

    int screenWidth = fb_vinfo.xres;
    int screenHeight = fb_vinfo.yres;

    printf("Screen size = %d x %d\n", screenWidth, screenHeight);

    CreateTestPattern();

    FillRectangle(fd_ge2d, 0, 0, screenWidth, screenHeight, BLUE); // RGBA

    // ----- RENDERING -----
    int frames = 0;
    float totalTime = 0;
    isRunning = true;
    char *dataptr = (char *)physicalAddress;

    ResetTime();

    printf("Start displaying YUV file. Exit by Ctrl+C...\n");

    // Trap signal to clean up
    signal(SIGINT, SignalHandler);
    do {
        BlitTestPattern(dataptr, fd_ge2d, 0, 0, testWidth, testHeight);
        BlitTestPattern(dataptr + testLength, fd_ge2d, testWidth, 0, testWidth, testHeight);
        BlitTestPattern(dataptr + testLength * 2, fd_ge2d, 0, testHeight, testWidth, testHeight);
        BlitTestPattern(dataptr + testLength * 3, fd_ge2d, testWidth, testHeight, testWidth, testHeight);

        // Flip
        ioctl(fbfd, FBIOPAN_DISPLAY, &fb_vinfo);

        // Wait for vsync
        // The normal order is to wait for vsync and then pan,
        // but its done backwards due to the wait its implemented
        // by Amlogic (non-syncronous).
        ioctl(fbfd, FBIO_WAITFORVSYNC, 0);

        // FPS
        float deltaTime = GetTime();

        totalTime += deltaTime;
        ++frames;

        if (totalTime >= 5.0f)
        {
          float fps = (frames / totalTime);
          printf("FPS: %.1f, %dframes/%fs\n", fps, frames, totalTime);

          frames = 0;
          totalTime = 0;
        }
    } while (isRunning != 0);


    close(fd_ge2d);

    printf("-EXIT!!!-\n");

    return 0;
}

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
#include "ion.h"
#include "meson_ion.h"
#include "convert.h"

typedef struct {
  void *start;
  size_t length;
  unsigned long phys_addr;
  ion_user_handle_t handle;
} ion_buffer;

#define USING_SW_YUV_RGB 0

#define CLEAR(x) memset(&(x), 0, sizeof(x))

// RGBA colors
const int BLUE = 0x0000ffff;
const int RED = 0xff0000ff;
const int BLACK = 0x000000ff;

struct fb_var_screeninfo fb_vinfo;
struct fb_fix_screeninfo fb_finfo;
ion_buffer ion_buf;
ion_buffer ion_rgb_buf;

// Test pattern sizes
const int testWidth = 1280;
const int testHeight = 768;
const int testLength = (testWidth * testHeight * 3) / 2; // YUV size
const char testYUVFile[] = "sample_1280x768.yuv";

// Global variable(s)
bool isRunning;
//int dmabuf_fd = -1;
timeval startTime;
timeval endTime;

int xioctl(int fd, int request, void *arg);

void alloc_ion_buffer(int ion_fd, ion_buffer *ion_buf, int size);

void free_ion_buffer(int ion_fd, ion_user_handle_t handle);

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

void CreateTestPattern(void *buf)
{
    char *chptr = (char *)buf;
    // The GE2D hardware only works with physically contiguous bus addresses.
    // Only the kernel or a driver can provide this type of memory.  Instead of
    // including a kernel driver, this code borrows memory from /dev/fb1.  This
    // makes the code fragile and prone to breakage should the kernel change.
    // Production code should use the kernel CMA allocator instead.

    // Read YUV file into memory
    FILE *fp = fopen(testYUVFile, "rb");
    int tmp, total = 0;

    do {
        tmp = fread(chptr + total, 1, testLength - total, fp);
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
    fprintf(stderr, "READ %d/%dbytes\n", total, testLength);
    // memset(ion_buf.start + (testWidth * testHeight * 5)/4, 0, testWidth*testHeight/4);
}

void CreateTestPatternRgb()
{
    int i,j;
    int *data = (int *)ion_rgb_buf.start;
    for (i = 0; i < testHeight; ++i) {
        for (j = 0; j < testWidth; ++j) {
            data[i * testWidth + j] = 0xff00ff00; // ARGB
        }
    }
}

void FillRectangle(int fd_ge2d, int x, int y, int width, int height, int color)
{
    // Tell the hardware the destination is /dev/fb0
    struct config_para_s config;
    memset(&config, 0x00, sizeof(config));

    config.src_dst_type = OSD0_OSD0;

    int ret = ioctl(fd_ge2d, GE2D_CONFIG, &config);
    printf("GE2D_CONFIG ret: %x\n", ret);


    // Perform a fill operation;
    struct ge2d_para_s fillRectParam;
    fillRectParam.src1_rect.x = x;
    fillRectParam.src1_rect.y = y;
    fillRectParam.src1_rect.w = width;
    fillRectParam.src1_rect.h = height;
    fillRectParam.color = color; // R G B A

    ret = ioctl(fd_ge2d, GE2D_FILLRECTANGLE, &fillRectParam);
    printf("GE2D_FILLRECTANGLE ret: %x\n", ret);
}

void BlitTestPattern(int fd_ge2d, int dstX, int dstY, int screenWidth, int screenHeight)
{
    config_para_s config = { 0 };

    config.src_dst_type = ALLOC_OSD0; //ALLOC_OSD0;
    config.alu_const_color = 0xffffffff;
    //GE2D_FORMAT_S16_YUV422T, GE2D_FORMAT_S16_YUV422B kernel panics
#if USING_SW_YUV_RGB
    config.src_format = GE2D_LITTLE_ENDIAN | GE2D_FORMAT_S32_ARGB;
#else
    config.src_format = GE2D_LITTLE_ENDIAN | GE2D_FORMAT_M24_NV12;
#endif

    // Plane 0 contains Y data
    config.src_planes[0].addr = ion_rgb_buf.phys_addr;
    config.src_planes[0].w = testWidth;
    config.src_planes[0].h = testHeight;
    // Plane 1 contains UV data
#if !USING_SW_YUV_RGB
    config.src_planes[1].addr = ion_rgb_buf.phys_addr + (testWidth * testHeight);
    config.src_planes[1].w = testWidth;
    config.src_planes[1].h = testHeight / 2;
#endif

    config.dst_format = GE2D_LITTLE_ENDIAN | GE2D_FORMAT_S32_ARGB; //GE2D_FORMAT_S32_ARGB;
    // config.dst_planes[0].addr = (unsigned long int) fb_finfo.smem_start;
    // config.dst_planes[0].w = screenWidth;
    // config.dst_planes[0].h = screenHeight;

    int ret = ioctl(fd_ge2d, GE2D_CONFIG, &config);
    if (ret < 0) {
        perror("GE2D_CONFIG");
    }

    // Perform the blit operation
    struct ge2d_para_s blitRect;
    memset(&blitRect, 0, sizeof(blitRect));

    blitRect.src1_rect.x = 0;
    blitRect.src1_rect.y = 0;
    blitRect.src1_rect.w = testWidth;
    blitRect.src1_rect.h = testHeight;
    blitRect.dst_rect.x = 0;
    blitRect.dst_rect.y = 0;
    blitRect.dst_rect.x = dstX;
    blitRect.dst_rect.y = dstY;

    ret = ioctl(fd_ge2d, GE2D_BLIT_NOALPHA, &blitRect);
    if (ret < 0) {
        perror("GE2D_BLIT_NOALPHA");
    }
}

int main()
{
    int ion_fd = open("/dev/ion", O_RDWR);
    if (ion_fd < 0) {
        perror("Can't open /dev/ion");
        exit(1);
    }
    // Allocate buffer
    // memset(&ion_buf, 0, sizeof(ion_buf));
    // alloc_ion_buffer(ion_fd, &ion_buf, testWidth * testHeight * 2);
    memset(&ion_rgb_buf, 0, sizeof(ion_rgb_buf));
    alloc_ion_buffer(ion_fd, &ion_rgb_buf, testWidth * testHeight * 4);

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

    int screenWidth = fb_vinfo.xres;
    int screenHeight = fb_vinfo.yres;

    printf("Screen size = %d x %d\n", screenWidth, screenHeight);
    printf("Sample size = %d x %d\n", testWidth, testHeight);

#if USING_SW_YUV_RGB
    char *yuvData = (char *)malloc(testWidth*testHeight*2);
    CreateTestPattern(yuvData);
#else
    CreateTestPattern(ion_rgb_buf.start);
#endif /* USING_SW_YUV_RGB */

#if USING_SW_YUV_RGB
    ResetTime();
    chroma::NV21ToRGB32(yuvData, ion_rgb_buf.start, testWidth, testHeight);
    printf("NV21ToRGB32 time: %f\n", GetTime());
    free(yuvData);
#endif

    FillRectangle(fd_ge2d, 0, 0, screenWidth, screenHeight, BLUE); // RGBA

    // ----- RENDERING -----
    int frames = 0;
    float totalTime = 0;
    isRunning = true;

    ResetTime();

    printf("Start displaying YUV file. Exit by Ctrl+C...\n");

    // Trap signal to clean up
    signal(SIGINT, SignalHandler);
    do {
        BlitTestPattern(fd_ge2d, 0, 0, screenWidth, screenHeight);
        // BlitTestPattern(fd_ge2d, testWidth, testHeight, screenWidth, screenHeight);
        // BlitTestPattern(fd_ge2d, 0, testHeight, screenWidth, screenHeight);
        // BlitTestPattern(fd_ge2d, testWidth, 0, screenWidth, screenHeight);

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
          int fps = (int)(frames / totalTime);
          printf("FPS: %i, %dframes/%fs\n", fps, frames, totalTime);

          frames = 0;
          totalTime = 0;
        }
    } while (isRunning != 0);

    close(fbfd);
    close(fd_ge2d);
    if (ion_fd >= 0) {
        // free_ion_buffer(ion_fd, ion_buf.handle);
        free_ion_buffer(ion_fd, ion_rgb_buf.handle);
        close(ion_fd);
    }

    printf("-EXIT!!!-\n");

    return 0;
}



int xioctl(int fd, int request, void *arg) {
  int r;

  do {
    r = ioctl(fd, request, arg);
  } while (r == -1 && errno == EINTR);

  return r;
}

void print_ion_buffer(ion_buffer *ion_buf) {
  printf(
      "ION Buffer:\n"
      "\tAddr: %#010x\n"
      "\tPhys Addr: %#010x\n"
      "\tLength: %d\n"
      "\tHandle: %d\n",
      ion_buf->start,
      ion_buf->phys_addr,
      ion_buf->length,
      ion_buf->handle);
}

void alloc_ion_buffer(int ion_fd, ion_buffer *ion_buf, int size) {
  // ION data structures
  struct ion_allocation_data ion_alloc_data;
  struct ion_fd_data ion_data;
  struct meson_phys_data meson_phys_data;
  struct ion_custom_data ion_custom_data;

  CLEAR(ion_alloc_data);

  ion_alloc_data.len = size;

  ion_alloc_data.heap_id_mask = ION_HEAP_CARVEOUT_MASK;
  ion_alloc_data.flags = 0;
  if (xioctl(ion_fd, ION_IOC_ALLOC, &ion_alloc_data) < 0)
    perror("ION_IOC_ALLOC Failed");

  CLEAR(ion_data);
  ion_data.handle = ion_alloc_data.handle;
  if (xioctl(ion_fd, ION_IOC_SHARE, &ion_data) < 0)
    perror("ION_IOC_SHARE Failed");

  CLEAR(meson_phys_data);
  meson_phys_data.handle = ion_data.fd;

  CLEAR(ion_custom_data);
  ion_custom_data.cmd = ION_IOC_MESON_PHYS_ADDR;
  ion_custom_data.arg = (long unsigned int)&meson_phys_data;
  if (xioctl(ion_fd, ION_IOC_CUSTOM, &ion_custom_data) < 0)
    perror("IOC_IOC_MESON_PHYS_ADDR Failed");

  ion_buf->start = mmap(
      NULL,
      ion_alloc_data.len,
      PROT_READ | PROT_WRITE,
      MAP_FILE | MAP_SHARED,
      ion_data.fd,
      0);

  if (ion_buf->start == MAP_FAILED)
    perror("Mapping ION memory failed");

  ion_buf->handle = ion_alloc_data.handle;
  ion_buf->length = ion_alloc_data.len;
  ion_buf->phys_addr = meson_phys_data.phys_addr;

  print_ion_buffer(ion_buf);
}

void free_ion_buffer(int ion_fd, ion_user_handle_t handle) {
  struct ion_handle_data ion_handle_data;

  printf("Freeing ION buffer with handle: %d\n", handle);

  CLEAR(ion_handle_data);
  ion_handle_data.handle = handle;

  if (xioctl(ion_fd, ION_IOC_FREE, &ion_handle_data) < 0)
    perror("ION_IOC_FREE failed");
}

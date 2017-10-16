# HelloGE2D
Simple demonstration of GE2D hardware blit engine on Odroid-C2

This example using fb1 physical memory to store YUV file (for 4K YUV, its size is 3840 * 2160 * 1.5 bytes), then the memory size of fb1 need to be extended.
In DTS file, update:
```
meson-fb {
		compatible = "amlogic, meson-fb";
		memory-region = <&fb_reserved>;
		dev_name = "meson-fb";
		status = "okay";
		interrupts = <0 3 1
					  0 89 1>;
		interrupt-names = "viu-vsync", "rdma";
		mem_size = <0x06000000 0x01000000>; /* fb0/fb1 memory size */
		vmode = <3>;	/* 1080p60hz */
		scale_mode = <1>;
		4k2k_fb = <1>;
		display_size_default = <3840 2160 3840 4320 32>;
	};
```
compile to DTB file then replace existing in /media/boot

To compile:
```
  make
```

To run (you must be root to run):
```
  sudo ./HelloGE2D
```


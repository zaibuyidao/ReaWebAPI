# ReaWebAPI

- Fix video preview falling back to web rendering when native I420 is unsupported. Retry native RGBA to avoid unnecessary transfer and display latency.
- Support native YUV/GPU presentation for valid even-sized video frames whose widths are not multiples of 16, preserving CPU conversion restrictions.

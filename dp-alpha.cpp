#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include <drm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <iostream>
#include <system_error>
#include <memory>

class autofd {
public:
	autofd(const char *file, int flags) {
		_fd = ::open(file, flags);
		if (_fd == -1)
			throw std::system_error(errno, std::system_category(),
						"Failed to open file");
	}
	autofd(const autofd&) = delete;
	autofd &operator = (const autofd&) = delete;
	autofd(autofd&&) = default;
	~autofd() {
		::close(_fd);
	}
	operator int() const {
		return _fd;
	}
private:
	int _fd;
};

class DrmBuffer {
public:
	DrmBuffer(int fd, uint32_t width, uint32_t height)
		: _fd(fd), _width(width), _height(height) {
		create();
	}

	~DrmBuffer() {
		cleanup();
	}

	DrmBuffer(const DrmBuffer&) = delete;
	DrmBuffer& operator=(const DrmBuffer&) = delete;

	uint32_t fb_id() const { return _fb_id; }

	void fill_chessboard(bool premultiplied) {
		uint32_t *pixels = (uint32_t *)_mapped;
		const int sq_sz = 64;
		const uint32_t white = premultiplied ?  0x80808080 : 0x80ffffff;
		const uint32_t grey = 0xff808080;

		for (uint32_t y = 0; y < _height; y++)
			for (uint32_t x = 0; x < _width; x++)
				pixels[y * (_pitch / 4) + x] =
					((x / sq_sz) + (y / sq_sz)) % 2 ?
						grey : white;
	}

private:
	void create() {
		// Create dumb buffer
		struct drm_mode_create_dumb create_req = {};
		create_req.width = _width;
		create_req.height = _height;
		create_req.bpp = 32;

		if (drmIoctl(_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0)
			throw std::system_error(errno, std::system_category(),
						"Failed to create dumb buffer");

		_handle = create_req.handle;
		_pitch = create_req.pitch;
		_size = create_req.size;

		// Create framebuffer
		uint32_t handles[4] = {_handle, 0, 0, 0};
		uint32_t pitches[4] = {_pitch, 0, 0, 0};
		uint32_t offsets[4] = {0, 0, 0, 0};

		if (drmModeAddFB2(_fd, _width, _height, _format, handles,
				  pitches, offsets, &_fb_id, 0) < 0) {
			drm_mode_destroy_dumb destroy_req { _handle };
			drmIoctl(_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
			throw std::system_error(errno, std::system_category(),
						"Failed to add framebuffer");
		}

		// Map the buffer
		drm_mode_map_dumb map_req = {};
		map_req.handle = _handle;

		if (drmIoctl(_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
			drmModeRmFB(_fd, _fb_id);
			drm_mode_destroy_dumb destroy_req = { _handle };
			drmIoctl(_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
			throw std::system_error(errno, std::system_category(),
						"Failed to map dumb buffer");
		}

		_mapped = ::mmap(0, _size, PROT_READ | PROT_WRITE, MAP_SHARED,
				 _fd, map_req.offset);
		if (_mapped == MAP_FAILED) {
			drmModeRmFB(_fd, _fb_id);
			drm_mode_destroy_dumb destroy_req = {_handle};
			drmIoctl(_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
			throw std::system_error(errno, std::system_category(),
						"Failed to mmap framebuffer");
		}
	}

	void cleanup() {
		if (_mapped && _mapped != MAP_FAILED)
			::munmap(_mapped, _size);
		if (_fb_id)
			drmModeRmFB(_fd, _fb_id);
		if (_handle) {
			drm_mode_destroy_dumb destroy_req = { _handle };
			drmIoctl(_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
		}
	}

	int _fd;
	uint32_t _width;
	uint32_t _height;
	const uint32_t _format = DRM_FORMAT_ARGB8888;
	uint32_t _fb_id = 0;
	uint32_t _handle;
	uint32_t _pitch;
	size_t _size;
	void *_mapped = nullptr;
};

class DrmDevice {
public:
	DrmDevice(const char* device_path = "/dev/dri/card0")
		: _fd(device_path, O_RDWR) {
		initialize();
	}

	~DrmDevice() {
		cleanup();
	}

	DrmDevice(const DrmDevice&) = delete;
	DrmDevice& operator=(const DrmDevice&) = delete;

	int fd() const { return _fd; }

	std::unique_ptr<DrmBuffer> create_buffer() {
		return std::make_unique<DrmBuffer>(_fd, _mode.hdisplay, _mode.vdisplay);
	}

	void set_crtc(uint32_t fb_id) {
		if (drmModeSetCrtc(_fd, _crtc_id, fb_id, 0, 0,
				   &_connector->connector_id, 1, &_mode) < 0) {
			throw std::system_error(errno, std::system_category(),
						"Failed to set CRTC");
		}
	}

private:
	void initialize() {
		// Get DRM resources
		_resources = drmModeGetResources(_fd);
		if (!_resources)
			throw std::runtime_error("Failed to get DRM resources");

		// Find first connected connector
		for (int i = 0; i < _resources->count_connectors; i++) {
			_connector = drmModeGetConnector(_fd, _resources->connectors[i]);
			if (_connector->connection == DRM_MODE_CONNECTED)
				break;
			drmModeFreeConnector(_connector);
			_connector = nullptr;
		}

		if (!_connector)
			throw std::runtime_error("No connected connector found");

		// Get the preferred mode
		_mode = _connector->modes[0];

		// Find encoder and CRTC
		_encoder = drmModeGetEncoder(_fd, _connector->encoder_id);
		if (!_encoder)
			throw std::runtime_error("Failed to get encoder");

		_crtc_id = _encoder->crtc_id;
		if (!_crtc_id) {
			// Find a CRTC
			for (int i = 0; i < _resources->count_crtcs; i++) {
				if (_encoder->possible_crtcs & (1 << i)) {
					_crtc_id = _resources->crtcs[i];
					break;
				}
			}
		}

		if (!_crtc_id)
			throw std::runtime_error("Failed to find CRTC");
	}

	void cleanup() {
		if (_encoder)
			drmModeFreeEncoder(_encoder);
		if (_connector)
			drmModeFreeConnector(_connector);
		if (_resources)
			drmModeFreeResources(_resources);
	}

	autofd _fd;
	drmModeConnector *_connector = nullptr;
	drmModeEncoder *_encoder = nullptr;
	drmModeRes *_resources = nullptr;
	drmModeModeInfo _mode = {};
	uint32_t _crtc_id = 0;
};

int usage() {
	std::cout << "Usage: dp-alpha p|s" << std::endl;
	std::cout << "\tp: use premultiplied alpha" << std::endl;
	std::cout << "\ts: use straight alpha" << std::endl;

	return -1;
}

int main(int argc, char** argv) {
	if (argc != 2)
		return usage();

	bool premultiplied = false;
	if (argv[1][0] == 'p')
		premultiplied = true;
	else if (argv[1][0] == 's')
		premultiplied = false;
	else
		return usage();

	try {
		DrmDevice drm_device;
		auto buffer = drm_device.create_buffer();
		buffer->fill_chessboard(premultiplied);
		drm_device.set_crtc(buffer->fb_id());

		std::cout << "If you see a chessboard pattern, "
			  << (premultiplied ? "premultiplied" : "straight")
			  << " alpha blending mode is incorrect." << std::endl;
		std::cin.get();
	} catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return -1;
	}

	return 0;
}
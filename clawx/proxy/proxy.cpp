#define DLL_NAME "PROXY"

// Include GLEW
#include <GL/glew.h>

#include "log.h"
#include "proxy.h"
#include "dump.h"

#include "json.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <SFML/Window.hpp>
#include <SFML/OpenGL.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <fstream>
#include <unordered_map>

#define config (GetProxy()->GetConfig())

using json = nlohmann::json;

const unsigned MAGIC = 0x1a1b1c00;

bool DISABLE_PROXY = false;

sf::Window *window;

template<typename T>
size_t h(const T* ptr) {
	const char *c = (const char *)ptr;
	int n = sizeof(T);
	std::size_t seed = 0;
	for (int i = 0; i < n; ++i) {
		char j = c[i];
		seed ^= j + 0x9e3779b9 + (seed << 6) + (seed >> 2);
	}
	return seed;
}

size_t h_ddsd00(DDSURFACEDESC *ddsd) {
	DDSURFACEDESC ddsd00 = *ddsd;
	ddsd00.dwWidth = ddsd00.dwHeight = 0;
	return h(&ddsd00);
}

#define PROXY_UNIMPLEMENTED() 0

#define DDRAW_PALETTE_PROXY(method)

struct DirectDrawPaletteProxy : public IDirectDrawPalette
{
	static const unsigned PALETTE_SIZE = 256;

	std::vector<PALETTEENTRY> entries;
	std::vector<byte> texture_data;

	GLuint texture;

	DirectDrawPaletteProxy() {
		entries.resize(PALETTE_SIZE);
		texture_data.resize(PALETTE_SIZE * 4);

		glGenTextures(1, &texture);

		glBindTexture(GL_TEXTURE_1D, texture);
		glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

		assert(!glGetError());
	}

	/*** IUnknown methods ***/
	STDMETHOD(QueryInterface) (THIS_ REFIID riid, LPVOID FAR * ppvObj) {
		DDRAW_PALETTE_PROXY(QueryInterface);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD_(ULONG, AddRef) (THIS) {
		DDRAW_PALETTE_PROXY(AddRef);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD_(ULONG, Release) (THIS) {
		DDRAW_PALETTE_PROXY(Release);
		return PROXY_UNIMPLEMENTED();
	}
	/*** IDirectDrawPalette methods ***/
	STDMETHOD(GetCaps)(THIS_ LPDWORD) {
		DDRAW_PALETTE_PROXY(GetCaps);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(GetEntries)(
		DWORD          dwFlags,
		DWORD          dwBase,
		DWORD          dwNumEntries,
		LPPALETTEENTRY lpEntries
	) {
		DDRAW_PALETTE_PROXY(GetEntries);

		assert(dwBase == 0 && dwNumEntries == PALETTE_SIZE);

		for (int i = 0; i < PALETTE_SIZE; ++i) {
			lpEntries[i] = entries[i];
		}

		return S_OK;
	}
	STDMETHOD(Initialize)(THIS_ LPDIRECTDRAW, DWORD, LPPALETTEENTRY) {
		DDRAW_PALETTE_PROXY(Initialize);
		return PROXY_UNIMPLEMENTED();
	}

	void UpdateTexture() {
		for (int i = 0; i < PALETTE_SIZE; ++i) {
			byte *px = &texture_data[i * 4];
			auto &e = entries[i];
			px[0] = e.peRed;
			px[1] = e.peGreen;
			px[2] = e.peBlue;
			px[3] = 0xFF;
		}

		texture_data[0] = texture_data[1] = texture_data[2] = texture_data[3] = 0;

		glBindTexture(GL_TEXTURE_1D, texture);
		glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA, PALETTE_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, texture_data.data());

		assert(!glGetError());
	}

	STDMETHOD(SetEntries)(
		DWORD          dwFlags,
		DWORD          dwStartingEntry,
		DWORD          dwCount,
		LPPALETTEENTRY lpEntries
	) {
		DDRAW_PALETTE_PROXY(SetEntries);

		assert(dwStartingEntry == 0 && dwCount == PALETTE_SIZE);

		for (int i = 0; i < PALETTE_SIZE; ++i) {
			entries[i] = lpEntries[i];
		}

		UpdateTexture();

		return S_OK;
	}
};

int fileExists(TCHAR * file)
{
	WIN32_FIND_DATA FindFileData;
	HANDLE handle = FindFirstFile(file, &FindFileData);
	int found = handle != INVALID_HANDLE_VALUE;
	if (found)
	{
		//FindClose(&handle); this will crash
		FindClose(handle);
	}
	return found;
}

size_t ddsd00_h(LPDDSURFACEDESC ddsd) {
	DDSURFACEDESC ddsd00 = *ddsd;
	ddsd00.dwWidth = ddsd00.dwHeight = 0;
	ddsd00.lpSurface = nullptr;
	return h(&ddsd00);
}

class DirectDrawProxy;
class DirectDrawSurfaceProxy;

DirectDrawSurfaceProxy *front_buffer;
DirectDrawSurfaceProxy *back_buffer;

inline int pow2roundup(int x)
{
	if (x < 0)
		return 0;
	--x;
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;
	return x + 1;
}

void log_surface_call(const char *method, DirectDrawSurfaceProxy* ddsp);

#define DDRAW_SURFACE_PROXY(method) log_surface_call(#method, this);

#define _DDSCL_NORMAL 0

std::vector<char> buffer(1920 * 1080 * 4);

GLuint shaderProgram;
GLuint shaderProgram2;

class DirectDrawSurfaceProxy : public IDirectDrawSurface3
{
public:
	enum Kind {
		NORMAL_SURFACE,
		FRONT_BUFFER,
		BACK_BUFFER
	};

	int magic = MAGIC;

	DirectDrawProxy *ddp = nullptr;
	Kind kind;
	size_t d_h = 0;
	int width = 0;
	int height = 0;
	int pitch = 0;

	std::vector<UCHAR> indexed_buffer;

	DirectDrawPaletteProxy *ddpp = nullptr;

	GLuint vao;
	GLuint texture;
	GLuint frameBuffer = 0;

	DirectDrawSurfaceProxy(DirectDrawProxy *ddp, Kind kind, size_t d_h, int width, int height, int pitch) {
		this->ddp = ddp;
		this->kind = kind;
		this->d_h = d_h;
		this->width = width;
		this->height = height;
		pitch = pow2roundup(width);
		pitch = width;
		this->pitch = pitch;

		indexed_buffer.resize(pitch * height); // ...

											  // Create Vertex Array Object
		glGenVertexArrays(1, &vao);
		glBindVertexArray(vao);

		// Create an element array
		GLuint ebo;
		glGenBuffers(1, &ebo);

		GLuint elements[] = {
			0, 1, 2,
			2, 3, 0
		};

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(elements), elements, GL_STATIC_DRAW);

		// Create a Vertex Buffer Object and copy the vertex data to it
		GLuint vbo;
		glGenBuffers(1, &vbo);

		int L = 0;
		float R = float(width) / this->pitch;
		int T = 0;
		int B = 1;

		GLfloat vertices[] = {
			//  Position      Texcoords
			0,		0,		L, T, // Top-left
			width,	0,		R, T, // Top-right
			width,	height, R, B, // Bottom-right
			0,		height, L, B  // Bottom-left
		};

		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

		assert(!glGetError());

		GLuint program = (kind == FRONT_BUFFER) ? shaderProgram2 : shaderProgram;

		// Specify the layout of the vertex data
		GLint posAttrib = glGetAttribLocation(program, "position");
		if (posAttrib >= 0) {
			glEnableVertexAttribArray(posAttrib);
			glVertexAttribPointer(posAttrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), 0);
		}
		assert(!glGetError());

		GLint texAttrib = glGetAttribLocation(program, "texcoord");
		if (texAttrib >= 0) {
			glEnableVertexAttribArray(texAttrib);
			glVertexAttribPointer(texAttrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)(2 * sizeof(GLfloat)));
		}
		
		assert(!glGetError());
		// Load textures
		glGenTextures(1, &texture);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, texture);
		
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

		glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, pitch, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

		assert(!glGetError());
	}

	void GenFramebuffer() {
		if (frameBuffer == 0) {
			glGenFramebuffers(1, &frameBuffer);
			glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
		}
	}

	/*** IUnknown methods ***/
	STDMETHOD(QueryInterface) (THIS_ REFIID riid, LPVOID FAR * ppvObj) {
		DDRAW_SURFACE_PROXY(QueryInterface);

		*ppvObj = this;

		return S_OK;
	}
	STDMETHOD_(ULONG, AddRef) (THIS) {
		DDRAW_SURFACE_PROXY(AddRef);
		return S_OK;
	}
	STDMETHOD_(ULONG, Release) (THIS) {
		DDRAW_SURFACE_PROXY(Release);
		return S_OK;
	}
	/*** IDirectDrawSurface methods ***/
	STDMETHOD(AddAttachedSurface)(THIS_ LPDIRECTDRAWSURFACE3) {
		DDRAW_SURFACE_PROXY(AddAttachedSurface);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(AddOverlayDirtyRect)(THIS_ LPRECT) {
		DDRAW_SURFACE_PROXY(AddOverlayDirtyRect);
		return PROXY_UNIMPLEMENTED();
	}

	void Dump() {
		log(img_dump(pitch, height, indexed_buffer.data()));
	}

	void Draw(GLuint frameBuffer, int x, int y, GLuint palette_texture) {
		GLuint program = palette_texture ? shaderProgram2 : shaderProgram;

		glUseProgram(program);
		glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer);

		assert(!glGetError());

		glm::mat4 pos;
		pos = glm::translate(pos, { x, y, 0 });

		auto uniPos = glGetUniformLocation(program, "pos");
		if(uniPos >= 0)
			glUniform2f(uniPos, x, y);

		assert(!glGetError());

		glm::mat4 trans;

		if (frameBuffer == 0) {
			trans = glm::scale(trans, { 1, -1, 1 });
		}

		trans = glm::translate(trans, { -1, -1, 0 });

		float sx = 2 / 640.f;
		float sy = 2 / 480.f;

		trans = glm::scale(trans, { sx, sy, 1 });

		GLint uniTrans = glGetUniformLocation(program, "trans");
		if(uniTrans >= 0)
			glUniformMatrix4fv(uniTrans, 1, GL_FALSE, glm::value_ptr(trans));

		assert(!glGetError());

		glBindVertexArray(vao);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, texture);

		assert(!glGetError());

		if (palette_texture) {
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_1D, palette_texture);
		}
		
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

		assert(!glGetError());
	}

	HRESULT BltGeneric(int x, int y, LPDIRECTDRAWSURFACE3 dds) {
		DirectDrawSurfaceProxy *ddsp = (DirectDrawSurfaceProxy *)dds;

		bool disable_log = config["disable_log"];

		if (!disable_log) {
			log(json_dump({
				{ "x", x },{ "y", y },{ "w", ddsp->width },{ "h", ddsp->height }
			}));
		}


		if (kind == NORMAL_SURFACE) {
			return S_OK; // ???
		}

		GenFramebuffer();

		ddsp->Draw(frameBuffer, x, y, 0);

		bool blt_dump_ddsp = config["blt_dump_ddsp"];
		bool blt_dump_this = config["blt_dump_this"];

		if (blt_dump_ddsp)
			ddsp->Dump();

		if (blt_dump_this) {
			Stall();
			this->Dump();
		}

		return S_OK;
	}

	STDMETHOD(Blt)(
		 LPRECT               lpDestRect,
		 LPDIRECTDRAWSURFACE3 lpDDSrcSurface,
		 LPRECT               lpSrcRect,
		 DWORD                dwFlags,
		 LPDDBLTFX            lpDDBltFx
	) {
		DDRAW_SURFACE_PROXY(Blt);

		if (!lpDDSrcSurface) {
			return S_OK;
		}

		int x = 0;
		int y = 0;

		if (lpDestRect) {
			x = lpDestRect->left;
			y = lpDestRect->top;
		}

		if (lpSrcRect) {
			x -= lpSrcRect->left;
			y -= lpSrcRect->top;
		}

		return BltGeneric(x, y, lpDDSrcSurface);
	}

	STDMETHOD(BltBatch)(THIS_ LPDDBLTBATCH, DWORD, DWORD) {
		DDRAW_SURFACE_PROXY(BltBatch);
		return PROXY_UNIMPLEMENTED();
	}

	STDMETHOD(BltFast)(THIS_
		DWORD                dwX,
		DWORD                dwY,
		LPDIRECTDRAWSURFACE3 lpDDSrcSurface,
		LPRECT               lpSrcRect,
		DWORD                dwFlags
	) {
		DDRAW_SURFACE_PROXY(BltFast);

		int x = dwX;
		int y = dwY;

		if (lpSrcRect) {
			x -= lpSrcRect->left;
			y -= lpSrcRect->top;
		}

		return BltGeneric(x, y, lpDDSrcSurface);
	}

	STDMETHOD(DeleteAttachedSurface)(THIS_ DWORD, LPDIRECTDRAWSURFACE3) {
		DDRAW_SURFACE_PROXY(DeleteAttachedSurface);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(EnumAttachedSurfaces)(THIS_ LPVOID, LPDDENUMSURFACESCALLBACK) {
		DDRAW_SURFACE_PROXY(EnumAttachedSurfaces);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(EnumOverlayZOrders)(THIS_ DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) {
		DDRAW_SURFACE_PROXY(EnumOverlayZOrders);
		return PROXY_UNIMPLEMENTED();
	}
	
	void Clear() {
		glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer);
		glClear(GL_COLOR_BUFFER_BIT);
	}

	STDMETHOD(Flip)(THIS_ LPDIRECTDRAWSURFACE3 a, DWORD b) {
		DDRAW_SURFACE_PROXY(Flip);

		assert(kind == FRONT_BUFFER);

		static int i = 0;
		++i;
		if (i % 5 == 0)
			GetProxy()->ReloadConfig();

		GenFramebuffer();

		back_buffer->Draw(frameBuffer, 0, 0, 0);
		back_buffer->Clear();

		if(ddpp)
			Draw(0, 0, 0, ddpp->texture);

		window->display();

		glClear(GL_COLOR_BUFFER_BIT);

		return S_OK;
	}
	STDMETHOD(GetAttachedSurface)(THIS_ LPDDSCAPS a, LPDIRECTDRAWSURFACE3 FAR *out_dds) {
		DDRAW_SURFACE_PROXY(GetAttachedSurface);

		auto ddsp = new DirectDrawSurfaceProxy(ddp, BACK_BUFFER, d_h, width, height, width);
		*out_dds = ddsp;

		back_buffer = ddsp;
			
		return S_OK;
	}
	STDMETHOD(GetBltStatus)(THIS_ DWORD) {
		DDRAW_SURFACE_PROXY(GetBltStatus);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(GetCaps)(THIS_ LPDDSCAPS) {
		DDRAW_SURFACE_PROXY(GetCaps);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(GetClipper)(THIS_ LPDIRECTDRAWCLIPPER FAR*) {
		DDRAW_SURFACE_PROXY(GetClipper);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(GetColorKey)(THIS_ DWORD, LPDDCOLORKEY) {
		DDRAW_SURFACE_PROXY(GetColorKey);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(GetDC)(THIS_ HDC FAR *a) {
		DDRAW_SURFACE_PROXY(GetDC);

		*a = (HDC)1;

		return S_OK;
	}
	STDMETHOD(GetFlipStatus)(THIS_ DWORD) {
		DDRAW_SURFACE_PROXY(GetFlipStatus);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(GetOverlayPosition)(THIS_ LPLONG, LPLONG) {
		DDRAW_SURFACE_PROXY(GetOverlayPosition);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(GetPalette)(THIS_ LPDIRECTDRAWPALETTE FAR*) {
		DDRAW_SURFACE_PROXY(GetPalette);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(GetPixelFormat)(THIS_ LPDDPIXELFORMAT a) {
		DDRAW_SURFACE_PROXY(GetPixelFormat);
		return S_OK;
	}
	STDMETHOD(GetSurfaceDesc)(THIS_ LPDDSURFACEDESC a) {
		DDRAW_SURFACE_PROXY(GetSurfaceDesc);

		std::string s = "GetSurfaceDesc_" + std::to_string(d_h);

		Load(s, a);

		a->dwWidth = width;
		a->dwHeight = height;
		a->lPitch = pitch;

		return S_OK;
	}
	STDMETHOD(Initialize)(THIS_ LPDIRECTDRAW, LPDDSURFACEDESC) {
		DDRAW_SURFACE_PROXY(Initialize);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(IsLost)(THIS) {
		//DDRAW_SURFACE_PROXY(IsLost);
		return 0;
	}

	void Stall() {
		if (frameBuffer) {
			glBindTexture(GL_TEXTURE_2D, texture);
			glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, indexed_buffer.data());
		}
	}

	STDMETHOD(Lock)(THIS_ LPRECT a, LPDDSURFACEDESC b, DWORD c, HANDLE d) {
		DDRAW_SURFACE_PROXY(Lock);

		Stall();

		Load("Lock_" + std::to_string(d_h), b);
		b->dwWidth = width;
		b->dwHeight = height;
		b->lPitch = pitch;

		b->lpSurface = indexed_buffer.data();

		//dump("_lock");

		assert(!glGetError());

		return S_OK;
	}
	STDMETHOD(ReleaseDC)(THIS_ HDC a) {
		DDRAW_SURFACE_PROXY(ReleaseDC);
		return S_OK;
	}
	STDMETHOD(Restore)(THIS) {
		DDRAW_SURFACE_PROXY(Restore);
		return S_OK;
	}
	STDMETHOD(SetClipper)(THIS_ LPDIRECTDRAWCLIPPER) {
		DDRAW_SURFACE_PROXY(SetClipper);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(SetColorKey)(THIS_ DWORD a, LPDDCOLORKEY b) {
		DDRAW_SURFACE_PROXY(SetColorKey);
		return S_OK;
	}
	STDMETHOD(SetOverlayPosition)(THIS_ LONG, LONG) {
		DDRAW_SURFACE_PROXY(SetOverlayPosition);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(SetPalette)(THIS_ LPDIRECTDRAWPALETTE a) {
		DDRAW_SURFACE_PROXY(SetPalette);
		
		ddpp = (DirectDrawPaletteProxy *)a;

		return S_OK;
	}

	void dump(std::string p) {
#if 0
		if (indexed_buffer.size() && fileExists("dump")) {
			static int i = 0;
			int p = width;
			if (pitch > 0 && pitch < 1024) p = pitch;

			auto rgbab = indexed2rgba((UCHAR*)indexed_buffer.data(), width, height, p);

			char t[64];

			const char *tp = "sf";
			if (kind == FRONT_BUFFER) tp = "fb";
			if (kind == BACK_BUFFER) tp = "bb";

			sprintf_s(t, "d/%05d_%p_%s_%s.png", i++, this, tp, unlock ? "unlock": "lock");

			stbi_write_bmp(t, width, height, 4, rgbab.data());
		}
#else
		if (fileExists("dump")) {
			static int i = 0;

			char t[64];

			const char *tp = "sf";
			if (kind == FRONT_BUFFER) tp = "fb";
			if (kind == BACK_BUFFER) tp = "bb";

			sprintf_s(t, "d/%05d_%p_%s_%s.bmp", i++, this, tp, p.c_str());

			//stbi_write_bmp(t, width, height, 4, pixels);
		}
#endif
	}

	STDMETHOD(Unlock)(THIS_ LPVOID a) {
		DDRAW_SURFACE_PROXY(Unlock);

		glBindTexture(GL_TEXTURE_2D, texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, pitch, height, 0, GL_RED, GL_UNSIGNED_BYTE, indexed_buffer.data());

		assert(!glGetError());


		if (kind == FRONT_BUFFER) {
			assert(ddpp && ddpp->texture);
			Draw(0, 0, 0, ddpp->texture);
		}

		if (config["unlock_dump_this"])
			this->Dump();
			
		return S_OK;
	}

	STDMETHOD(UpdateOverlay)(THIS_ LPRECT, LPDIRECTDRAWSURFACE3, LPRECT, DWORD, LPDDOVERLAYFX) {
		DDRAW_SURFACE_PROXY(UpdateOverlay);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(UpdateOverlayDisplay)(THIS_ DWORD) {
		DDRAW_SURFACE_PROXY(UpdateOverlayDisplay);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(UpdateOverlayZOrder)(THIS_ DWORD, LPDIRECTDRAWSURFACE3) {
		DDRAW_SURFACE_PROXY(UpdateOverlayZOrder);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(GetDDInterface)(THIS_ LPVOID FAR *) {
		DDRAW_SURFACE_PROXY(GetDDInterface);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(PageLock)(THIS_ DWORD) {
		DDRAW_SURFACE_PROXY(PageLock);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(PageUnlock)(THIS_ DWORD) {
		DDRAW_SURFACE_PROXY(PageUnlock);
		return PROXY_UNIMPLEMENTED();
	}
	/*** Added in the V3 interface ***/
	STDMETHOD(SetSurfaceDesc)(THIS_ LPDDSURFACEDESC a, DWORD b) {
		DDRAW_SURFACE_PROXY(SetSurfaceDesc);
		return PROXY_UNIMPLEMENTED();
	}
};

void log_surface_call(const char *method, DirectDrawSurfaceProxy* ddsp) {
	bool disable_log = config["disable_log"];

	if (disable_log) {
		return;
	}

	log_call("DirectDrawSurfaceProxy", method, ddsp);

	std::string t = "surface";

	if (ddsp->kind == DirectDrawSurfaceProxy::FRONT_BUFFER) {
		t = "frontbuffer";
	}
	else if (ddsp->kind == DirectDrawSurfaceProxy::BACK_BUFFER) {
		t = "backbuffer";
	}

	log(tag("span", { { "class", t } }, t) + '\n');
}

std::string read_file(std::string filename) {
	std::ifstream f(filename.c_str());
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

GLuint create_shader(GLenum shader_type, std::string shader_name) {
	// Shader sources
	std::string source = read_file(shader_name);
	auto sourcePtr = source.c_str();

	// Create and compile the vertex shader
	GLuint shader = glCreateShader(shader_type);
	glShaderSource(shader, 1, &sourcePtr, NULL);
	glCompileShader(shader);

	char shader_log[1024];
	glGetShaderInfoLog(shader, 1024, NULL, shader_log);

	if (shader_log[0]) {
		MessageBox(0, shader_log, shader_name.c_str(), 0);
	}

	assert(!glGetError());

	return shader;
}

GLuint create_program(GLuint vertex_shader, GLuint fragment_shader) {
	GLuint shaderProgram = glCreateProgram();
	glAttachShader(shaderProgram, vertex_shader);
	glAttachShader(shaderProgram, fragment_shader);

	glBindFragDataLocation(shaderProgram, 0, "outColor");

	glLinkProgram(shaderProgram);
	glUseProgram(shaderProgram);

	char shaderProgramLog[1024];
	glGetProgramInfoLog(shaderProgram, 1024, NULL, shaderProgramLog);

	if (shaderProgramLog[0]) {
		MessageBox(0, shaderProgramLog, "shader program", 0);
	}

	assert(!glGetError());

	return shaderProgram;
}

#define DDRAW_PROXY(method) log_call("DirectDrawProxy", #method, this);

struct DirectDrawProxy : public IDirectDraw2
{
	DirectDrawSurfaceProxy *GetFrontBuffer() {
		return front_buffer;
	}

	DirectDrawSurfaceProxy *GetBackBuffer() {
		return back_buffer;
	}

	void InitGl() {
		glewExperimental = GL_TRUE;
		glewInit();

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		glViewport(0, 0, 640, 480);
	}

	void LoadSurfaceProgram() {
		GLuint vertexShader = create_shader(GL_VERTEX_SHADER, "vertexShader.vert");
		GLuint fragmentShader = create_shader(GL_FRAGMENT_SHADER, "fragmentShader.frag");

		shaderProgram = create_program(vertexShader, fragmentShader);

		assert(!glGetError());

		GLint u = glGetUniformLocation(shaderProgram, "surface_texture");
		if (u >= 0)
			glUniform1i(u, 0);

		assert(!glGetError());
	}

	void LoadFrontbufferProgram() {
		assert(!glGetError());

		GLuint vertexShader = create_shader(GL_VERTEX_SHADER, "vertexShader.vert");
		GLuint fragmentShader2 = create_shader(GL_FRAGMENT_SHADER, "fragmentShader2.frag");

		shaderProgram2 = create_program(vertexShader, fragmentShader2);

		assert(!glGetError());

		GLint u;

		u = glGetUniformLocation(shaderProgram2, "surface_texture");
		if (u >= 0)
			glUniform1i(u, 0);

		assert(!glGetError());

		u = glGetUniformLocation(shaderProgram2, "palette_texture");
		if (u >= 0)
			glUniform1i(u, 1);

		assert(!glGetError());
	}

	DirectDrawProxy() {
		InitGl();

		LoadSurfaceProgram();
		LoadFrontbufferProgram();
	}

	/*** IUnknown methods ***/
	STDMETHOD(QueryInterface) (THIS_ REFIID riid, LPVOID FAR * ppvObj) {
		DDRAW_PROXY(QueryInterface);

		*ppvObj = this;

		return S_OK;
	}

	STDMETHOD_(ULONG, AddRef) (THIS) {
		DDRAW_PROXY(AddRef);
		return S_OK;
	}

	STDMETHOD_(ULONG, Release) (THIS) {
		DDRAW_PROXY(Release);
		return S_OK;
	}

	/*** IDirectDraw methods ***/
	STDMETHOD(Compact)(THIS) {
		DDRAW_PROXY(Compact);
		return PROXY_UNIMPLEMENTED();
	}

	STDMETHOD(CreateClipper)(THIS_ DWORD a, LPDIRECTDRAWCLIPPER FAR* b, IUnknown FAR *c) {
		DDRAW_PROXY(CreateClipper);
		return PROXY_UNIMPLEMENTED();
	}

	STDMETHOD(CreatePalette)(
			DWORD                   dwFlags,
			LPPALETTEENTRY          lpDDColorArray,
			LPDIRECTDRAWPALETTE FAR *lplpDDPalette,
			IUnknown FAR            *pUnkOuter
	) {
		DDRAW_PROXY(CreatePalette);

		auto ddpp = new DirectDrawPaletteProxy();
		*lplpDDPalette = ddpp;

		ddpp->SetEntries(dwFlags, 0, DirectDrawPaletteProxy::PALETTE_SIZE, lpDDColorArray);
		
		return S_OK;
	}

	STDMETHOD(CreateSurface)(THIS_  LPDDSURFACEDESC ddsd, LPDIRECTDRAWSURFACE FAR *out_dds, IUnknown FAR *c) {
		DDRAW_PROXY(CreateSurface);

		DirectDrawSurfaceProxy::Kind kind = DirectDrawSurfaceProxy::NORMAL_SURFACE;

		int width = ddsd->dwWidth;
		int height = ddsd->dwHeight;

		if (ddsd->dwBackBufferCount) {
			kind = DirectDrawSurfaceProxy::FRONT_BUFFER;
			width = config["backbuffer_w"];
			height = config["backbuffer_h"];
		}

		size_t d_h = kind == DirectDrawSurfaceProxy::FRONT_BUFFER ? h(ddsd) : ddsd00_h(ddsd);

		DirectDrawSurfaceProxy *ddsp = new DirectDrawSurfaceProxy(this, kind, d_h, width, height, width);

		if (kind == DirectDrawSurfaceProxy::FRONT_BUFFER) {
			front_buffer = ddsp;
		}

		log(ptr(ddsp));

		*out_dds = (IDirectDrawSurface*)ddsp;

		return S_OK;
	}

	STDMETHOD(DuplicateSurface)(THIS_ LPDIRECTDRAWSURFACE a, LPDIRECTDRAWSURFACE FAR *b) {
		DDRAW_PROXY(DuplicateSurface);
		return PROXY_UNIMPLEMENTED();
	}

	STDMETHOD(EnumDisplayModes)(THIS_ DWORD a, LPDDSURFACEDESC b, LPVOID user_ptr, LPDDENUMMODESCALLBACK callback) {
		DDRAW_PROXY(EnumDisplayModes);

		DDSURFACEDESC ddsd;

		Load("EnumDisplayModes_0", &ddsd);
		callback(&ddsd, user_ptr);

		Load("EnumDisplayModes_1", &ddsd);
		callback(&ddsd, user_ptr);

		Load("EnumDisplayModes_2", &ddsd);
		callback(&ddsd, user_ptr);

		return S_OK;
	}

	STDMETHOD(EnumSurfaces)(THIS_ DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) {
		DDRAW_PROXY(EnumSurfaces);
		return PROXY_UNIMPLEMENTED();
	}

	STDMETHOD(FlipToGDISurface)(THIS) {
		DDRAW_PROXY(FlipToGDISurface);
		return PROXY_UNIMPLEMENTED();
	}

	STDMETHOD(GetCaps)(THIS_ LPDDCAPS a, LPDDCAPS b) {
		DDRAW_PROXY(GetCaps);

		Load("GetCaps_a", a);
		Load("GetCaps_b", a);

		return S_OK;
	}

	STDMETHOD(GetDisplayMode)(THIS_ LPDDSURFACEDESC a) {
		DDRAW_PROXY(GetDisplayMode);

		Load("EnumDisplayModes_0", a);

		a->lPitch = 1024;

		return S_OK;
	}

	STDMETHOD(GetFourCCCodes)(THIS_  LPDWORD, LPDWORD) {
		DDRAW_PROXY(GetFourCCCodes);
		return PROXY_UNIMPLEMENTED();
	}
	STDMETHOD(GetGDISurface)(THIS_ LPDIRECTDRAWSURFACE FAR *) {
		DDRAW_PROXY(GetGDISurface);
		return PROXY_UNIMPLEMENTED();
	}

	STDMETHOD(GetMonitorFrequency)(THIS_ LPDWORD) {
		DDRAW_PROXY(GetMonitorFrequency);
		return PROXY_UNIMPLEMENTED();
	}

	STDMETHOD(GetScanLine)(THIS_ LPDWORD) {
		DDRAW_PROXY(GetScanLine);
		return PROXY_UNIMPLEMENTED();
	}

	STDMETHOD(GetVerticalBlankStatus)(THIS_ LPBOOL) {
		DDRAW_PROXY(GetVerticalBlankStatus);
		return PROXY_UNIMPLEMENTED();
	}

	STDMETHOD(Initialize)(THIS_ GUID FAR *) {
		DDRAW_PROXY(Initialize);
		return PROXY_UNIMPLEMENTED();
	}

	STDMETHOD(RestoreDisplayMode)(THIS) {
		DDRAW_PROXY(RestoreDisplayMode);
		return S_OK;
	}

	STDMETHOD(SetCooperativeLevel)(THIS_ HWND hWnd, DWORD flags) {
		DDRAW_PROXY(SetCooperativeLevel);
		return S_OK;
	}

	STDMETHOD(SetDisplayMode)(THIS_ DWORD w, DWORD h, DWORD c, DWORD d, DWORD e) {
		DDRAW_PROXY(SetDisplayMode);
		return S_OK;
	}

	STDMETHOD(WaitForVerticalBlank)(THIS_ DWORD a, HANDLE b) {
		DDRAW_PROXY(WaitForVerticalBlank);
		return S_OK;
	}

	/*** Added in the v2 interface ***/
	STDMETHOD(GetAvailableVidMem)(THIS_ LPDDSCAPS, LPDWORD, LPDWORD) {
		DDRAW_PROXY(GetAvailableVidMem);
		return PROXY_UNIMPLEMENTED();
	};
};

const char *html_head = R"HTML(
<head>
<link rel="stylesheet" type="text/css" href="style.css">
</head>
)HTML";

class Proxy : public IProxy {
	json _config;
	std::ofstream _log;

public:

	Proxy() {
		ReloadConfig();

		std::string log_filename = _config["log_filename"];
		_log.open(log_filename.c_str());

		_log << html_head;

		window = new sf::Window;
	}

	~Proxy() {}

	HWND CreateWindowExA(
		CreateWindowExA_type _CreateWindowExA,
		DWORD     dwExStyle,
		LPCTSTR   lpClassName,
		LPCTSTR   lpWindowName,
		DWORD     dwStyle,
		int       x,
		int       y,
		int       nWidth,
		int       nHeight,
		HWND      hWndParent,
		HMENU     hMenu,
		HINSTANCE hInstance,
		LPVOID    lpParam
	) {
		if (config["CreateWindowExA_disable_style"]) {
			dwStyle = 0;
		}

		nWidth = config["CreateWindowExA_nWidth"];
		nHeight = config["CreateWindowExA_nHeight"];

		HWND hWnd = _CreateWindowExA(
			dwExStyle,
			lpClassName,
			lpWindowName,
			dwStyle,
			x,
			y,
			nWidth,
			nHeight,
			hWndParent,
			hMenu,
			hInstance,
			lpParam
		);

		window->create(hWnd);

		bool vsync = config["vsync"];

		window->setVerticalSyncEnabled(vsync);

		return hWnd;
	}

	HRESULT DirectDrawProxyCreate(
		DirectDrawCreatePtr _DirectDrawCreate,
		GUID *lpGUID,
		LPDIRECTDRAW *lplpDD,
		IUnknown     *pUnkOuter
	) {
		auto ddp = new DirectDrawProxy();

		*lplpDD = (IDirectDraw *)ddp;

		return S_OK;
	}

	void ReloadConfig() {
		std::ifstream cfg_file;
		cfg_file.open("config.json");
		_config << cfg_file;
	}

	const json &GetConfig() {
		return _config;
	}

	void Log(std::string s) {
		bool disable_log = _config["disable_log"];

		if (disable_log) {
			return;
		}

		_log << s;
		_log.flush();
	}
};

PROXY_EXPORTS IProxy *GetProxy() {
	static Proxy *proxy = nullptr;
	if (!proxy) {
		proxy = new Proxy();
	}
	return proxy;
}

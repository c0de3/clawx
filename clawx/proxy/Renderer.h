#pragma once

#include "Palette.h"

#include <GL/glew.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <SFML/Window.hpp>
#include <SFML/OpenGL.hpp>

#include <Windows.h>

#include <vector>

struct Renderer
{
	sf::Window window;
	int window_width = 0;
	int window_height = 0;
	Palette *palette = nullptr;

	GLuint surface_program = 0;
	GLuint frontbuffer_program = 0;
	GLuint progressbar_program = 0;
	GLuint progressbar_vao = 0;

	void InitGl();

	void LoadSurfaceProgram();

	void LoadFrontbufferProgram();

	void LoadProgressbarProgram();

public:
	static const int FRONTBUFFER_WIDTH = 640;
	static const int FRONTBUFFER_HEIGHT = 480;

	struct Surface {
		int width = 0;
		int height = 0;

		GLuint fbo = 0;

		GLuint vao;
		GLuint vbo;
		GLuint ebo;
		GLuint texture;

		std::vector<byte> texture_buffer;

		Surface() = default;
		
		Surface(Surface &&);

		Surface &operator=(Surface &&);

		Surface(int width, int height, GLuint program);

		~Surface();
	};

	Renderer(HWND hwnd, int window_width, int window_height);

	~Renderer();

	Surface CreateSurface(int width, int height, bool fb);

	void CreateFramebuffer(Surface *surface) {
		if (surface->fbo == 0) {
			glGenFramebuffers(1, &surface->fbo);
			glBindFramebuffer(GL_FRAMEBUFFER, surface->fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, surface->texture, 0);
		}
	}

	void DownloadSurfaceBuffer(Surface *surface) {
		glBindTexture(GL_TEXTURE_2D, surface->texture);
		glGetTexImage(
			GL_TEXTURE_2D,
			0,
			GL_RED,
			GL_UNSIGNED_BYTE,
			surface->texture_buffer.data());
	}

	void UploadSurfaceBuffer(Surface *surface) {
		glBindTexture(GL_TEXTURE_2D, surface->texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, surface->width, surface->height, 0, GL_RED, GL_UNSIGNED_BYTE, surface->texture_buffer.data());
	}

	void SetPalette(Palette *palette);

	void Render(int x, int y, int sx_, int sy_, int color_key, Surface *a, Surface *b);

	void RenderToScreen(Surface *b) {
		if (!palette)
			return;

		glViewport(0, 0, window_width, window_height);

		GLuint fbo = 0;
		GLuint program = frontbuffer_program;
		auto &surface = *b;
		int x = 0;
		int y = 0;

		glUseProgram(program);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);

		assert(!glGetError());

		glm::mat4 pos;
		pos = glm::translate(pos, { x, y, 0 });

		auto uniPos = glGetUniformLocation(program, "pos");
		if (uniPos >= 0)
			glUniform2f(uniPos, x, y);

		assert(!glGetError());

		glm::mat4 trans;

		if (fbo == 0) {
			trans = glm::scale(trans, { 1, -1, 1 });
		}

		trans = glm::translate(trans, { -1, -1, 0 });

		float sx = 2 / 640.f;
		float sy = 2 / 480.f;

		trans = glm::scale(trans, { sx, sy, 1 });

		GLint uniTrans = glGetUniformLocation(program, "trans");
		if (uniTrans >= 0)
			glUniformMatrix4fv(uniTrans, 1, GL_FALSE, glm::value_ptr(trans));

		assert(!glGetError());

		glBindVertexArray(surface.vao);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, surface.texture);

		assert(!glGetError());

		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_1D, palette->palette_texture);

		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

		assert(!glGetError());
	}

	void RenderRect(Surface *surface, int color_index, int x, int y, int w, int h);

	void Clear(Surface *surface, int color);
};


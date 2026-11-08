#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <string>
#include <vector>
#include <map>

#include "graphics.h"
#include "log.h"

Scene2D::Scene2D(int w, int h, int pixelDepth)
{
	this->width = w;
	this->height = h;
	this->depth = pixelDepth;
	
	this->frameBufferSize = this->width * this->height * this->depth;
	this->ramBackBuffer = NULL;
}

Scene2D::~Scene2D()
{
	if (this->ramBackBuffer)
	{
		free(this->ramBackBuffer);
		this->ramBackBuffer = NULL;
	}
	deallocateVideoMem();
}

bool Scene2D::Init(size_t memSize, int numFrameBuffers)
{
	int rc;
	
	this->video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
	this->videoMem = NULL;

	if (this->video < 0)
	{
		DEBUGLOG << "Failed to open a video out handle: " << std::string(strerror(errno));
		return false;
	}
	
#ifdef GRAPHICS_USES_FONT
	// Load freetype
	rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_FREETYPE_OL);

	if (rc < 0)
	{
		DEBUGLOG << "Failed to load freetype: " << std::string(strerror(errno));
		return false;
	}

	// Initialize freetype
	rc = FT_Init_FreeType(&this->ftLib);

	if (rc != 0)
	{
		DEBUGLOG << "Failed to initialize freetype: " << std::string(strerror(errno));
		return false;
	}
#endif
	
	if(!initFlipQueue())
	{
		DEBUGLOG << "Failed to initialize flip queue: " << std::string(strerror(errno));
		return false;
	}
	
	if(!allocateVideoMem(memSize, 0x200000))
	{
		DEBUGLOG << "Failed to allocate video memory: " << std::string(strerror(errno));
		return false;
	}
	
	if(!allocateFrameBuffers(numFrameBuffers))
	{
		DEBUGLOG << "Failed to allocate frame buffers: " << std::string(strerror(errno));
		return false;
	}

	this->ramBackBuffer = (uint32_t*)malloc(this->frameBufferSize);
	if (this->ramBackBuffer)
	{
		memset(this->ramBackBuffer, 0, this->frameBufferSize);
	}
	
	sceVideoOutSetFlipRate(this->video, 0);
	return true;
}

bool Scene2D::initFlipQueue()
{
	int rc = sceKernelCreateEqueue(&flipQueue, "homebrew flip queue");
	
	if(rc < 0)
		return false;
		
	sceVideoOutAddFlipEvent(flipQueue, this->video, 0);
	return true;
}

bool Scene2D::allocateFrameBuffers(int num)
{
	// Allocate frame buffers array
	this->frameBuffers = new char*[num];
	
	// Set the display buffers
	for(int i = 0; i < num; i++)
		this->frameBuffers[i] = this->allocateDisplayMem(frameBufferSize);

	// Set SRGB pixel format
	sceVideoOutSetBufferAttribute(&this->attr, 0x80000000, 1, 0, this->width, this->height, this->width);
	
	// Register the buffers to the video handle
	return (sceVideoOutRegisterBuffers(this->video, 0, (void **)this->frameBuffers, num, &this->attr) == 0);
}

char *Scene2D::allocateDisplayMem(size_t size)
{
	// Essentially just bump allocation
	char *allocatedPtr = (char *)videoMemSP;
	videoMemSP += size;

	return allocatedPtr;
}

bool Scene2D::allocateVideoMem(size_t size, int alignment)
{
	int rc;
	
	// Align the allocation size
	this->directMemAllocationSize = (size + alignment - 1) / alignment * alignment;
	
	// Allocate memory for display buffer
	rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), this->directMemAllocationSize, alignment, 3, &this->directMemOff);
	
	if(rc < 0)
	{
		this->directMemAllocationSize = 0;
		return false;
	}
	
	// Map the direct memory
	rc = sceKernelMapDirectMemory(&this->videoMem, this->directMemAllocationSize, 0x33, 0, this->directMemOff, alignment);
	
	if(rc < 0)
	{
		sceKernelReleaseDirectMemory(this->directMemOff, this->directMemAllocationSize);
		
		this->directMemOff = 0;
		this->directMemAllocationSize = 0;
		
		return false;
	}
	
	// Set the stack pointer to the beginning of the buffer
	this->videoMemSP = (uintptr_t)this->videoMem;
	return true;
}

void Scene2D::deallocateVideoMem()
{
	// Free the direct memory
	sceKernelReleaseDirectMemory(this->directMemOff, this->directMemAllocationSize);
	
	// Zero out meta data
	this->videoMem = 0;
	this->videoMemSP = 0;
	this->directMemOff = 0;
	this->directMemAllocationSize = 0;
	
	// Free the frame buffer array
	delete this->frameBuffers;
	this->frameBuffers = 0;
}

void Scene2D::SetActiveFrameBuffer(int index)
{
	this->activeFrameBufferIdx = index;
}

void Scene2D::SubmitFlip(int frameID)
{
	if (this->ramBackBuffer && this->frameBuffers && this->frameBuffers[this->activeFrameBufferIdx])
	{
		memcpy(this->frameBuffers[this->activeFrameBufferIdx], this->ramBackBuffer, this->frameBufferSize);
	}
	sceVideoOutSubmitFlip(this->video, this->activeFrameBufferIdx, ORBIS_VIDEO_OUT_FLIP_VSYNC, frameID);
}

void Scene2D::FrameWait(int frameID)
{
	OrbisKernelEvent evt;
	int count;
	
	// If the video handle is not initialized, bail out. This is mostly a failsafe, this should never happen.
	if(this->video == 0)
		return;
		
	for(;;)
	{
		OrbisVideoOutFlipStatus flipStatus;
		
		// Get the flip status and check the arg for the given frame ID
		sceVideoOutGetFlipStatus(video, &flipStatus);
		
		if(flipStatus.flipArg == frameID)
			break;
			
		// Wait on next flip event
		if(sceKernelWaitEqueue(this->flipQueue, &evt, 1, &count, 0) != 0)
			break;
	}
}

void Scene2D::FrameBufferSwap()
{
	// Swap the frame buffer for some perf
	this->activeFrameBufferIdx = (this->activeFrameBufferIdx + 1) % 2;
}

void Scene2D::FrameBufferClear()
{
	// Clear the screen with a white frame buffer
	Color blank = { 255, 255, 255 };
	FrameBufferFill(blank);
}

#ifdef GRAPHICS_USES_FONT
bool Scene2D::InitFont(FT_Face *face, const char *fontPath, int fontSize)
{
	int rc;
	
	rc = FT_New_Face(this->ftLib, fontPath, 0, face);
	
	if(rc < 0)
		return false;

	rc = FT_Set_Pixel_Sizes(*face, 0, fontSize);
	
	if(rc < 0)
		return false;
	
	return true;
}
#endif

void Scene2D::FrameBufferFill(Color color)
{
	DrawRectangle(0, 0, this->width, this->height, color);
}

void Scene2D::DrawPixel(int x, int y, Color color)
{
	if (x < 0 || x >= this->width || y < 0 || y >= this->height) return;
	int pixel = (y * this->width) + x;
	uint32_t encodedColor = 0x80000000 | (color.r << 16) | (color.g << 8) | color.b;
	if (this->ramBackBuffer)
	{
		this->ramBackBuffer[pixel] = encodedColor;
	}
	else if (this->frameBuffers && this->frameBuffers[this->activeFrameBufferIdx])
	{
		((uint32_t *)this->frameBuffers[this->activeFrameBufferIdx])[pixel] = encodedColor;
	}
}

void Scene2D::DrawPixelBlend(int x, int y, Color color, uint8_t alpha)
{
	if (x < 0 || x >= this->width || y < 0 || y >= this->height || alpha == 0) return;
	int pixel = (y * this->width) + x;
	uint32_t* fb = this->ramBackBuffer ? this->ramBackBuffer : (this->frameBuffers ? (uint32_t*)this->frameBuffers[this->activeFrameBufferIdx] : NULL);
	if (!fb) return;
	if (alpha >= 255) {
		fb[pixel] = 0x80000000 | (color.r << 16) | (color.g << 8) | color.b;
		return;
	}
	uint32_t bg = fb[pixel];
	uint32_t bgR = (bg >> 16) & 0xFF;
	uint32_t bgG = (bg >> 8) & 0xFF;
	uint32_t bgB = bg & 0xFF;
	
	uint32_t invA = 255 - alpha;
	uint32_t outR = (color.r * alpha + bgR * invA) / 255;
	uint32_t outG = (color.g * alpha + bgG * invA) / 255;
	uint32_t outB = (color.b * alpha + bgB * invA) / 255;
	
	fb[pixel] = 0x80000000 | (outR << 16) | (outG << 8) | outB;
}

void Scene2D::DrawRectangle(int x, int y, int w, int h, Color color)
{
	if (w <= 0 || h <= 0) return;
	int x1 = x < 0 ? 0 : x;
	int y1 = y < 0 ? 0 : y;
	int x2 = (x + w) > this->width ? this->width : (x + w);
	int y2 = (y + h) > this->height ? this->height : (y + h);
	if (x1 >= x2 || y1 >= y2) return;

	uint32_t encodedColor = 0x80000000 | (color.r << 16) | (color.g << 8) | color.b;
	uint32_t* fb = this->ramBackBuffer ? this->ramBackBuffer : (this->frameBuffers ? (uint32_t*)this->frameBuffers[this->activeFrameBufferIdx] : NULL);
	if (!fb) return;

	int span = x2 - x1;
	for (int yPos = y1; yPos < y2; yPos++)
	{
		uint32_t* row = fb + (yPos * this->width + x1);
		for (int xPos = 0; xPos < span; xPos++)
		{
			row[xPos] = encodedColor;
		}
	}
}

void Scene2D::CopyToActiveBuffer(const uint32_t* srcPixels)
{
	if (!srcPixels) return;
	if (this->ramBackBuffer)
	{
		memcpy(this->ramBackBuffer, srcPixels, this->frameBufferSize);
	}
	else if (this->frameBuffers && this->frameBuffers[this->activeFrameBufferIdx])
	{
		memcpy(this->frameBuffers[this->activeFrameBufferIdx], srcPixels, this->frameBufferSize);
	}
}

#ifdef GRAPHICS_USES_FONT
struct CachedGlyph {
	int width;
	int rows;
	int left;
	int top;
	int advanceX;
	std::vector<uint8_t> buffer;
	bool loaded;

	CachedGlyph() : width(0), rows(0), left(0), top(0), advanceX(0), loaded(false) {}
};

static std::map<FT_Face, std::vector<CachedGlyph> > s_glyphCache;

static const CachedGlyph* get_cached_glyph(FT_Face face, unsigned char ch)
{
	std::vector<CachedGlyph>& faceCache = s_glyphCache[face];
	if (faceCache.empty())
	{
		faceCache.resize(256);
	}
	CachedGlyph& cg = faceCache[ch];
	if (!cg.loaded)
	{
		FT_UInt glyph_index = FT_Get_Char_Index(face, ch);
		if (FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT) == 0)
		{
			FT_GlyphSlot slot = face->glyph;
			if (FT_Render_Glyph(slot, ft_render_mode_normal) == 0)
			{
				cg.width = slot->bitmap.width;
				cg.rows = slot->bitmap.rows;
				cg.left = slot->bitmap_left;
				cg.top = slot->bitmap_top;
				cg.advanceX = slot->advance.x >> 6;
				int bufSize = cg.width * cg.rows;
				if (bufSize > 0 && slot->bitmap.buffer)
				{
					cg.buffer.assign(slot->bitmap.buffer, slot->bitmap.buffer + bufSize);
				}
			}
			else
			{
				cg.advanceX = slot->advance.x >> 6;
			}
		}
		cg.loaded = true;
	}
	return &cg;
}

int Scene2D::GetTextWidth(const char *txt, FT_Face face)
{
	if (!txt || !face) return 0;
	int totalWidth = 0;
	int lineWidth = 0;
	for (size_t i = 0; txt[i] != '\0'; i++)
	{
		if (txt[i] == '\n')
		{
			if (lineWidth > totalWidth) totalWidth = lineWidth;
			lineWidth = 0;
			continue;
		}
		const CachedGlyph* cg = get_cached_glyph(face, (unsigned char)txt[i]);
		if (cg)
		{
			lineWidth += cg->advanceX;
		}
	}
	if (lineWidth > totalWidth) totalWidth = lineWidth;
	return totalWidth;
}

void Scene2D::DrawText(char *txt, FT_Face face, int startX, int startY, Color bgColor, Color fgColor)
{
	if (!txt || !face) return;
	int xOffset = 0;
	int yOffset = 0;
	size_t len = strlen(txt);

	for (size_t n = 0; n < len; n++)
	{
		if (txt[n] == '\n')
		{
			xOffset = 0;
			yOffset += (face->size ? (face->size->metrics.height >> 6) : 24);
			continue;
		}

		const CachedGlyph* cg = get_cached_glyph(face, (unsigned char)txt[n]);
		if (!cg || !cg->loaded) continue;

		if (cg->width > 0 && cg->rows > 0 && !cg->buffer.empty())
		{
			const uint8_t* bmp = cg->buffer.data();
			for (int yPos = 0; yPos < cg->rows; yPos++)
			{
				int py = startY + yPos + yOffset - cg->top;
				if (py < 0 || py >= this->height) continue;
				for (int xPos = 0; xPos < cg->width; xPos++)
				{
					uint8_t pixel = bmp[(yPos * cg->width) + xPos];
					if (pixel != 0)
					{
						int px = startX + xPos + xOffset + cg->left;
						this->DrawPixelBlend(px, py, fgColor, pixel);
					}
				}
			}
		}

		xOffset += cg->advanceX;
	}
}
#endif


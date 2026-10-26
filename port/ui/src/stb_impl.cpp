// Written for Demon's Souls. stb_image and stb_image_write (public domain / MIT, fetched by port/ui/CMakeLists.txt):
// the menu's images (render_d3d12.cpp, render_d3d11.cpp) and the reports' screenshots (report.cpp).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#define STBI_ONLY_JPEG
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

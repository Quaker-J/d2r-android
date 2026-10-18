// BC -> uncompressed Vulkan formats, ported from d2r-mobile/dm/gfx/tex/nvn_bcdec.c.
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#include "../../third_party/bcdec/bcdec.h"
#include "nvn_bcdec_vk.h"
#include <string.h>

int nvn_bc_unpacked_vk(uint32_t f, VkFormat *format, int *bpp) {
    switch (f) {
    case 0x41: case 0x42: case 0x43: case 0x44: case 0x4d: *format = VK_FORMAT_R8G8B8A8_UNORM; *bpp = 4; return 1;
    case 0x45: case 0x46: case 0x47: case 0x48: case 0x4e: *format = VK_FORMAT_R8G8B8A8_SRGB; *bpp = 4; return 1;
    case 0x49: *format = VK_FORMAT_R8_UNORM; *bpp = 1; return 1;
    case 0x4a: *format = VK_FORMAT_R8_SNORM; *bpp = 1; return 1;
    case 0x4b: *format = VK_FORMAT_R8G8_UNORM; *bpp = 2; return 1;
    case 0x4c: *format = VK_FORMAT_R8G8_SNORM; *bpp = 2; return 1;
    case 0x4f: case 0x50: *format = VK_FORMAT_R16G16B16A16_SFLOAT; *bpp = 8; return 1;
    default: return 0;
    }
}

static void block(uint32_t f, const uint8_t *b, uint8_t *o, int op) {
    switch (f) {
    case 0x41: case 0x45: bcdec_bc1(b, o, op); for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) o[y * op + x * 4 + 3] = 255; break;
    case 0x42: case 0x46: bcdec_bc1(b, o, op); break;
    case 0x43: case 0x47: bcdec_bc2(b, o, op); break;
    case 0x44: case 0x48: bcdec_bc3(b, o, op); break;
    case 0x49: bcdec_bc4(b, o, op, 0); break;
    case 0x4a: bcdec_bc4(b, o, op, 1); break;
    case 0x4b: bcdec_bc5(b, o, op, 0); break;
    case 0x4c: bcdec_bc5(b, o, op, 1); break;
    case 0x4d: case 0x4e: bcdec_bc7(b, o, op); break;
    case 0x4f: case 0x50: {
        uint16_t rgb[16 * 3]; bcdec_bc6h_half(b, rgb, 4 * 3, f == 0x4f);
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            uint16_t *d = (uint16_t *)(o + y * op) + x * 4; const uint16_t *s = rgb + (y * 4 + x) * 3;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 0x3c00;
        }
        break;
    }
    }
}

int nvn_bc_decode(uint32_t f, const uint8_t *src, size_t pitch, uint32_t w, uint32_t h, uint8_t *dst, size_t dpitch) {
    VkFormat format; int bpp;
    if (!nvn_bc_unpacked_vk(f, &format, &bpp)) return -1;
    int bb = (f == 0x41 || f == 0x42 || f == 0x45 || f == 0x46 || f == 0x49 || f == 0x4a) ? 8 : 16;
    uint8_t tmp[4 * 4 * 8];
    for (uint32_t by = 0; by * 4 < h; by++) for (uint32_t bx = 0; bx * 4 < w; bx++) {
        const uint8_t *b = src + by * pitch + bx * bb;
        uint32_t cw = w - bx * 4 < 4 ? w - bx * 4 : 4, ch = h - by * 4 < 4 ? h - by * 4 : 4;
        uint8_t *o = dst + (size_t)by * 4 * dpitch + (size_t)bx * 4 * bpp;
        if (cw == 4 && ch == 4) { block(f, b, o, (int)dpitch); continue; }
        block(f, b, tmp, 4 * bpp);
        for (uint32_t y = 0; y < ch; y++) memcpy(o + y * dpitch, tmp + y * 4 * bpp, cw * bpp);
    }
    return 0;
}

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

// BC texture fallback for GPUs without textureCompressionBC (e.g. Mali).
int nvn_bc_unpacked_vk(uint32_t nvn_fmt, VkFormat *format, int *bpp);
int nvn_bc_decode(uint32_t nvn_fmt, const uint8_t *src, size_t pitch, uint32_t w, uint32_t h, uint8_t *dst, size_t dpitch);

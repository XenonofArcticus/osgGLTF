// ibl-bake-cpu -- CPU-only IBL prefilter baker (no OpenGL required).
//
// Usage: ibl-bake-cpu <input.hdr> <output.ktx2> [--prefilter-size N] [--samples N]
//
// Directly integrates GGX importance samples against the equirectangular HDR;
// no intermediate cubemap step. Output is identical in format to ibl-bake (GPU).
// Uses OpenMP when available.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_HDR
#define STBI_ONLY_PIC // HDR format uses Radiance RGBE internally
#include <stb_image.h>

#include <ktx.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _OPENMP
# include <omp.h>
#endif

#ifndef M_PI
# define M_PI 3.14159265358979323846
#endif

// VK_FORMAT_R16G16B16_SFLOAT
#ifndef VK_FORMAT_R16G16B16_SFLOAT
# define VK_FORMAT_R16G16B16_SFLOAT 90
#endif

// ---------------------------------------------------------------------------
// Minimal math types (no external deps)
// ---------------------------------------------------------------------------

struct v2 { float x, y; };
struct v3 { float x, y, z; };

static v3 operator+(v3 a, v3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
static v3 operator*(v3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }
static v3 operator-(v3 a) { return {-a.x, -a.y, -a.z}; }

static float dot(v3 a, v3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static v3 cross(v3 a, v3 b) {
	return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}
static v3 normalize(v3 v) {
	float len = std::sqrt(dot(v, v));
	if (len < 1e-10f) return {0, 0, 1};
	return {v.x/len, v.y/len, v.z/len};
}
static v3 reflect(v3 v, v3 n) { return v + n * (-2.0f * dot(v, n)); }

// ---------------------------------------------------------------------------
// Float32 -> Float16 conversion
// ---------------------------------------------------------------------------

static uint16_t f32_to_f16(float f)
{
	union { float f; uint32_t u; } bits = {f};
	uint32_t s = (bits.u >> 31) & 1u;
	uint32_t e = (bits.u >> 23) & 0xFFu;
	uint32_t m = bits.u & 0x7FFFFFu;

	if (e == 0xFF) return (uint16_t)((s << 15) | 0x7C00u | (m ? 0x200u : 0u)); // inf/nan

	int se = (int)e - 127 + 15;
	if (se >= 31) return (uint16_t)((s << 15) | 0x7C00u); // overflow -> inf
	if (se <= 0) {
		if (se < -10) return (uint16_t)(s << 15); // underflow -> 0
		uint32_t ms = (m | 0x800000u) >> (uint32_t)(14 - se);
		return (uint16_t)((s << 15) | ms);
	}
	return (uint16_t)((s << 15) | ((uint32_t)se << 10) | (m >> 13));
}

// ---------------------------------------------------------------------------
// Equirectangular HDR sampling
// Matches the equirect_uv() + Z-up->Y-up chain in ibl-bake.cpp GLSL shaders.
// dir_gl: direction in GL Y-up cubemap space.
// ---------------------------------------------------------------------------

static v3 sampleHDR(const float* img, int W, int H, v3 dir_gl)
{
	// GL Y-up -> Z-up: (x, y, z) -> (x, -z, y)
	float dzu_x = dir_gl.x, dzu_y = -dir_gl.z, dzu_z = dir_gl.y;

	// equirect_uv(dir_zup) from GLSL: d = (dir_zup.x, dir_zup.z, -dir_zup.y)
	float dx = dzu_x, dy = dzu_z, dz = -dzu_y;

	float phi = std::atan2(dz, dx) - float(M_PI) / 2.0f;
	float theta = std::acos(std::max(-1.0f, std::min(1.0f, dy)));
	float u = phi / (2.0f * float(M_PI)) + 0.5f;
	float v = 1.0f - theta / float(M_PI);

	u -= std::floor(u); // wrap horizontally
	v = std::max(0.0f, std::min(1.0f, v)); // clamp vertically

	float px = u * float(W - 1);
	float py = v * float(H - 1);
	int x0 = (int)px, y0 = (int)py;
	int x1 = std::min(x0 + 1, W - 1);
	int y1 = std::min(y0 + 1, H - 1);
	float tx = px - float(x0), ty = py - float(y0);

	auto fetch = [&](int x, int y) -> v3 {
		int i = (y * W + x) * 3;
		return {img[i], img[i+1], img[i+2]};
	};
	auto lerp3 = [](v3 a, v3 b, float t) -> v3 {
		return a + (b + a * -1.0f) * t;
	};

	v3 c0 = lerp3(fetch(x0, y0), fetch(x1, y0), tx);
	v3 c1 = lerp3(fetch(x0, y1), fetch(x1, y1), tx);
	return lerp3(c0, c1, ty);
}

// ---------------------------------------------------------------------------
// Cubemap face direction -- matches PREFILTER_FRAG faceIndex branches exactly
// ---------------------------------------------------------------------------

static v3 faceDir(int face, float u, float v)
{
	// u, v in [-1, 1]
	switch (face) {
		case 0: return normalize({ 1.0f, -v, -u});
		case 1: return normalize({-1.0f, -v, u});
		case 2: return normalize({ u, 1.0f, v});
		case 3: return normalize({ u, -1.0f, -v});
		case 4: return normalize({ u, -v, 1.0f});
		default:return normalize({-u, -v, -1.0f});
	}
}

// ---------------------------------------------------------------------------
// Hammersley + GGX importance sampling (direct port from GLSL)
// ---------------------------------------------------------------------------

static v2 hammersley(uint32_t i, uint32_t N)
{
	uint32_t bits = i;
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return {float(i) / float(N), float(bits) * 2.3283064365386963e-10f};
}

static v3 importanceSampleGGX(v2 Xi, v3 N, float roughness)
{
	float a = roughness * roughness;
	float phi = 2.0f * float(M_PI) * Xi.x;
	float cosTheta = std::sqrt((1.0f - Xi.y) / (1.0f + (a*a - 1.0f) * Xi.y));
	float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));

	v3 H = {sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};

	v3 up = (std::abs(N.z) < 0.999f) ? v3{0, 0, 1} : v3{1, 0, 0};
	v3 T = normalize(cross(up, N));
	v3 B = cross(N, T);

	return normalize({
		T.x*H.x + B.x*H.y + N.x*H.z,
		T.y*H.x + B.y*H.y + N.y*H.z,
		T.z*H.x + B.z*H.y + N.z*H.z,
	});
}

// ---------------------------------------------------------------------------
// bake one mipxface block into buf (RGB float32, already allocated)
// ---------------------------------------------------------------------------

static void bakeFaceMip(
	const float* hdr, int hdrW, int hdrH,
	float* out, int mipSize,
	int face, float roughness, uint32_t numSamples)
{
	float invSize = 1.0f / float(mipSize);

	#pragma omp parallel for schedule(dynamic, 4)
	for (int py = 0; py < mipSize; ++py)
	{
		for (int px = 0; px < mipSize; ++px)
		{
			float u = (float(px) + 0.5f) * invSize * 2.0f - 1.0f;
			float v = (float(py) + 0.5f) * invSize * 2.0f - 1.0f;
			v3 N = faceDir(face, u, v);
			v3 V = N; // V = N approximation (same as GLSL prefilter)

			v3 prefilt = {0, 0, 0};
			float weight = 0.0f;

			for (uint32_t i = 0; i < numSamples; ++i) {
				v2 Xi = hammersley(i, numSamples);
				v3 H = importanceSampleGGX(Xi, N, roughness);
				v3 L = normalize(reflect(-V, H));

				float NdotL = std::max(0.0f, dot(N, L));
				if (NdotL > 0.0f) {
					v3 s = sampleHDR(hdr, hdrW, hdrH, L);
					prefilt.x += s.x * NdotL;
					prefilt.y += s.y * NdotL;
					prefilt.z += s.z * NdotL;
					weight += NdotL;
				}
			}

			float inv = 1.0f / std::max(weight, 0.001f);
			int idx = (py * mipSize + px) * 3;
			out[idx + 0] = prefilt.x * inv;
			out[idx + 1] = prefilt.y * inv;
			out[idx + 2] = prefilt.z * inv;
		}
	}
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
	if (argc < 3) {
		fprintf(stderr,
			"Usage: ibl-bake-cpu <input.hdr> <output.ktx2>"
			" [--prefilter-size N] [--samples N]\n");
		return 1;
	}

	const char* hdrPath = argv[1];
	const char* outputPath = argv[2];
	int prefilterSize = 128;
	uint32_t numSamples = 1024;

	for (int i = 3; i < argc; ++i) {
		if (!strcmp(argv[i], "--prefilter-size") && i+1 < argc) prefilterSize = std::atoi(argv[++i]);
		if (!strcmp(argv[i], "--samples") && i+1 < argc) numSamples = (uint32_t)std::atoi(argv[++i]);
	}

	// Load HDR -- flip vertically so row 0 = bottom (OpenGL convention),
	// matching OSG's HDR reader and the GLSL equirect_uv() formula.
	stbi_set_flip_vertically_on_load(1);
	int hdrW = 0, hdrH = 0, hdrCh = 0;
	float* hdr = stbi_loadf(hdrPath, &hdrW, &hdrH, &hdrCh, 3);
	if (!hdr) {
		fprintf(stderr, "ibl-bake-cpu: failed to load '%s': %s\n",
			hdrPath, stbi_failure_reason());
		return 1;
	}
	printf("Loaded HDR: %dx%d channels=%d\n", hdrW, hdrH, hdrCh);

	// Compute mip chain params
	int numMips = 0;
	for (int s = prefilterSize; s >= 1; s >>= 1) ++numMips;
	printf("Prefilter: %dx%d %d mips %u samples\n",
		prefilterSize, prefilterSize, numMips, numSamples);
#ifdef _OPENMP
	printf("OpenMP threads: %d\n", omp_get_max_threads());
#endif

	// Allocate float32 buffers for all mips, all faces
	// Layout: [mip][face][pixels * 3]
	std::vector<std::vector<std::vector<float>>> data(numMips,
		std::vector<std::vector<float>>(6));

	for (int mip = 0; mip < numMips; ++mip) {
		int mipSize = std::max(1, prefilterSize >> mip);
		float roughness = (numMips > 1) ? float(mip) / float(numMips - 1) : 0.0f;

		for (int face = 0; face < 6; ++face) {
			data[mip][face].resize((size_t)mipSize * mipSize * 3, 0.0f);
			printf(" baking mip %d/%d face %d size=%d roughness=%.3f ...\n",
				mip, numMips-1, face, mipSize, roughness);
			fflush(stdout);

			bakeFaceMip(hdr, hdrW, hdrH,
				data[mip][face].data(), mipSize,
				face, roughness, numSamples);
		}
	}

	stbi_image_free(hdr);

	// ---------------------------------------------------------------------------
	// Write KTX2 using libktx
	// ---------------------------------------------------------------------------

	ktxTextureCreateInfo info = {};
	info.vkFormat = VK_FORMAT_R16G16B16_SFLOAT;
	info.baseWidth = (ktx_uint32_t)prefilterSize;
	info.baseHeight = (ktx_uint32_t)prefilterSize;
	info.baseDepth = 1;
	info.numDimensions = 2;
	info.numLevels = (ktx_uint32_t)numMips;
	info.numLayers = 1;
	info.numFaces = 6;
	info.isArray = KTX_FALSE;
	info.generateMipmaps = KTX_FALSE;

	ktxTexture2* ktx = nullptr;
	KTX_error_code rc = ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &ktx);
	if (rc != KTX_SUCCESS) {
		fprintf(stderr, "ibl-bake-cpu: ktxTexture2_Create failed (%d)\n", (int)rc);
		return 1;
	}

	for (int mip = 0; mip < numMips; ++mip) {
		int mipSize = std::max(1, prefilterSize >> mip);
		size_t pixelCount = (size_t)mipSize * mipSize;

		// Convert float32 -> float16 per face
		std::vector<uint16_t> f16(pixelCount * 3);

		for (int face = 0; face < 6; ++face) {
			const auto& f32 = data[mip][face];
			for (size_t k = 0; k < pixelCount * 3; ++k)
				f16[k] = f32_to_f16(f32[k]);

			size_t imageBytes = pixelCount * 3 * sizeof(uint16_t);
			rc = ktxTexture_SetImageFromMemory(
				(ktxTexture*)ktx, (ktx_uint32_t)mip, 0, (ktx_uint32_t)face,
				(const ktx_uint8_t*)f16.data(), imageBytes);
			if (rc != KTX_SUCCESS) {
				fprintf(stderr, "ibl-bake-cpu: SetImage mip=%d face=%d failed (%d)\n",
					mip, face, (int)rc);
				ktxTexture_Destroy((ktxTexture*)ktx);
				return 1;
			}
		}
	}

	rc = ktxTexture_WriteToNamedFile((ktxTexture*)ktx, outputPath);
	ktxTexture_Destroy((ktxTexture*)ktx);

	if (rc != KTX_SUCCESS) {
		fprintf(stderr, "ibl-bake-cpu: write failed (%d): %s\n", (int)rc, outputPath);
		return 1;
	}

	printf("Wrote %s\n", outputPath);
	return 0;
}

// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "ppsspp_config.h"
#include <functional>

#include "Common/Profiler/Profiler.h"
#include "Common/GPU/Vulkan/VulkanRenderManager.h"

#include "Common/Log.h"

#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"

#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/GPU/Vulkan/VulkanMemory.h"

#include "GPU/GPUCommon.h"
#include "GPU/Common/SplineCommon.h"
#include "GPU/Common/TransformCommon.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/Common/SoftwareTransformCommon.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/Common/ShaderUniforms.h"
#include "GPU/Vulkan/DrawEngineVulkan.h"
#include "GPU/Vulkan/StvSombraRecorte.h"  // STV_SOMBRA_RECORTE_v1
#include "Common/Math/CrossSIMD.h"
#include "Common/StvProp.h"
#include "Common/File/FileUtil.h"  // STV_VOLDUMP_v1
#include "Core/Config.h"
#include <cstring>
#include "Common/TimeUtil.h"
#include <cmath>
#include "GPU/Vulkan/TextureCacheVulkan.h"
#include "GPU/Vulkan/ShaderManagerVulkan.h"
#include "GPU/Vulkan/PipelineManagerVulkan.h"
#include "GPU/Vulkan/FramebufferManagerVulkan.h"

using namespace PPSSPP_VK;

enum {
	TRANSFORMED_VERTEX_BUFFER_SIZE = VERTEX_BUFFER_MAX * sizeof(TransformedVertex)
};

DrawEngineVulkan::DrawEngineVulkan(Draw::DrawContext *draw)
	: draw_(draw) {
	decOptions_.expandAllWeightsToFloat = false;
	decOptions_.expand8BitNormalsToFloat = false;
}

void DrawEngineVulkan::InitDeviceObjects() {
	// All resources we need for PSP drawing. Usually only bindings 0 and 2-4 are populated.

	BindingType bindingTypes[VKRPipelineLayout::MAX_DESC_SET_BINDINGS] = {
		BindingType::COMBINED_IMAGE_SAMPLER,  // main
		BindingType::COMBINED_IMAGE_SAMPLER,  // framebuffer-read
		BindingType::COMBINED_IMAGE_SAMPLER,  // palette
		BindingType::UNIFORM_BUFFER_DYNAMIC_ALL,  // uniforms
		BindingType::UNIFORM_BUFFER_DYNAMIC_VERTEX,  // lights
		BindingType::UNIFORM_BUFFER_DYNAMIC_VERTEX,  // bones
		BindingType::STORAGE_BUFFER_VERTEX,  // tess
		BindingType::STORAGE_BUFFER_VERTEX,
		BindingType::STORAGE_BUFFER_VERTEX,
	};

	VulkanContext *vulkan = (VulkanContext *)draw_->GetNativeObject(Draw::NativeObject::CONTEXT);
	VkDevice device = vulkan->GetDevice();

	VulkanRenderManager *renderManager = (VulkanRenderManager *)draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);
	pipelineLayout_ = renderManager->CreatePipelineLayout(bindingTypes, ARRAY_SIZE(bindingTypes), draw_->GetDeviceCaps().geometryShaderSupported, "drawengine_layout");

	pushUBO_ = (VulkanPushPool *)draw_->GetNativeObject(Draw::NativeObject::PUSH_POOL);
	pushVertex_ = new VulkanPushPool(vulkan, "pushVertex", 4 * 1024 * 1024, 256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	pushIndex_ = new VulkanPushPool(vulkan, "pushIndex", 512 * 1024, 64, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

	VkSamplerCreateInfo samp{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samp.magFilter = VK_FILTER_LINEAR;
	samp.minFilter = VK_FILTER_LINEAR;
	samp.maxLod = VK_LOD_CLAMP_NONE;  // recommended by best practices, has no effect since we don't use mipmaps.
	VkResult res = vkCreateSampler(device, &samp, nullptr, &samplerSecondaryLinear_);
	samp.magFilter = VK_FILTER_NEAREST;
	samp.minFilter = VK_FILTER_NEAREST;
	res = vkCreateSampler(device, &samp, nullptr, &samplerSecondaryNearest_);
	_dbg_assert_(VK_SUCCESS == res);
	res = vkCreateSampler(device, &samp, nullptr, &nullSampler_);
	_dbg_assert_(VK_SUCCESS == res);

	tessDataTransferVulkan = new TessellationDataTransferVulkan(vulkan);
	tessDataTransfer = tessDataTransferVulkan;

	draw_->SetInvalidationCallback(std::bind(&DrawEngineVulkan::Invalidate, this, std::placeholders::_1));
}

DrawEngineVulkan::~DrawEngineVulkan() {
	DestroyDeviceObjects();
}

void DrawEngineVulkan::DestroyDeviceObjects() {
	if (!draw_) {
		// We've already done this from LostDevice.
		return;
	}

	VulkanContext *vulkan = (VulkanContext *)draw_->GetNativeObject(Draw::NativeObject::CONTEXT);
	VulkanRenderManager *renderManager = (VulkanRenderManager *)draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);

	draw_->SetInvalidationCallback(InvalidationCallback());

	delete tessDataTransferVulkan;
	tessDataTransfer = nullptr;
	tessDataTransferVulkan = nullptr;

	pushUBO_ = nullptr;

	if (pushVertex_) {
		pushVertex_->Destroy();
		delete pushVertex_;
		pushVertex_ = nullptr;
	}
	if (pushIndex_) {
		pushIndex_->Destroy();
		delete pushIndex_;
		pushIndex_ = nullptr;
	}

	if (samplerSecondaryNearest_ != VK_NULL_HANDLE)
		vulkan->Delete().QueueDeleteSampler(samplerSecondaryNearest_);
	if (samplerSecondaryLinear_ != VK_NULL_HANDLE)
		vulkan->Delete().QueueDeleteSampler(samplerSecondaryLinear_);
	if (nullSampler_ != VK_NULL_HANDLE)
		vulkan->Delete().QueueDeleteSampler(nullSampler_);

	renderManager->DestroyPipelineLayout(pipelineLayout_);
}

void DrawEngineVulkan::DeviceLost() {
	DestroyDeviceObjects();
	DirtyAllUBOs();
	draw_ = nullptr;
}

void DrawEngineVulkan::DeviceRestore(Draw::DrawContext *draw) {
	draw_ = draw;
	InitDeviceObjects();
}

// STV_DRAWINFO_v1 (arco GoS 1:1 STV, 2026-10-01): en un tiler el tiempo por draw no se puede medir
// (Mali difiere los fragmentos al final del pase), asi que el costo de cada draw se saca por
// BISECCION de indices. Cada draw que llega a la GPU lleva un indice dentro del cuadro (se reinicia
// en BeginFrame). Con `debug.stv.drawinfo=1` se describe cada draw de uno de cada 60 cuadros
// (STVDI: destino, vertices, estado y la textura que samplea: tamaño PSP, formato PSP y de Vulkan,
// niveles, o el framebuffer si es render-a-textura). Con `debug.stv.skiprng=a:b` se saltean los
// draws con indice en [a,b) -- rompe la imagen a proposito, es un instrumento. Las dos props se
// releen en cada cuadro (se pueden mover en caliente).
static TextureCacheVulkan *stvDiTex = nullptr;
static int stvDiIdx = 0, stvDiFrame = 0, stvDiModo = 0, stvDiA = -1, stvDiB = -1;
static int stvSwA = -1, stvSwB = -1;  // debug.stv.swforzar
static const TransformedVertex *stvDiTV = nullptr;  // STVDI3: vertices transformados (through/software) del draw actual
static int stvDiTVn = 0;
static void StvDrawInfoCuadro() {
	stvDiFrame++;
	stvDiIdx = 0;
	stvDiModo = StvPropInt("debug.stv.drawinfo");
	stvDiA = stvDiB = -1;
	stvSwA = stvSwB = -1;
#if defined(__ANDROID__)
	{
		char w[PROP_VALUE_MAX] = {0};
		if (__system_property_get("debug.stv.swforzar", w) > 0 && w[0]) {
			int a = -1, b = -1;
			if (sscanf(w, "%d:%d", &a, &b) == 2 && a >= 0 && b > a) { stvSwA = a; stvSwB = b; }
		}
	}
	char v[PROP_VALUE_MAX] = {0};
	if (__system_property_get("debug.stv.skiprng", v) > 0 && v[0]) {
		int a = -1, b = -1;
		if (sscanf(v, "%d:%d", &a, &b) == 2 && a >= 0 && b > a) { stvDiA = a; stvDiB = b; }
	}
#endif
}
// STVDI3: debug.stv.swforzar=a:b fuerza la transformacion por SOFTWARE en los draws de indice [a,b) para
// que STVDI3 vea sus vertices en pantalla y sus UV (instrumento: SW y HW pueden diferir en detalles).
static bool StvForzarSw() {
	return stvSwA >= 0 && stvDiIdx >= stvSwA && stvDiIdx < stvSwB;
}
static bool StvDrawInfo(int verts, const VulkanPipelineRasterStateKey &k, int prim) {
	const int i = stvDiIdx++;
	if (stvDiModo > 0 && (stvDiFrame % 60) == 0) {
		const bool clear = gstate.isModeClear();
		const bool tex = gstate.isTextureMapEnabled() && !clear;
		char t[200] = "-";
		if (tex && stvDiTex) {
			const TexCacheEntry *e = stvDiTex->StvEntradaActual();
			const VirtualFramebuffer *fb = stvDiTex->StvFbTexturaActual();
			if (fb) {
				snprintf(t, sizeof(t), "FB %08x %dx%d fmt%d render %dx%d", fb->fb_address, fb->width, fb->height, (int)fb->fb_format, fb->renderWidth, fb->renderHeight);
			} else if (e) {
				const VulkanTexture *vt = e->vkTex;
				snprintf(t, sizeof(t), "%08x %dx%d ge%d clut%d lvl%d bufw%d vk%d %dx%d mips%d", e->addr, gstate.getTextureWidth(0), gstate.getTextureHeight(0),
					(int)e->format, (int)gstate.getClutPaletteFormat(), (int)e->maxLevel, (int)e->bufw,
					vt ? (int)vt->GetFormat() : -1, vt ? vt->GetWidth() : 0, vt ? vt->GetHeight() : 0, vt ? vt->GetNumMips() : 0);
			}
		}
		if (stvDiModo >= 3 && stvDiTV && stvDiTVn > 0) {
			float b[8] = {1e9f, 1e9f, -1e9f, -1e9f, 1e9f, 1e9f, -1e9f, -1e9f};
			for (int k = 0; k < stvDiTVn; k++) {
				const TransformedVertex &tv = stvDiTV[k];
				b[0] = std::min(b[0], tv.x); b[1] = std::min(b[1], tv.y); b[2] = std::max(b[2], tv.x); b[3] = std::max(b[3], tv.y);
				b[4] = std::min(b[4], tv.u); b[5] = std::min(b[5], tv.v); b[6] = std::max(b[6], tv.u); b[7] = std::max(b[7], tv.v);
			}
			char vs[400]; int o = 0;
			for (int k = 0; k < std::min(stvDiTVn, 6); k++)
				o += snprintf(vs + o, sizeof(vs) - o, " (%.2f,%.2f|%.4f,%.4f|%08x)", stvDiTV[k].x, stvDiTV[k].y, stvDiTV[k].u, stvDiTV[k].v, stvDiTV[k].color0_32);
			if (stvDiModo >= 4) {
				for (int k0 = 0; k0 < stvDiTVn; k0 += 8) {
					char ls[800]; int lo = 0;
					for (int k = k0; k < std::min(stvDiTVn, k0 + 8); k++)
						lo += snprintf(ls + lo, sizeof(ls) - lo, " %.2f,%.2f,%.3f,%.4f,%.4f,%.3f", stvDiTV[k].x, stvDiTV[k].y, stvDiTV[k].pos_w, stvDiTV[k].u, stvDiTV[k].v, stvDiTV[k].uv_w);
					STV_LOG("STVDI4 f=%d i=%d k=%d%s", stvDiFrame, i, k0, ls);
				}
			}
			STV_LOG("STVDI3 f=%d i=%d n=%d xy=%.2f,%.2f-%.2f,%.2f uv=%.4f,%.4f-%.4f,%.4f uvscale=%.5f,%.5f,%.5f,%.5f v:%s", stvDiFrame, i, stvDiTVn, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
				gstate_c.uv.uScale, gstate_c.uv.vScale, gstate_c.uv.uOff, gstate_c.uv.vOff, vs);
		}
		if (stvDiModo >= 2) {
			STV_LOG("STVDI5 f=%d i=%d texaddr=%08x tw=%d th=%d bufw=%d clamp=%d/%d uvgen=%d uvproj=%d curTex=%dx%d off=%d,%d fbaddr=%08x fbw=%d fmt=%d ofs=%d,%d",
				stvDiFrame, i, gstate.getTextureAddress(0), gstate.getTextureWidth(0), gstate.getTextureHeight(0), gstate.getTextureWidth(0) ? (int)(gstate.texbufwidth[0] & 0x7FF) : 0,
				gstate.isTexCoordClampedS() ? 1 : 0, gstate.isTexCoordClampedT() ? 1 : 0, (int)gstate.getUVGenMode(), (int)gstate.getUVProjMode(),
				(int)gstate_c.curTextureWidth, (int)gstate_c.curTextureHeight, gstate_c.curTextureXOffset, gstate_c.curTextureYOffset,
				gstate.getFrameBufAddress(), gstate.FrameBufStride(), (int)gstate.FrameBufFormat(), gstate.getOffsetX16(), gstate.getOffsetY16());
			STV_LOG("STVDI2 f=%d i=%d bA=%d bB=%d beq=%d fixA=%06x fixB=%06x sfunc=%d sref=%02x smask=%02x sop=%d/%d/%d swm=%02x zf=%d afunc=%d aref=%02x tfn=%d talpha=%d dbl=%d amask=%d",
				stvDiFrame, i, (int)gstate.getBlendFuncA(), (int)gstate.getBlendFuncB(), (int)gstate.getBlendEq(), gstate.getFixA(), gstate.getFixB(),
				(int)gstate.getStencilTestFunction(), gstate.getStencilTestRef(), gstate.getStencilTestMask(),
				(int)gstate.getStencilOpSFail(), (int)gstate.getStencilOpZFail(), (int)gstate.getStencilOpZPass(), (int)gstate.getStencilWriteMask(),
				(int)gstate.getDepthTestFunction(), (int)gstate.getAlphaTestFunction(), gstate.getAlphaTestRef(),
				(int)gstate.getTextureFunction(), gstate.isTextureAlphaUsed() ? 1 : 0, gstate.isColorDoublingEnabled() ? 1 : 0, gstate.isClearModeAlphaMask() ? 1 : 0);
		}
		STV_LOG("STVDI f=%d i=%d rt=%08x v=%d prim=%d bl=%d thr=%d zt=%d zw=%d st=%d fog=%d at=%d clr=%d cmask=%06x minf=%d magf=%d tx=%s",
			stvDiFrame, i, gstate.getFrameBufRawAddress(), verts, prim, (int)k.blendEnable, gstate.isModeThrough() ? 1 : 0,
			gstate.isDepthTestEnabled() ? 1 : 0, gstate.isDepthWriteEnabled() ? 1 : 0, gstate.isStencilTestEnabled() ? 1 : 0,
			gstate.isFogEnabled() ? 1 : 0, gstate.isAlphaTestEnabled() ? 1 : 0, clear ? 1 : 0, gstate.getColorMask() & 0xFFFFFF,
			(int)gstate.isMinifyFilteringEnabled(), (int)gstate.isMagnifyFilteringEnabled(), t);
	}
	return stvDiA >= 0 && i >= stvDiA && i < stvDiB && StvAbActivo();
}

// STV_TEXSONDA_v1 (arco GoS 1:1 STV): sonda para separar el costo de TEXTURA del de fragmentos en el
// pase principal. debug.stv.texsonda=2: sampler nearest para toda textura que no sea framebuffer (un
// texel por muestra, misma huella). Rompe la imagen a proposito. (El modo 1, vista nula 1x1, se
// quito: la vista nula no coincide con el tipo de vista que esperan los sombreadores y se caia.)
static int stvTexSondaModo = 0;
static VkSampler stvTexSondaNearest = VK_NULL_HANDLE;
static FramebufferManagerCommon *stvTsFbm = nullptr;
static void StvTexSonda(VkImageView &view, VkSampler &sampler) {
	// modo 3 (instrumento, no exacto): las lecturas de un framebuffer ESCALADO hacia uno al menos 2 veces
	// mas chico (la bajada del bloom) con sampler nearest: mide cuanto cuesta el filtro de 2 filas.
	if (stvTexSondaModo == 3 && stvDiTex && stvDiTex->StvFbTexturaActual() && stvTsFbm && StvAbActivo()) {
		const VirtualFramebuffer *src = stvDiTex->StvFbTexturaActual();
		const VirtualFramebuffer *rt = stvTsFbm->GetCurrentRenderVFB();
		if (rt && src->renderScaleFactor >= 2.0f * rt->renderScaleFactor && stvTexSondaNearest != VK_NULL_HANDLE)
			sampler = stvTexSondaNearest;
		return;
	}
	if (stvTexSondaModo <= 0 || !stvDiTex || stvDiTex->StvFbTexturaActual())
		return;
	if (stvTexSondaModo == 2 && stvTexSondaNearest != VK_NULL_HANDLE)
		sampler = stvTexSondaNearest;
	(void)view;
}

void DrawEngineVulkan::BeginFrame() {
	DrawEngineCommon::BeginFrame();
	stvDiTex = textureCache_;  // STV_DRAWINFO_v1
	StvDrawInfoCuadro();
	// STV_AB_v1: fase del cuadro y pase de marca en la fase B (ver Common/StvProp.h)
	{
		static int faseAnt = -1;
		StvAbFase() = StvPropInt("debug.stv.ab") == 1 ? 1 : 0;
		if (StvAbFase() != faseAnt) {   // el estado cacheado (pipeline, stencil, blend) puede depender de la fase
			faseAnt = StvAbFase();
			gstate_c.Dirty(DIRTY_DEPTHSTENCIL_STATE | DIRTY_BLEND_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_RASTER_STATE);
		}
	}
	StvAbGate() = StvPropInt("debug.stv.abgate");
	StvSombra::InicioCuadro();  // STV_SOMBRA_RECORTE_v1
	if ((StvAbGate() || StvPropInt("debug.stv.abmarca") == 1) && StvAbFase() == 1 && draw_) {
		static Draw::Framebuffer *stvMarca = nullptr;
		if (!stvMarca) stvMarca = draw_->CreateFramebuffer({ 32, 32, 1, 1, 0, false, "STVMarcaAB" });
		if (stvMarca) {
			draw_->BindFramebufferAsRenderTarget(stvMarca, { Draw::RPAction::CLEAR, Draw::RPAction::CLEAR, Draw::RPAction::CLEAR, 0xFF00FF00 }, "STVMarcaAB");
			draw_->Invalidate(InvalidationFlags::CACHED_RENDER_STATE);
		}
	}
	stvTexSondaModo = StvPropInt("debug.stv.texsonda");  // STV_TEXSONDA_v1
	stvTexSondaNearest = samplerSecondaryNearest_;
	stvTsFbm = framebufferManager_;

	lastPipeline_ = nullptr;

	// These will be re-bound if needed, let's not let old bindings linger around too long.
	boundDepal_ = VK_NULL_HANDLE;
	boundSecondary_ = VK_NULL_HANDLE;

	// pushUBO is the thin3d push pool, don't need to BeginFrame again.
	pushVertex_->BeginFrame();
	pushIndex_->BeginFrame();

	tessDataTransferVulkan->SetPushPool(pushUBO_);

	DirtyAllUBOs();

	AssertEmpty();
}

void DrawEngineVulkan::EndFrame() {
	stats_.pushVertexSpaceUsed = (int)pushVertex_->GetUsedThisFrame();
	stats_.pushIndexSpaceUsed = (int)pushIndex_->GetUsedThisFrame();

	AssertEmpty();
}

void DrawEngineVulkan::DirtyAllUBOs() {
	baseUBOOffset = 0;
	lightUBOOffset = 0;
	boneUBOOffset = 0;
	baseBuf = VK_NULL_HANDLE;
	lightBuf = VK_NULL_HANDLE;
	boneBuf = VK_NULL_HANDLE;
	dirtyUniforms_ = DIRTY_BASE_UNIFORMS | DIRTY_LIGHT_UNIFORMS | DIRTY_BONE_UNIFORMS;
	imageView = VK_NULL_HANDLE;
	sampler = VK_NULL_HANDLE;
	gstate_c.Dirty(DIRTY_TEXTURE_IMAGE);
}

void DrawEngineVulkan::Invalidate(InvalidationCallbackFlags flags) {
	if (flags & InvalidationCallbackFlags::COMMAND_BUFFER_STATE) {
		// Nothing here anymore (removed the "frame descriptor set"
		// If we add back "seldomly-changing" descriptors, we might use this again.
	}
	if (flags & InvalidationCallbackFlags::RENDER_PASS_STATE) {
		// If have a new render pass, dirty our dynamic state so it gets re-set.
		//
		// Dirty everything that has dynamic state that will need re-recording.
		gstate_c.Dirty(DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_BLEND_STATE | DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS);
		lastPipeline_ = nullptr;
	}
}

// The inline wrapper in the header checks for numDrawCalls_ == 0
// STV: contadores para decidir dos lineas SIN adivinar.
//   stvDrawsSinDepth: draws que caen en el workaround NO_DEPTH_CANNOT_DISCARD
//                     (si es 0, quitar el workaround no puede dar nada)
//   stvCopiasBlend:   veces que hay que COPIAR el render target entero para
//                     poder leerlo (framebufferFetch esta en false duro en
//                     Vulkan); cada una corta el render pass en un tiler.
// Se vuelcan por prop debug.stv.fbcnt para no ensuciar cuando no se miden.
static uint32_t stvDrawsSinDepth = 0;
static uint32_t stvCopiasBlend = 0;
static uint32_t stvDrawsTotal = 0;
void StvContarDraw(bool sinDepth) { stvDrawsTotal++; if (sinDepth) stvDrawsSinDepth++; }
void StvContarCopiaBlend() { stvCopiasBlend++; }
void StvVolcarContadores() {
	INFO_LOG(Log::G3D, "STVFB: draws=%u sin_depth_write=%u copias_blend=%u",
		stvDrawsTotal, stvDrawsSinDepth, stvCopiasBlend);
	stvDrawsTotal = 0; stvDrawsSinDepth = 0; stvCopiasBlend = 0;
}

// STV: descripcion compacta del primer draw de un paso de render (instrumento
// del perfil por pase): primitiva, vertices, through/HW y el estado que decide
// si el draw escribe cada pixel sin leer el anterior.
static void StvDescribirPrimerDraw(VulkanRenderManager *rm, int prim, int verts, bool hw) {
	if (!rm->StvPasoSinDraws()) return;
	char b[96];
	snprintf(b, sizeof(b), "p%d v%d %s%s bl%d at%d ct%d cm%06x z%d/%d/%d st%d lo%d",
		prim, verts, hw ? "hw" : "sw", gstate.isModeThrough() ? "T" : "", (int)gstate.isAlphaBlendEnabled(),
		gstate.isAlphaTestEnabled() ? (int)gstate.getAlphaTestFunction() : -1, (int)gstate.isColorTestEnabled(),
		gstate.getColorMask() & 0xFFFFFF, (int)gstate.isDepthTestEnabled(), (int)gstate.isDepthWriteEnabled(), (int)gstate.getDepthTestFunction(),
		gstate.isStencilTestEnabled() ? (int)gstate.getStencilTestFunction() : -1, gstate.isLogicOpEnabled() ? (int)gstate.getLogicOp() : -1);
	rm->StvPrimerDraw(b);
}

// STV (fsbits): histograma por segundo de los bits del fragment shader de los
// draws (y del subconjunto CON blend), para saber que llevan los shaders de las
// capas transparentes que dominan el pase principal. Con debug.stv.gpuprof=1.
// Y la biseccion por bits: debug.stv.skipdraw 64 = saltear draws con alpha
// test, 128 = con niebla, 256 = con depal en shader, 512 = con textura.
static FShaderID stvUltimoFs;   // STV: ID del ultimo fragment shader calculado (los draws sin cambio de estado lo reutilizan)
static VShaderID stvUltimoVs;   // STV: idem, vertex shader (instrumento STV_VOLUMEN_v1 nivel 2)
// STV_CLASIF_v1 (arco Dante, 2026-10-01): histograma por segundo de los draws por categoria
// (textura, blend, through, stencil, color enmascarado, escribe depth, modo clear) con su suma de
// vertices, y biseccion extra por debug.stv.skipdraw: 1024 = sin textura, 2048 = through,
// 4096 = con stencil test, 8192 = color totalmente enmascarado, 16384 = >= 500 vertices,
// 32768 = sin textura y NO through. Con debug.stv.gpuprof=1 imprime STVCLASIF.
// STV_VOLUMEN_v1: con debug.stv.volumen=1 vuelca (40 lineas, una por estado distinto) el estado
// completo de los draws con stencil test: shader de fragmentos, mascara de escritura del pipeline,
// ops de stencil, cull, funcion de depth, primitiva y vertices. Para abaratar las sombras por
// volumen sin cambiar la imagen hay que saber EXACTAMENTE con que estado se dibujan.
static bool StvClasif(int verts, const VulkanPipelineRasterStateKey &k, int prim) {
	static int modo = -1, mask = -1, vol = -1;
	if (modo < 0) { modo = StvPropInt("debug.stv.gpuprof"); mask = StvPropInt("debug.stv.skipdraw"); vol = StvPropInt("debug.stv.volumen"); }
	if (vol > 0 && gstate.isStencilTestEnabled() && !gstate.isModeClear()) {
		static int nVol = 0; static uint64_t vistos[64]; static int nVistos = 0;
		uint64_t h = (uint64_t)stvUltimoFs.d[0] * 31 + stvUltimoFs.d[1];
		h = h * 131 + (uint64_t)k.colorWriteMask + ((uint64_t)k.cullMode << 4) + ((uint64_t)k.stencilPassOp << 8) + ((uint64_t)k.stencilDepthFailOp << 12) + ((uint64_t)k.stencilFailOp << 16) + ((uint64_t)k.depthCompareOp << 20) + ((uint64_t)k.blendEnable << 24) + ((uint64_t)prim << 28);
		h = h * 7 + gstate.stencilop + ((uint64_t)gstate.stenciltest << 24) + ((uint64_t)gstate.getColorMask() << 32);
		bool nuevo = true;
		for (int i = 0; i < nVistos; i++) if (vistos[i] == h) { nuevo = false; break; }
		if (nuevo && nVol < 40) {
			if (nVistos < 64) vistos[nVistos++] = h;
			nVol++;
			STV_LOG("STVVOLUMEN dclamp=%d prim=%d verts=%d thr=%d pmsk=%08x vkmask=%x blend=%d cullEn=%d cull=%d vkcull=%d ztest=%d zfunc=%d vkz=%d zw=%d stfunc=%d ref=%02x msk=%02x ops(sf,zf,zp)=%d,%d,%d vkops=%d,%d,%d wmask=%02x fs=[%s]",
				(int)k.depthClampEnable, prim, verts, gstate.isModeThrough() ? 1 : 0, gstate.getColorMask(), k.colorWriteMask, k.blendEnable,
				gstate.isCullEnabled() ? 1 : 0, (int)gstate.getCullMode(), k.cullMode,
				gstate.isDepthTestEnabled() ? 1 : 0, (int)gstate.getDepthTestFunction(), k.depthCompareOp, gstate.isDepthWriteEnabled() ? 1 : 0,
				(int)gstate.getStencilTestFunction(), gstate.getStencilTestRef(), gstate.getStencilTestMask(),
				(int)gstate.getStencilOpSFail(), (int)gstate.getStencilOpZFail(), (int)gstate.getStencilOpZPass(),
				k.stencilFailOp, k.stencilDepthFailOp, k.stencilPassOp, gstate.getStencilWriteMask(),
				FragmentShaderDesc(stvUltimoFs).c_str());
		}
	}
	// Nivel 2: TODOS los draws, uno por par de shaders distinto, con el shader de vertices.
	if (vol >= 2) {
		static int nV2 = 0; static uint64_t vistos2[96]; static int nVistos2 = 0;
		uint64_t h = ((uint64_t)stvUltimoFs.d[0] * 31 + stvUltimoFs.d[1]) * 1000003ull + (uint64_t)stvUltimoVs.d[0] * 131 + stvUltimoVs.d[1] + ((uint64_t)gstate.vertType << 40);
		bool nuevo = true;
		for (int i = 0; i < nVistos2; i++) if (vistos2[i] == h) { nuevo = false; break; }
		if (nuevo && nV2 < 80) {
			if (nVistos2 < 96) vistos2[nVistos2++] = h;
			nV2++;
			STV_LOG("STVSHADERS verts=%d prim=%d vtype=%08x st=%d vs=[%s] fs=[%s]", verts, prim, gstate.vertType, gstate.isStencilTestEnabled() ? 1 : 0,
				VertexShaderDesc(stvUltimoVs).c_str(), FragmentShaderDesc(stvUltimoFs).c_str());
		}
	}
	if (modo <= 0 && mask <= 0) return false;
	bool clear = gstate.isModeClear();
	bool tex = gstate.isTextureMapEnabled() && !clear;
	bool bl = gstate.isAlphaBlendEnabled() && !clear;
	bool thr = gstate.isModeThrough();
	bool st = gstate.isStencilTestEnabled() && !clear;
	bool cm = (gstate.getColorMask() & 0xFFFFFF) == 0xFFFFFF;
	bool zw = gstate.isDepthWriteEnabled();
	if (modo >= 1) {
		static int n[128] = {}; static long v[128] = {}; static double t0 = 0;
		int k = (tex ? 1 : 0) | (bl ? 2 : 0) | (thr ? 4 : 0) | (st ? 8 : 0) | (cm ? 16 : 0) | (zw ? 32 : 0) | (clear ? 64 : 0);
		n[k]++; v[k] += verts;
		double t = time_now_d();
		if (t - t0 > 1.0) {
			t0 = t;
			for (int i = 0; i < 128; i++) {
				if (!n[i]) continue;
				STV_LOG("STVCLASIF tex=%d bl=%d thr=%d st=%d cmask=%d zw=%d clr=%d draws=%d verts=%ld", i & 1, (i >> 1) & 1, (i >> 2) & 1, (i >> 3) & 1, (i >> 4) & 1, (i >> 5) & 1, (i >> 6) & 1, n[i], v[i]);
			}
			memset(n, 0, sizeof(n)); memset(v, 0, sizeof(v));
		}
	}
	if (mask > 0) {
		if ((mask & 1024) && !tex) return true;
		if ((mask & 2048) && thr) return true;
		if ((mask & 4096) && st) return true;
		if ((mask & 8192) && cm) return true;
		if ((mask & 16384) && verts >= 500) return true;
		if ((mask & 32768) && !tex && !thr) return true;
	}
	return false;
}

static bool StvFsBits(const FShaderID &id, bool blend) {
	static int modo = -1, mask = -1;
	if (modo < 0) { modo = StvPropInt("debug.stv.gpuprof"); mask = StvPropInt("debug.stv.skipdraw"); }
	bool at = id.Bit(FS_BIT_ALPHA_TEST), az = id.Bit(FS_BIT_ALPHA_AGAINST_ZERO), fog = id.Bit(FS_BIT_ENABLE_FOG);
	bool tex = id.Bit(FS_BIT_DO_TEXTURE), depal = id.Bits(FS_BIT_SHADER_DEPAL_MODE, 2) != 0, ct = id.Bit(FS_BIT_COLOR_TEST);
	bool rb = id.Bits(FS_BIT_REPLACE_BLEND, 3) != 0, ta = id.Bit(FS_BIT_TEXALPHA);
	if (modo >= 1) {
		static int n[2][9] = {}; static double t0 = 0;
		int b = blend ? 1 : 0;
		n[b][0]++; n[b][1] += at; n[b][2] += az; n[b][3] += fog; n[b][4] += tex; n[b][5] += depal; n[b][6] += ct; n[b][7] += rb; n[b][8] += ta;
		double t = time_now_d();
		if (t - t0 > 1.0) {
			t0 = t;
			for (int k = 0; k < 2; k++)
				STV_LOG("STVFSBITS %s: draws=%d alphatest=%d contra0=%d niebla=%d textura=%d depal=%d colortest=%d replaceblend=%d texalpha=%d", k ? "CON blend" : "sin blend", n[k][0], n[k][1], n[k][2], n[k][3], n[k][4], n[k][5], n[k][6], n[k][7], n[k][8]);
			memset(n, 0, sizeof(n));
		}
	}
	if (mask > 0) {
		if ((mask & 64) && at) return true;
		if ((mask & 128) && fog) return true;
		if ((mask & 256) && depal) return true;
		if ((mask & 512) && tex) return true;
	}
	return false;
}

// ===== STV_DRAW_NULO_v1 (arco GoS 1:1 STV, 2026-10-02) ============================================
// Un draw cuya mezcla es dst*1 + src*0 (FIX 0xFFFFFF y FIX 0) y que no escribe profundidad ni
// stencil/alfa (con el test de stencil apagado PPSSPP no escribe alfa) no cambia ningun bit: no se
// envia. Ghost of Sparta dibuja uno a pantalla entera leyendo el framebuffer principal (2 % de la GPU).
// debug.stv.nulo=0 lo apaga; respeta STV_AB_v1.
static bool StvDrawNulo() {
	static int modo = -1;
	if (modo < 0) modo = StvPropDef("debug.stv.nulo", 1);
	if (modo < 1 || !StvAbActivo() || gstate.isModeClear()) return false;
	if (!gstate.isAlphaBlendEnabled() || gstate.getBlendEq() != GE_BLENDMODE_MUL_AND_ADD) return false;
	if (gstate.getBlendFuncA() != GE_SRCBLEND_FIXA || gstate.getFixA() != 0) return false;
	if (gstate.getBlendFuncB() != GE_DSTBLEND_FIXB || gstate.getFixB() != 0xFFFFFF) return false;
	if (gstate.isLogicOpEnabled() || gstate.isStencilTestEnabled()) return false;
	if (gstate.isDepthTestEnabled() && gstate.isDepthWriteEnabled()) return false;
	static int n = 0;
	if (modo >= 2 && (++n % 600) == 1) STV_LOG("STVNULO: draw nulo salteado (%d)", n);
	return true;
}

// ===== STV_SOMBRA_RECORTE_v1 =====================================================================
Mat4F32 ComputeFinalProjMatrix();  // DrawEngineCommon.cpp
static GEBufferFormat StvSomFmtTex(GETextureFormat f) { return f == GE_TFMT_8888 ? GE_FORMAT_8888 : GE_FORMAT_565; }
static bool StvSomTexFmtOk(GETextureFormat f) { return f == GE_TFMT_5650 || f == GE_TFMT_5551 || f == GE_TFMT_4444 || f == GE_TFMT_8888; }
static int StvSomTexStride() { return gstate.texbufwidth[0] & 0x7FF; }
static int StvSomPitchRT() { return gstate.FrameBufStride() * StvSombra::Bpp(gstate.FrameBufFormat()); }

// 0 = nada que hacer en la CPU, 1 = silueta (destino dentro de un area), 2 = proyeccion candidata.
int DrawEngineVulkan::StvSomClasificarHW() {
	using namespace StvSombra;
	if (Modo() < 1) return 0;
	VramIntacta();
	if (Modo() >= 3 && (stvDiFrame % 120) == 0)
		STV_LOG("STVSOMBRA3 HW i=%d rt=%08x through=%d areas=%d tex=%08x uvgen=%d", stvDiIdx, gstate.getFrameBufAddress(), gstate.isModeThrough() ? 1 : 0, (int)Areas().size(), gstate.getTextureAddress(0), (int)gstate.getUVGenMode());
	if (Areas().empty()) return 0;
	const u32 rt = gstate.getFrameBufAddress();
	const int pitch = StvSomPitchRT();
	int x, y;
	Area *aSil = nullptr;
	if (!gstate.isModeThrough() && !gstate.isModeClear() && (aSil = AreaDe(rt, pitch, &x, &y)) != nullptr) {
		if ((lastVType_ & GE_VTYPE_WEIGHT_MASK) && !applySkinInDecode_) { Escritura(rt, pitch); return 0; }
		// presupuesto por cuadro: GoS dibuja 4 siluetas (~2.700 vertices). Un area que recibe decenas de draws
		// 3D es la escena (en GoW CoO un borrado negro creaba un area sobre todo el framebuffer): dejar de
		// seguirla, sin gastar CPU transformando la escena entera.
		aSil->nSil++; aSil->nVert += numDrawVerts_ > 0 ? (int)ComputeNumVertsToDecode() : 0;
		if (aSil->nSil > 16 || aSil->nVert > 12000) { Invalidar(*aSil); return 0; }
		return 1;
	}
	const VirtualFramebuffer *vfb = framebufferManager_->GetCurrentRenderVFB();
	Escritura(rt, pitch, vfb ? vfb->height : 512);
	if (!Activo() || gstate.isModeThrough() || gstate.isModeClear() || !gstate.isTextureMapEnabled()) return 0;
	if (gstate.getUVGenMode() != GE_TEXMAP_TEXTURE_MATRIX || gstate.getUVProjMode() != GE_PROJMAP_POSITION) return 0;
	St().cand++;
	int r = 0;
	if (!gstate.isAlphaBlendEnabled() || gstate.getBlendEq() != GE_BLENDMODE_MUL_AND_ADD) r = 1;
	else if (gstate.getBlendFuncA() != GE_SRCBLEND_FIXA || gstate.getFixA() != 0 || gstate.getBlendFuncB() != GE_DSTBLEND_INVSRCCOLOR) r = 2;
	else if (gstate.getTextureFunction() != GE_TEXFUNC_REPLACE || gstate.isColorDoublingEnabled() || gstate.isFogEnabled()) r = 3;
	else if (gstate.isLogicOpEnabled() || gstate.isColorTestEnabled() || gstate.isStencilTestEnabled()) r = 4;
	else if (gstate.isDepthTestEnabled() && gstate.isDepthWriteEnabled()) r = 5;
	else if (!gstate.isTexCoordClampedS() || !gstate.isTexCoordClampedT()) r = 6;
	else if (!StvSomTexFmtOk(gstate.getTextureFormat())) r = 7;
	else if ((lastVType_ & GE_VTYPE_WEIGHT_MASK) && !applySkinInDecode_) r = 8;
	else if (!AreaDe(gstate.getTextureAddress(0), StvSomTexStride() * Bpp(StvSomFmtTex(gstate.getTextureFormat())), &x, &y)) r = 10;
	if (r) { St().rech[r]++; return 0; }
	return 2;
}

void DrawEngineVulkan::StvSomSilueta(GEPrimitiveType prim, int vertexCount, bool useElements, VulkanRenderManager *rm) {
	using namespace StvSombra;
	float m[16];
	ComputeFinalProjMatrix().Store(m);
	const DecVtxFormat &f = dec_->GetDecVtxFmt();
	static std::vector<float> xy;
	xy.resize((size_t)numDecodedVerts_ * 2);
	Caja c = { 1e9f, 1e9f, -1e9f, -1e9f };
	for (int i = 0; i < numDecodedVerts_; i++) {
		const float *p = (const float *)(decoded_ + i * f.stride + f.posoff);
		const float X = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
		const float Y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
		const float W = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
		if (!(W > 1e-4f)) { Escritura(gstate.getFrameBufAddress(), StvSomPitchRT()); return; }
		xy[i * 2] = X / W; xy[i * 2 + 1] = Y / W;
		c.x1 = std::min(c.x1, xy[i * 2]); c.y1 = std::min(c.y1, xy[i * 2 + 1]); c.x2 = std::max(c.x2, xy[i * 2]); c.y2 = std::max(c.y2, xy[i * 2 + 1]);
	}
	if (c.x1 > c.x2) return;
	// acotar el area de render del pase a la caja del draw (todos los vertices adelante de la camara: la caja
	// en pantalla contiene cada fragmento). Mismo mecanismo que el area de los draws through (f27).
	{
		const VirtualFramebuffer *vfb = framebufferManager_->GetCurrentRenderVFB();
		if (rm && vfb && vfb->renderScaleFactor > 0.0f) {
			const float e = vfb->renderScaleFactor;
			const int offX = std::max(gstate_c.curRTOffsetX, 0), offY = std::max(gstate_c.curRTOffsetY, 0);
			rm->StvAcotarProximoDraw(std::max(0, (int)floorf((c.x1 + offX) * e) - 2), std::max(0, (int)floorf((c.y1 + offY) * e) - 2),
				(int)ceilf((c.x2 + offX) * e) + 2, (int)ceilf((c.y2 + offY) * e) + 2);
		}
	}
	// v2: una caja por triangulo (la silueta ocupa mucho menos que su caja envolvente)
	if (prim == GE_PRIM_TRIANGLES && vertexCount >= 3) {
		bool ok = true;
		for (int i = 0; i < vertexCount && ok; i++) if ((useElements ? decIndex_[i] : i) >= numDecodedVerts_) ok = false;
		if (ok) {
			for (int t = 0; t + 2 < vertexCount; t += 3) {
				int k0 = useElements ? decIndex_[t] : t, k1 = useElements ? decIndex_[t + 1] : t + 1, k2 = useElements ? decIndex_[t + 2] : t + 2;
				const Caja ct = { std::min({ xy[k0 * 2], xy[k1 * 2], xy[k2 * 2] }), std::min({ xy[k0 * 2 + 1], xy[k1 * 2 + 1], xy[k2 * 2 + 1] }),
					std::max({ xy[k0 * 2], xy[k1 * 2], xy[k2 * 2] }), std::max({ xy[k0 * 2 + 1], xy[k1 * 2 + 1], xy[k2 * 2 + 1] }) };
				Sucio3D(gstate.getFrameBufAddress(), gstate.FrameBufStride(), gstate.FrameBufFormat(), ct);
			}
			return;
		}
	}
	Sucio3D(gstate.getFrameBufAddress(), gstate.FrameBufStride(), gstate.FrameBufFormat(), c);
}

// Devuelve false si el draw no cambia NADA (se puede saltear). Si devuelve true y el scissor quedo
// definido (*sx2 > *sx1), es la caja en pixeles de render del render target.
bool DrawEngineVulkan::StvSomProyeccion(GEPrimitiveType prim, int vertexCount, bool useElements, int *sx1, int *sy1, int *sx2, int *sy2, std::vector<StvSomSub> *subs) {
	using namespace StvSombra;
	*sx1 = *sy1 = *sx2 = *sy2 = 0;
	St().proyecciones++;
	if (prim != GE_PRIM_TRIANGLES) return true;
	const int bpp = Bpp(StvSomFmtTex(gstate.getTextureFormat()));
	int tx, ty;
	Area *a = AreaDe(gstate.getTextureAddress(0), StvSomTexStride() * bpp, &tx, &ty);
	if (!a) return true;
	const int tw = gstate.getTextureWidth(0), th = gstate.getTextureHeight(0);
	const Caja T = { (float)tx, (float)ty, (float)(tx + tw * bpp), (float)(ty + th) };   // bytes x filas
	if (!Dentro(T, a->rect)) return true;
	// v2: tiras de celdas sucias dentro de la textura, en TEXELS relativos a la textura
	static std::vector<Caja> tiras;
	a->Tiras(T, &tiras);
	if (tiras.empty()) { St().salteadas++; return false; }   // la casilla entera es cero: el draw no cambia nada
	const float inf = INFINITY;
	struct RU { float u0, u1, v0, v1; };
	static std::vector<RU> rs;
	rs.clear();
	for (const Caja &k : tiras) {
		const Caja R = { std::floor((std::max(k.x1, T.x1) - T.x1) / bpp), std::max(k.y1, T.y1) - T.y1, std::ceil((std::min(k.x2, T.x2) - T.x1) / bpp), std::min(k.y2, T.y2) - T.y1 };
		if (R.x1 >= R.x2 || R.y1 >= R.y2) continue;
		// un texel i influye en las muestras con u*tw en (i-0.5, i+1.5); 1 texel mas de margen. Borde + clamp -> infinito
		rs.push_back(RU{ R.x1 <= 1 ? -inf : (R.x1 - 1.5f) / tw, R.x2 >= tw - 1 ? inf : (R.x2 + 1.5f) / tw,
			R.y1 <= 1 ? -inf : (R.y1 - 1.5f) / th, R.y2 >= th - 1 ? inf : (R.y2 + 1.5f) / th });
	}
	if (rs.empty()) { St().salteadas++; return false; }
	float m[16];
	ComputeFinalProjMatrix().Store(m);
	const float *g = gstate.tgenMatrix;
	const DecVtxFormat &f = dec_->GetDecVtxFmt();
	static std::vector<V7> vs;
	vs.resize(numDecodedVerts_);
	for (int i = 0; i < numDecodedVerts_; i++) {
		const float *p = (const float *)(decoded_ + i * f.stride + f.posoff);
		V7 &v = vs[i];
		v.X = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
		v.Y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
		v.Z = 0.0f;
		v.W = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
		v.u = p[0] * g[0] + p[1] * g[3] + p[2] * g[6] + g[9];
		v.v = p[0] * g[1] + p[1] * g[4] + p[2] * g[7] + g[10];
		v.q = p[0] * g[2] + p[1] * g[5] + p[2] * g[8] + g[11];
	}
	static std::vector<u16> sec;
	const u16 *ind = decIndex_;
	if (!useElements) {
		sec.resize(vertexCount);
		for (int i = 0; i < vertexCount; i++) sec[i] = (u16)i;
		ind = sec.data();
	}
	for (int i = 0; i < vertexCount; i++) if (ind[i] >= numDecodedVerts_) return true;   // indices fuera: no tocar
	// bloques de pantalla de B px PSP: los que algun poligono recortado puede tocar
	const VirtualFramebuffer *vfb = framebufferManager_->GetCurrentRenderVFB();
	if (!vfb || vfb->renderScaleFactor <= 0.0f) return true;
	static int Bp = -1, Bb = -1, nB = 0;
	if (Bp < 0 || (++nB & 63) == 0) { Bp = std::max(2, StvPropDef("debug.stv.sombra.b", 8)); Bb = StvPropInt("debug.stv.sombra.bb"); }
	const int B = (StvAbFase() == 1 && Bb > 1) ? Bb : Bp;
	const int nbx = (vfb->width + B - 1) / B, nby = (vfb->height + B - 1) / B;
	static std::vector<uint8_t> bloques;
	bloques.assign((size_t)nbx * nby, 0);
	bool hay = false;
	// El recorte de f117 (vector por plano): sigue para sombra.alg=0 y como referencia de sombra.cmp=1.
	auto bloquesF117 = [&](std::vector<uint8_t> &bl, bool &hy) {
		std::vector<V7> pol;
		for (int t = 0; t + 2 < vertexCount; t += 3) {
			const V7 *tri[3] = { &vs[ind[t]], &vs[ind[t + 1]], &vs[ind[t + 2]] };
			// rechazo rapido: con q > 0 en los 3 vertices, la caja (u/q, v/q) del triangulo contiene la de cualquier punto
			bool qpos = tri[0]->q > 0 && tri[1]->q > 0 && tri[2]->q > 0;
			float tu0 = 0, tu1 = 0, tv0 = 0, tv1 = 0;
			if (qpos) {
				tu0 = tu1 = tri[0]->u / tri[0]->q; tv0 = tv1 = tri[0]->v / tri[0]->q;
				for (int k = 1; k < 3; k++) { const float u = tri[k]->u / tri[k]->q, v = tri[k]->v / tri[k]->q; tu0 = std::min(tu0, u); tu1 = std::max(tu1, u); tv0 = std::min(tv0, v); tv1 = std::max(tv1, v); }
			}
			for (const RU &r : rs) {
				if (qpos && (tu1 < r.u0 || tu0 > r.u1 || tv1 < r.v0 || tv0 > r.v1)) continue;
				for (int sgn = 0; sgn < 2; sgn++) {
					if (qpos && sgn == 1) continue;
					const float sg = sgn == 0 ? 1.0f : -1.0f;
					pol.assign({ *tri[0], *tri[1], *tri[2] });
					Recortar(pol, [](const V7 &v) { return v.W - 1e-4f; });
					Recortar(pol, [&](const V7 &v) { return sg * v.q; });
					if (std::isfinite(r.u0)) Recortar(pol, [&](const V7 &v) { return sg * (v.u - r.u0 * v.q); });
					if (std::isfinite(r.u1)) Recortar(pol, [&](const V7 &v) { return sg * (r.u1 * v.q - v.u); });
					if (std::isfinite(r.v0)) Recortar(pol, [&](const V7 &v) { return sg * (v.v - r.v0 * v.q); });
					if (std::isfinite(r.v1)) Recortar(pol, [&](const V7 &v) { return sg * (r.v1 * v.q - v.v); });
					if (pol.empty()) continue;
					float x1 = 1e9f, y1 = 1e9f, x2 = -1e9f, y2 = -1e9f;
					for (const V7 &v : pol) { const float x = v.X / v.W, y = v.Y / v.W; x1 = std::min(x1, x); y1 = std::min(y1, y); x2 = std::max(x2, x); y2 = std::max(y2, y); }
					const int bx1 = std::max(0, (int)std::floor((x1 - 1.0f) / B)), bx2 = std::min(nbx - 1, (int)std::floor((x2 + 1.0f) / B));
					const int by1 = std::max(0, (int)std::floor((y1 - 1.0f) / B)), by2 = std::min(nby - 1, (int)std::floor((y2 + 1.0f) / B));
					for (int by = by1; by <= by2; by++) for (int bx = bx1; bx <= bx2; bx++) { bl[(size_t)by * nbx + bx] = 1; hy = true; }
				}
			}
		}
	};
	if (Alg() == 0) {
		bloquesF117(bloques, hay);
	} else {
		// STV_SOMBRA_SINHEAP_v1: los mismos bloques que el de f117, sin memoria dinamica y sin repetir trabajo.
		//  - el polígono vive en un arreglo fijo (RecortarF, misma aritmetica que Recortar);
		//  - los recortes por W y por el signo de q son los mismos para todas las tiras: se hacen una vez por signo
		//    (el de f117 los repetia por tira con las mismas entradas: mismo resultado bit a bit);
		//  - marcar es un OR: si todos los bloques que el triangulo PUEDE tocar (CajaTriangulo) ya estan marcados,
		//    no puede agregar nada y se saltea; se vuelve a mirar cada vez que una tira marca un bloque nuevo.
		bool desborde = false;
		auto marcar = [&](const Poli &pol) {
			float x1 = 1e9f, y1 = 1e9f, x2 = -1e9f, y2 = -1e9f;
			for (int i = 0; i < pol.n; i++) { const float x = pol.v[i].X / pol.v[i].W, y = pol.v[i].Y / pol.v[i].W; x1 = std::min(x1, x); y1 = std::min(y1, y); x2 = std::max(x2, x); y2 = std::max(y2, y); }
			const int bx1 = std::max(0, (int)std::floor((x1 - 1.0f) / B)), bx2 = std::min(nbx - 1, (int)std::floor((x2 + 1.0f) / B));
			const int by1 = std::max(0, (int)std::floor((y1 - 1.0f) / B)), by2 = std::min(nby - 1, (int)std::floor((y2 + 1.0f) / B));
			bool nuevo = false;
			for (int by = by1; by <= by2; by++) for (int bx = bx1; bx <= bx2; bx++) {
				uint8_t &b = bloques[(size_t)by * nbx + bx];
				if (!b) { b = 1; nuevo = true; }
				hay = true;
			}
			return nuevo;
		};
		auto todos = [&](const int cb[4]) {
			for (int by = cb[1]; by <= cb[3]; by++) for (int bx = cb[0]; bx <= cb[2]; bx++) if (!bloques[(size_t)by * nbx + bx]) return false;
			return true;
		};
		// caja (u,v) que une todas las tiras: un triangulo con q > 0 que cae fuera de ella no pasa el rechazo rapido
		// de ninguna tira (comparaciones exactas contra el minimo/maximo), y el de f117 no le hacia nada
		RU U = rs[0];
		for (const RU &r : rs) { U.u0 = std::min(U.u0, r.u0); U.u1 = std::max(U.u1, r.u1); U.v0 = std::min(U.v0, r.v0); U.v1 = std::max(U.v1, r.v1); }
		for (int t = 0; t + 2 < vertexCount && !desborde; t += 3) {
			const V7 *tri[3] = { &vs[ind[t]], &vs[ind[t + 1]], &vs[ind[t + 2]] };
			St().tri++;
			bool qpos = tri[0]->q > 0 && tri[1]->q > 0 && tri[2]->q > 0;
			float tu0 = 0, tu1 = 0, tv0 = 0, tv1 = 0;
			if (qpos) {
				tu0 = tu1 = tri[0]->u / tri[0]->q; tv0 = tv1 = tri[0]->v / tri[0]->q;
				for (int k = 1; k < 3; k++) { const float u = tri[k]->u / tri[k]->q, v = tri[k]->v / tri[k]->q; tu0 = std::min(tu0, u); tu1 = std::max(tu1, u); tv0 = std::min(tv0, v); tv1 = std::max(tv1, v); }
				if (tu1 < U.u0 || tu0 > U.u1 || tv1 < U.v0 || tv0 > U.v1) { St().triFuera++; continue; }
			}
			int cb[4];
			const bool caja = CajaTriangulo(tri, B, nbx, nby, cb);
			if (caja && todos(cb)) { St().triSalto++; continue; }
			bool lleno = false;
			for (int sgn = 0; sgn < 2 && !lleno && !desborde; sgn++) {
				if (qpos && sgn == 1) continue;
				const float sg = sgn == 0 ? 1.0f : -1.0f;
				// los recortes por W y por el signo de q, recien cuando una tira acepta el triangulo
				Poli base;
				bool baseHecha = false;
				for (const RU &r : rs) {
					if (qpos && (tu1 < r.u0 || tu0 > r.u1 || tv1 < r.v0 || tv0 > r.v1)) continue;
					if (!baseHecha) {
						baseHecha = true;
						base.Tri(*tri[0], *tri[1], *tri[2]);
						if (!RecortarF(base, [](const V7 &v) { return v.W - 1e-4f; }) || !RecortarF(base, [&](const V7 &v) { return sg * v.q; })) { desborde = true; break; }
					}
					if (base.n == 0) break;   // vacio para todas las tiras
					St().recortes++;
					Poli pol;
					pol.Copiar(base);
					bool ok = true;
					if (ok && std::isfinite(r.u0)) ok = RecortarF(pol, [&](const V7 &v) { return sg * (v.u - r.u0 * v.q); });
					if (ok && std::isfinite(r.u1)) ok = RecortarF(pol, [&](const V7 &v) { return sg * (r.u1 * v.q - v.u); });
					if (ok && std::isfinite(r.v0)) ok = RecortarF(pol, [&](const V7 &v) { return sg * (v.v - r.v0 * v.q); });
					if (ok && std::isfinite(r.v1)) ok = RecortarF(pol, [&](const V7 &v) { return sg * (r.v1 * v.q - v.v); });
					if (!ok) { desborde = true; break; }
					if (pol.n == 0) continue;
					if (marcar(pol) && caja && todos(cb)) { St().llenos++; lleno = true; break; }
				}
			}
		}
		// un poligono que no entro en el arreglo (solo con NaN): sin recorte, el draw entero (exacto)
		if (desborde) { St().desbordes++; return true; }
		if (Cmp() >= 1) {
			static std::vector<uint8_t> ref;
			ref.assign(bloques.size(), 0);
			bool hayRef = false;
			bloquesF117(ref, hayRef);
			long dif = 0;
			for (size_t i = 0; i < ref.size(); i++) if (ref[i] != bloques[i]) dif++;
			if (hayRef != hay) dif++;
			St().cmpProy++;
			if (dif) { St().cmpProyDif++; St().cmpDif += dif; }
		}
	}
	if (!hay) { St().salteadas++; return false; }
	// bloques -> rectangulos disjuntos (tiras por fila, unidas con la fila anterior si coinciden)
	struct RB { int x1, y1, x2, y2; };
	static std::vector<RB> rb;
	rb.clear();
	for (int by = 0; by < nby; by++) {
		int bx = 0;
		while (bx < nbx) {
			while (bx < nbx && !bloques[(size_t)by * nbx + bx]) bx++;
			if (bx >= nbx) break;
			int e = bx;
			while (e < nbx && bloques[(size_t)by * nbx + e]) e++;
			bool unida = false;
			for (RB &k : rb) if (k.x1 == bx && k.x2 == e && k.y2 == by) { k.y2 = by + 1; unida = true; break; }
			if (!unida) rb.push_back(RB{ bx, by, e, by + 1 });
			bx = e;
		}
	}
	// cada rectangulo es un draw aparte (la malla entera se procesa otra vez: trabajo de vertices/tiler):
	// juntar hasta K rectangulos, siempre el par cuya caja unida agrega menos area. Solo se juntan pares
	// cuya caja unida no pisa otro rectangulo (los rectangulos tienen que seguir siendo disjuntos).
	// medido en vivo: 3 mejor que 1, 2 y sin limite. En la fase B del A/B se puede usar otro (sombra.kb).
	static int Kp = -1, Kb = -1, nK = 0;
	if (Kp < 0 || (++nK & 63) == 0) { Kp = std::max(1, StvPropDef("debug.stv.sombra.k", 3)); Kb = StvPropInt("debug.stv.sombra.kb"); }
	const int K = (StvAbFase() == 1 && Kb > 0) ? Kb : Kp;
	auto areaRB = [](const RB &r) { return (long)(r.x2 - r.x1) * (r.y2 - r.y1); };
	while ((int)rb.size() > K) {
		long mejor = -1; size_t mi = 0, mj = 0;
		for (size_t i = 0; i < rb.size(); i++) for (size_t j = i + 1; j < rb.size(); j++) {
			const RB u = { std::min(rb[i].x1, rb[j].x1), std::min(rb[i].y1, rb[j].y1), std::max(rb[i].x2, rb[j].x2), std::max(rb[i].y2, rb[j].y2) };
			bool pisa = false;
			for (size_t k = 0; k < rb.size() && !pisa; k++) {
				if (k == i || k == j) continue;
				if (rb[k].x1 < u.x2 && u.x1 < rb[k].x2 && rb[k].y1 < u.y2 && u.y1 < rb[k].y2) pisa = true;
			}
			if (pisa) continue;
			const long extra = areaRB(u) - areaRB(rb[i]) - areaRB(rb[j]);
			if (mejor < 0 || extra < mejor) { mejor = extra; mi = i; mj = j; }
		}
		if (mejor < 0) {
			// ningun par se puede juntar sin pisar: todo en una caja
			RB u = rb[0];
			for (const RB &r : rb) { u.x1 = std::min(u.x1, r.x1); u.y1 = std::min(u.y1, r.y1); u.x2 = std::max(u.x2, r.x2); u.y2 = std::max(u.y2, r.y2); }
			rb.assign(1, u);
			break;
		}
		const RB u = { std::min(rb[mi].x1, rb[mj].x1), std::min(rb[mi].y1, rb[mj].y1), std::max(rb[mi].x2, rb[mj].x2), std::max(rb[mi].y2, rb[mj].y2) };
		rb[mi] = u; rb.erase(rb.begin() + mj);
	}
	const float e = vfb->renderScaleFactor;
	const int offX = std::max(gstate_c.curRTOffsetX, 0), offY = std::max(gstate_c.curRTOffsetY, 0);
	float ux1 = 1e9f, uy1 = 1e9f, ux2 = -1e9f, uy2 = -1e9f;
	if (subs) subs->clear();
	for (const RB &k : rb) {
		StvSomSub q; q.primero = 0; q.cuenta = vertexCount;
		// bordes de bloque en px PSP -> render; los rectangulos son disjuntos en PSP y se redondean igual en los dos
		// lados, asi que tambien son disjuntos en render (cada pixel se dibuja a lo sumo una vez)
		q.x1 = (int)std::floor((k.x1 * B + offX) * e); q.x2 = (int)std::floor((k.x2 * B + offX) * e);
		q.y1 = (int)std::floor((k.y1 * B + offY) * e); q.y2 = (int)std::floor((k.y2 * B + offY) * e);
		if (subs) subs->push_back(q);
		ux1 = std::min(ux1, (float)q.x1); uy1 = std::min(uy1, (float)q.y1); ux2 = std::max(ux2, (float)q.x2); uy2 = std::max(uy2, (float)q.y2);
		St().areaRecorte2 += (double)(k.x2 - k.x1) * (k.y2 - k.y1) * B * B;
	}
	*sx1 = (int)ux1; *sy1 = (int)uy1; *sx2 = (int)ux2; *sy2 = (int)uy2;
	if (subs && subs->size() <= 1) subs->clear();   // uno solo: alcanza con el scissor de la caja
	St().recortadas++;
	return true;
}

// Camino por software (through, clears): reconocer el rectangulo de bytes cero y la copia del desenfoque.
void DrawEngineVulkan::StvSomThrough(const SoftwareTransformResult &result, bool esClear, GEPrimitiveType prim, const u16 *inds, int nInds) {
	using namespace StvSombra;
	if (Modo() < 1) return;
	VramIntacta();
	const u32 rt = gstate.getFrameBufAddress();
	const int st = gstate.FrameBufStride();
	const GEBufferFormat fmt = gstate.FrameBufFormat();
	const int pitch = StvSomPitchRT();
	const VirtualFramebuffer *vfb = framebufferManager_->GetCurrentRenderVFB();
	const int filas = vfb ? vfb->height : 512;
	const bool sinMascara = (gstate.getColorMask() & 0xFFFFFF) == 0 && !gstate.isLogicOpEnabled();
	// En 8888 el byte de alfa es el stencil: para que el resultado sea CERO tiene que quedar en 0.
	const bool alfaCero = fmt != GE_FORMAT_8888 ||
		(gstate.isStencilTestEnabled() && gstate.getStencilTestFunction() == GE_COMP_ALWAYS && gstate.getStencilOpZPass() == GE_STENCILOP_ZERO &&
		 gstate.getStencilOpZFail() == GE_STENCILOP_ZERO && gstate.getStencilOpSFail() == GE_STENCILOP_ZERO && gstate.getStencilWriteMask() == 0);
	if (esClear && Modo() >= 3 && (stvDiFrame % 120) == 0)
		STV_LOG("STVSOMBRA3 CLEAR i=%d rt=%08x color=%08x cmask=%d", stvDiIdx, rt, result.color, gstate.isClearModeColorMask() ? 1 : 0);
	if (esClear) {
		const bool alfaOk = fmt != GE_FORMAT_8888 || (gstate.isClearModeAlphaMask() && (result.color >> 24) == 0);
		if (gstate.isClearModeColorMask() && sinMascara && (result.color & 0xFFFFFF) == 0 && alfaOk) {
			Caja c = result.stvBboxValido ? Caja{ result.stvBbox[0], result.stvBbox[1], result.stvBbox[2], result.stvBbox[3] }
				: Caja{ 0, 0, (float)(vfb ? vfb->width : 0), (float)filas };
			Negro(rt, st, fmt, c);
		} else if (gstate.isClearModeColorMask() || gstate.isClearModeAlphaMask()) {
			Escritura(rt, pitch, filas);
		}
		return;
	}
	const bool through = gstate.isModeThrough();
	const bool cubre = through && result.stvBboxValido && result.stvCubre;
	const bool testsOk = !gstate.isColorTestEnabled() && (!gstate.isAlphaTestEnabled() || gstate.getAlphaTestFunction() == GE_COMP_ALWAYS) &&
		(!gstate.isDepthTestEnabled() || gstate.getDepthTestFunction() == GE_COMP_ALWAYS) &&
		(!gstate.isStencilTestEnabled() || gstate.getStencilTestFunction() == GE_COMP_ALWAYS);
	const Caja dst = { result.stvBbox[0], result.stvBbox[1], result.stvBbox[2], result.stvBbox[3] };
	const TransformedVertex *tv = transformed_;   // un vertice por vertice decodificado (antes de expandir rects)
	// vertices de ESTE draw (por indices; el buffer puede tener restos). En rectangulos el color sale del
	// SEGUNDO vertice de cada par (el primero se ignora).
	const bool rects = prim == GE_PRIM_RECTANGLES;
	bool indOk = tv && inds && nInds > 0;
	for (int i = 0; indOk && i < nInds; i++) if (inds[i] >= numDecodedVerts_) indOk = false;
	if (Modo() >= 3 && (stvDiFrame % 120) == 0) {
		STV_LOG("STVSOMBRA3 i=%d rt=%08x st=%d fmt=%d through=%d bbox=%d cubre=%d tests=%d sinMasc=%d alfa0=%d tex=%d blend=%d A=%d B=%d nind=%d prim=%d bb=%.0f,%.0f,%.0f,%.0f",
			stvDiIdx, rt, st, (int)fmt, through ? 1 : 0, result.stvBboxValido ? 1 : 0, result.stvCubre ? 1 : 0, testsOk ? 1 : 0, sinMascara ? 1 : 0, alfaCero ? 1 : 0,
			gstate.isTextureMapEnabled() ? 1 : 0, gstate.isAlphaBlendEnabled() ? 1 : 0, (int)gstate.getBlendFuncA(), (int)gstate.getBlendFuncB(), nInds, (int)prim,
			dst.x1, dst.y1, dst.x2, dst.y2);
	}
	if (cubre && testsOk && sinMascara && indOk) {
		if (!gstate.isTextureMapEnabled()) {
			bool negro = true, opaco = true;
			for (int i = rects ? 1 : 0; i < nInds; i += rects ? 2 : 1) {
				const u32 c = tv[inds[i]].color0_32;
				if (c & 0xFFFFFF) negro = false;
				if ((c >> 24) != 0xFF) opaco = false;
			}
			const bool blendCopia = !gstate.isAlphaBlendEnabled() ||
				(opaco && gstate.getBlendEq() == GE_BLENDMODE_MUL_AND_ADD && gstate.getBlendFuncA() == GE_SRCBLEND_SRCALPHA && gstate.getBlendFuncB() == GE_DSTBLEND_INVSRCALPHA);
			if (negro && blendCopia && alfaCero) { Negro(rt, st, fmt, dst); return; }
		} else if (!gstate.isAlphaBlendEnabled() && !gstate.isColorDoublingEnabled() && StvSomTexFmtOk(gstate.getTextureFormat()) && fmt != GE_FORMAT_8888) {
			bool blanco = true;
			for (int i = rects ? 1 : 0; i < nInds; i += rects ? 2 : 1) if ((tv[inds[i]].color0_32 & 0xFFFFFF) != 0xFFFFFF) blanco = false;
			const GETexFunc fn = gstate.getTextureFunction();
			const u32 ta = gstate.getTextureAddress(0);
			if (fn == GE_TEXFUNC_REPLACE || (fn == GE_TEXFUNC_MODULATE && blanco)) {
				// en through las UV de transformed_ estan en TEXELS de la textura (origen = su direccion)
				float u1 = 1e9f, v1 = 1e9f, u2 = -1e9f, v2 = -1e9f;
				for (int i = 0; i < nInds; i++) {
					const TransformedVertex &t = tv[inds[i]];
					u1 = std::min(u1, t.u); u2 = std::max(u2, t.u); v1 = std::min(v1, t.v); v2 = std::max(v2, t.v);
				}
				const Caja src = { u1, v1, u2, v2 };
				const int tw = gstate.getTextureWidth(0), th = gstate.getTextureHeight(0);
				const bool clampEntera = gstate.isTexCoordClampedS() && gstate.isTexCoordClampedT() &&
					fabsf(src.x1) < 0.01f && fabsf(src.y1) < 0.01f && fabsf(src.x2 - tw) < 0.01f && fabsf(src.y2 - th) < 0.01f;
				const int sbpp = Bpp(StvSomFmtTex(gstate.getTextureFormat()));
				if (Modo() >= 3 && (stvDiFrame % 120) == 0)
					STV_LOG("STVSOMBRA3   copia ta=%08x src=%.1f,%.1f-%.1f,%.1f tw=%d th=%d clamp=%d", ta, src.x1, src.y1, src.x2, src.y2, tw, th, clampEntera ? 1 : 0);
				Copia(ta, StvSomTexStride(), sbpp, src, clampEntera, rt, st, Bpp(fmt), dst);
				return;
			}
		}
	}
	Escritura(rt, pitch, filas);
}

void DrawEngineVulkan::Flush() {
	if (!numDrawVerts_) {
		return;
	}

	VulkanRenderManager *renderManager = (VulkanRenderManager *)draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);

	renderManager->AssertInRenderPass();

	PROFILE_THIS_SCOPE("Flush");

	bool tess = gstate_c.submitType == SubmitType::HW_BEZIER || gstate_c.submitType == SubmitType::HW_SPLINE;

	bool textureNeedsApply = false;
	if (gstate_c.IsDirty(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS) && !gstate.isModeClear() && gstate.isTextureMapEnabled()) {
		textureCache_->SetTexture();
		gstate_c.Clean(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS);
		// NOTE: After this is set, we MUST call ApplyTexture before returning.
		textureNeedsApply = true;
	} else if (gstate.getTextureAddress(0) == (gstate.getFrameBufRawAddress() | 0x04000000)) {
		// This catches the case of clearing a texture.
		gstate_c.Dirty(DIRTY_TEXTURE_IMAGE);
	}

	GEPrimitiveType prim = prevPrim_;

	// Always use software for flat shading to fix the provoking index
	// if the provoking vertex extension is not available.
	bool provokingVertexOk = (tess || gstate.getShadeMode() != GE_SHADE_FLAT);
	if (renderManager->GetVulkanContext()->GetDeviceFeatures().enabled.provokingVertex.provokingVertexLast) {
		provokingVertexOk = true;
	}
	bool useHWTransform = CanUseHardwareTransform(prim) && provokingVertexOk && !StvForzarSw();

	// The optimization to avoid indexing isn't really worth it on Vulkan since it means creating more pipelines.
	// This could be avoided with the new dynamic state extensions, but not available enough on mobile.
	const bool forceIndexed = draw_->GetDeviceCaps().verySlowShaderCompiler;

	// STV_VOLDUMP_v1 (arco Dante, instrumento): `setprop debug.stv.voldump N` (N nuevo) guarda los
	// proximos 48 lotes de volumen de sombra (stencil, color enmascarado, sin depth write) en
	// <memstick>/stv_vol_N.bin, uno tras otro: cabecera (vtype, prim, cull, draws, tamaño de
	// vertice), matrices world/view/proj, viewport y offset, y los vertices crudos (+ indices).
	{
		static int ultimoVd = 0, quedan = 0, nProp = 0;
		static double tProp = 0.0;
		static FILE *fv = nullptr;
		// La prop se lee como mucho cada 0,25 s: Flush corre cientos de veces por cuadro.
		double ahora = time_now_d();
		if (ahora - tProp > 0.25) {
			tProp = ahora;
			char v[PROP_VALUE_MAX] = {0};
			nProp = (__system_property_get("debug.stv.voldump", v) > 0) ? atoi(v) : 0;
		}
		int n = nProp;
		if (n != 0 && n != ultimoVd) {
			ultimoVd = n; quedan = 48;
			if (fv) fclose(fv);
			char nombre[64]; snprintf(nombre, sizeof(nombre), "stv_vol_%d.bin", n);
			fv = File::OpenCFile(g_Config.memStickDirectory / nombre, "wb");
			STV_LOG("STVVOLDUMP: abierto %s -> %s", nombre, fv ? "ok" : "FALLO");
		}
		if (fv && quedan > 0 && gstate.isStencilTestEnabled() && !gstate.isModeClear() &&
			(gstate.getColorMask() & 0xFFFFFF) == 0xFFFFFF && !gstate.isDepthWriteEnabled() && numDrawVerts_ > 0) {
			uint32_t cab[8] = { 0x4C4F5653u, lastVType_, (uint32_t)prevPrim_, (uint32_t)gstate.isCullEnabled(), (uint32_t)gstate.getCullMode(), (uint32_t)numDrawVerts_, (uint32_t)numDrawInds_, (uint32_t)dec_->VertexSize() };
			fwrite(cab, sizeof(cab), 1, fv);
			fwrite(gstate.worldMatrix, sizeof(float), 12, fv);
			fwrite(gstate.viewMatrix, sizeof(float), 12, fv);
			fwrite(gstate.projMatrix, sizeof(float), 16, fv);
			float vp[8] = { gstate.getViewportXScale(), gstate.getViewportYScale(), gstate.getViewportZScale(), gstate.getViewportXCenter(), gstate.getViewportYCenter(), gstate.getViewportZCenter(), gstate.getOffsetX(), gstate.getOffsetY() };
			fwrite(vp, sizeof(vp), 1, fv);
			for (int i = 0; i < numDrawVerts_; i++) {
				uint32_t d2[4] = { (uint32_t)drawVerts_[i].vertexCount, drawVerts_[i].indexLowerBound, drawVerts_[i].indexUpperBound, 0 };
				fwrite(d2, sizeof(d2), 1, fv);
				fwrite(drawVerts_[i].verts, dec_->VertexSize(), drawVerts_[i].indexUpperBound + 1, fv);
			}
			for (int i = 0; i < numDrawInds_; i++) {
				uint32_t d3[4] = { (uint32_t)drawInds_[i].vertexCount, (uint32_t)drawInds_[i].indexType, (uint32_t)drawInds_[i].prim, (uint32_t)drawInds_[i].clockwise };
				fwrite(d3, sizeof(d3), 1, fv);
				if (drawInds_[i].inds && drawInds_[i].indexType) fwrite(drawInds_[i].inds, drawInds_[i].indexType == 1 ? 1 : (drawInds_[i].indexType == 2 ? 2 : 4), drawInds_[i].vertexCount, fv);
			}
			if (--quedan == 0) { fclose(fv); fv = nullptr; STV_LOG("STVVOLDUMP: cerrado (48 lotes)"); }
		}
	}

	if (useHWTransform) {
		uint32_t vbOffset;

		VkBuffer vbuf = VK_NULL_HANDLE;
		const int stvSomTipo = StvSomClasificarHW();  // STV_SOMBRA_RECORTE_v1: 1 silueta, 2 proyeccion (necesitan posiciones en CPU)
		if ((applySkinInDecode_ && (lastVType_ & GE_VTYPE_WEIGHT_MASK)) || stvSomTipo != 0) {
			// If software skinning, we're predecoding into "decoded". So make sure we're done, then push that content.
			DecodeVerts(dec_, decoded_);
			VkDeviceSize size = numDecodedVerts_ * dec_->GetDecVtxFmt().stride;
			u8 *dest = (u8 *)pushVertex_->Allocate(size, 4, &vbuf, &vbOffset);
			memcpy(dest, decoded_, size);
		} else {
			// Figure out how much pushbuffer space we need to allocate.
			int vertsToDecode = ComputeNumVertsToDecode();
			// Decode directly into the pushbuffer
			u8 *dest = pushVertex_->Allocate(vertsToDecode * dec_->GetDecVtxFmt().stride, 4, &vbuf, &vbOffset);
			DecodeVerts(dec_, dest);
		}

		int vertexCount;
		int maxIndex;
		bool useElements;
		DecodeIndsAndGetData(&prim, &vertexCount, &maxIndex, &useElements, false);
		// STV_SOMBRA_RECORTE_v1
		bool stvSomSaltar = false;
		int stvSx1 = 0, stvSy1 = 0, stvSx2 = 0, stvSy2 = 0;
		if (stvSomTipo == 1) StvSomSilueta(prim, vertexCount, useElements, renderManager);
		static std::vector<StvSomSub> stvSubs;
		stvSubs.clear();
		if (stvSomTipo == 2) stvSomSaltar = !StvSomProyeccion(prim, vertexCount, useElements, &stvSx1, &stvSy1, &stvSx2, &stvSy2, &stvSubs);
		if (StvDrawNulo()) stvSomSaltar = true;  // STV_DRAW_NULO_v1

		bool hasColor = (lastVType_ & GE_VTYPE_COL_MASK) != GE_VTYPE_COL_NONE;
		if (gstate.isModeThrough()) {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && (hasColor || gstate.getMaterialAmbientA() == 255);
		} else {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && ((hasColor && (gstate.materialupdate & 1)) || gstate.getMaterialAmbientA() == 255) && (!gstate.isLightingEnabled() || gstate.getAmbientA() == 255);
		}

		if (textureNeedsApply) {
			textureCache_->ApplyTexture();
			textureCache_->GetVulkanHandles(imageView, sampler);
			if (imageView == VK_NULL_HANDLE)
				imageView = (VkImageView)draw_->GetNativeObject(gstate_c.textureIsArray ? Draw::NativeObject::NULL_IMAGEVIEW_ARRAY : Draw::NativeObject::NULL_IMAGEVIEW);
			if (sampler == VK_NULL_HANDLE)
				sampler = nullSampler_;
		}

		if (!lastPipeline_ || gstate_c.IsDirty(DIRTY_BLEND_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_RASTER_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_GEOMETRYSHADER_STATE) || prim != lastPrim_) {
			if (prim != lastPrim_ || gstate_c.IsDirty(DIRTY_BLEND_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_RASTER_STATE | DIRTY_DEPTHSTENCIL_STATE)) {
				ConvertStateToVulkanKey(*framebufferManager_, shaderManager_, prim, pipelineKey_, dynState_);
			}

			VulkanVertexShader *vshader = nullptr;
			VulkanFragmentShader *fshader = nullptr;
			VulkanGeometryShader *gshader = nullptr;

			shaderManager_->GetShaders(prim, dec_->VertexType(), &vshader, &fshader, &gshader, pipelineState_, true, useHWTessellation_, decOptions_.expandAllWeightsToFloat, applySkinInDecode_);
			if (fshader) stvUltimoFs = fshader->GetID();
			if (vshader) stvUltimoVs = vshader->GetID();
			_dbg_assert_msg_(vshader->UseHWTransform(), "Bad vshader");
			VulkanPipeline *pipeline = pipelineManager_->GetOrCreatePipeline(renderManager, pipelineLayout_, pipelineKey_, &dec_->decFmt, vshader, fshader, gshader, true, 0, framebufferManager_->GetMSAALevel(), false);
			if (!pipeline || !pipeline->pipeline) {
				// Already logged, let's bail out.
				ResetAfterDraw();
				return;
			}
			BindShaderBlendTex();  // This might cause copies so important to do before BindPipeline.

			if (!renderManager->BindPipeline(pipeline->pipeline, pipeline->pipelineFlags, pipelineLayout_)) {
				renderManager->ReportBadStateForDraw();
				ResetAfterDraw();
				return;
			}
			if (pipeline != lastPipeline_) {
				if (lastPipeline_ && !(lastPipeline_->UsesBlendConstant() && pipeline->UsesBlendConstant())) {
					gstate_c.Dirty(DIRTY_BLEND_STATE);
				}
				lastPipeline_ = pipeline;
			}
			ApplyDrawStateLate(renderManager, false, 0, pipeline->UsesBlendConstant());
			gstate_c.Clean(DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_RASTER_STATE | DIRTY_VIEWPORTSCISSOR_STATE);
			gstate_c.Dirty(dirtyRequiresRecheck_);
			dirtyRequiresRecheck_ = 0;
			lastPipeline_ = pipeline;
		}
		lastPrim_ = prim;

		// STV_SOMBRA_RECORTE_v1: scissor = (scissor del juego) ∩ (caja donde la sombra puede cambiar algo)
		if (stvSx2 > stvSx1 && stvSy2 > stvSy1 && !stvSomSaltar) {
			const int x1 = std::max(stvSx1, dynState_.scissor.x), y1 = std::max(stvSy1, dynState_.scissor.y);
			const int x2 = std::min(stvSx2, dynState_.scissor.x + dynState_.scissor.width), y2 = std::min(stvSy2, dynState_.scissor.y + dynState_.scissor.height);
			if (x2 <= x1 || y2 <= y1) {
				stvSomSaltar = true;
			} else {
				renderManager->SetScissor(x1, y1, x2 - x1, y2 - y1);
				gstate_c.Dirty(DIRTY_VIEWPORTSCISSOR_STATE);   // el proximo draw vuelve a fijar el scissor del juego
				StvSombra::St().areaTotal += (double)dynState_.scissor.width * dynState_.scissor.height;
				StvSombra::St().areaRecorte += (double)(x2 - x1) * (y2 - y1);
			}
		}

		dirtyUniforms_ |= shaderManager_->UpdateUniforms(framebufferManager_->UseBufferedRendering());
		UpdateUBOs();

		int descCount = 6;
		if (tess)
			descCount = 9;
		int descSetIndex;
		PackedDescriptor *descriptors = renderManager->PushDescriptorSet(descCount, &descSetIndex);
		StvTexSonda(imageView, sampler);  // STV_TEXSONDA_v1 (instrumento, apagado por defecto)
		descriptors[0].image.view = imageView;
		descriptors[0].image.sampler = sampler;

		descriptors[1].image.view = boundSecondary_;
		descriptors[1].image.sampler = samplerSecondaryNearest_;

		descriptors[2].image.view = boundDepal_;
		descriptors[2].image.sampler = (boundDepal_ && boundDepalSmoothed_) ? samplerSecondaryLinear_ : samplerSecondaryNearest_;

		descriptors[3].buffer.buffer = baseBuf;
		descriptors[3].buffer.range = sizeof(UB_VS_FS_Base);
		descriptors[3].buffer.offset = 0;

		descriptors[4].buffer.buffer = lightBuf;
		descriptors[4].buffer.range = sizeof(UB_VS_Lights);
		descriptors[4].buffer.offset = 0;

		descriptors[5].buffer.buffer = boneBuf;
		descriptors[5].buffer.range = sizeof(UB_VS_Bones);
		descriptors[5].buffer.offset = 0;
		if (tess) {
			const VkDescriptorBufferInfo *bufInfo = tessDataTransferVulkan->GetBufferInfo();
			for (int j = 0; j < 3; j++) {
				descriptors[j + 6].buffer.buffer = bufInfo[j].buffer;
				descriptors[j + 6].buffer.range = bufInfo[j].range;
				descriptors[j + 6].buffer.offset = bufInfo[j].offset;
			}
		}
		// TODO: Can we avoid binding all three when not needed? Same below for hardware transform.
		// Think this will require different descriptor set layouts.
		const uint32_t dynamicUBOOffsets[3] = {
			baseUBOOffset, lightUBOOffset, boneUBOOffset,
		};
		if (useElements) {
			VkBuffer ibuf;
			u32 ibOffset = (uint32_t)pushIndex_->Push(decIndex_, sizeof(uint16_t) * vertexCount, 4, &ibuf);
			StvDescribirPrimerDraw(renderManager, (int)prim, vertexCount, true);
			if (!StvFsBits(stvUltimoFs, pipelineState_.blendState.blendEnabled) && !StvClasif(vertexCount, pipelineKey_, (int)prim) && !StvDrawInfo(vertexCount, pipelineKey_, (int)prim) && !stvSomSaltar) {
				if (!stvSubs.empty()) {
					// STV_SOMBRA_RECORTE_v1: un draw por grupo de triangulos, cada uno con su scissor (cada triangulo se dibuja UNA vez)
					const int gx1 = dynState_.scissor.x, gy1 = dynState_.scissor.y, gx2 = gx1 + dynState_.scissor.width, gy2 = gy1 + dynState_.scissor.height;
					for (const StvSomSub &q : stvSubs) {
						const int x1 = std::max(q.x1, gx1), y1 = std::max(q.y1, gy1), x2 = std::min(q.x2, gx2), y2 = std::min(q.y2, gy2);
						if (x2 <= x1 || y2 <= y1) continue;
						renderManager->SetScissor(x1, y1, x2 - x1, y2 - y1);
						StvSombra::St().areaRecorte2 += (double)(x2 - x1) * (y2 - y1);
						renderManager->DrawIndexed(descSetIndex, ARRAY_SIZE(dynamicUBOOffsets), dynamicUBOOffsets, vbuf, vbOffset, ibuf, ibOffset + q.primero * sizeof(uint16_t), q.cuenta, 1);
					}
					gstate_c.Dirty(DIRTY_VIEWPORTSCISSOR_STATE);
				} else {
					renderManager->DrawIndexed(descSetIndex, ARRAY_SIZE(dynamicUBOOffsets), dynamicUBOOffsets, vbuf, vbOffset, ibuf, ibOffset, vertexCount, 1);
				}
			}
		} else {
			StvDescribirPrimerDraw(renderManager, (int)prim, vertexCount, true);
			if (!StvFsBits(stvUltimoFs, pipelineState_.blendState.blendEnabled) && !StvClasif(vertexCount, pipelineKey_, (int)prim) && !StvDrawInfo(vertexCount, pipelineKey_, (int)prim) && !stvSomSaltar)
			renderManager->Draw(descSetIndex, ARRAY_SIZE(dynamicUBOOffsets), dynamicUBOOffsets, vbuf, vbOffset, vertexCount);
		}
		if (useDepthRaster_) {
			DepthRasterSubmitRaw(prim, dec_, dec_->VertexType(), vertexCount);
		}
	} else {
		PROFILE_THIS_SCOPE("soft");
		const VertexDecoder *swDec = dec_;
		if (swDec->nweights != 0) {
			u32 withSkinning = lastVType_ | (1 << 26);
			if (withSkinning != lastVType_) {
				swDec = GetVertexDecoder(withSkinning);
			}
		}
		int prevDecodedVerts = numDecodedVerts_;

		DecodeVerts(swDec, decoded_);
		int vertexCount = DecodeInds();

		bool hasColor = (lastVType_ & GE_VTYPE_COL_MASK) != GE_VTYPE_COL_NONE;
		if (gstate.isModeThrough()) {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && (hasColor || gstate.getMaterialAmbientA() == 255);
		} else {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && ((hasColor && (gstate.materialupdate & 1)) || gstate.getMaterialAmbientA() == 255) && (!gstate.isLightingEnabled() || gstate.getAmbientA() == 255);
		}

		gpuStats.numUncachedVertsDrawn += vertexCount;
		prim = IndexGenerator::GeneralPrim((GEPrimitiveType)drawInds_[0].prim);

		// At this point, the output is always an index triangle/line/point list, no strips/fans.

		u16 *inds = decIndex_;
		SoftwareTransformResult result{};
		SoftwareTransformParams params{};
		params.decoded = decoded_;
		params.transformed = transformed_;
		params.transformedExpanded = transformedExpanded_;
		params.fbman = framebufferManager_;
		params.texCache = textureCache_;
		// In Vulkan, we have to force drawing of primitives if !framebufferManager_->UseBufferedRendering() because Vulkan clears
		// do not respect scissor rects.
		params.allowClear = framebufferManager_->UseBufferedRendering();
		params.allowSeparateAlphaClear = false;

		if (gstate.getShadeMode() == GE_SHADE_FLAT) {
			if (!renderManager->GetVulkanContext()->GetDeviceFeatures().enabled.provokingVertex.provokingVertexLast) {
				// If we can't have the hardware do it, we need to rotate the index buffer to simulate a different provoking vertex.
				// We do this before line expansion etc.
				IndexBufferProvokingLastToFirst(prim, inds, vertexCount);
			}
		}
		params.flippedY = true;
		params.usesHalfZ = true;

		// We need to update the viewport early because it's checked for flipping in SoftwareTransform.
		// We don't have a "DrawStateEarly" in vulkan, so...
		// TODO: Probably should eventually refactor this and feed the vp size into SoftwareTransform directly (Unknown's idea).
		if (gstate_c.IsDirty(DIRTY_VIEWPORTSCISSOR_STATE)) {
			ViewportAndScissor vpAndScissor;
			ConvertViewportAndScissor(
				framebufferManager_->GetDisplayLayoutConfigCopy(),
				framebufferManager_->UseBufferedRendering(),
				framebufferManager_->GetRenderWidth(), framebufferManager_->GetRenderHeight(),
				framebufferManager_->GetTargetBufferWidth(), framebufferManager_->GetTargetBufferHeight(),
				vpAndScissor);
			UpdateCachedViewportState(vpAndScissor);
		}

		// At this point, rect and line primitives are still preserved as such. So, it's the best time to do software depth raster.
		// We could piggyback on the viewport transform below, but it gets complicated since it's different per-backend. Which we really
		// should clean up one day...
		if (useDepthRaster_) {
			DepthRasterPredecoded(prim, decoded_, numDecodedVerts_, swDec, vertexCount);
		}

		SoftwareTransform swTransform(params);

		const Lin::Vec3 trans(gstate_c.vpXOffset, gstate_c.vpYOffset, gstate_c.vpZOffset * 0.5f + 0.5f);
		const Lin::Vec3 scale(gstate_c.vpWidthScale, gstate_c.vpHeightScale, gstate_c.vpDepthScale * 0.5f);
		swTransform.SetProjMatrix(gstate.projMatrix, gstate_c.vpWidth < 0, gstate_c.vpHeight < 0, trans, scale);

		swTransform.Transform(prim, swDec->VertexType(), swDec->GetDecVtxFmt(), numDecodedVerts_, &result);

		// Non-zero depth clears are unusual, but some drivers don't match drawn depth values to cleared values.
		// Games sometimes expect exact matches (see #12626, for example) for equal comparisons.
		if (result.action == SW_CLEAR && everUsedEqualDepth_ && gstate.isClearModeDepthMask() && result.depth > 0.0f && result.depth < 1.0f)
			result.action = SW_NOT_READY;

		if (result.action == SW_NOT_READY) {
			// decIndex_ here is always equal to inds currently, but it may not be in the future.
			swTransform.BuildDrawingParams(prim, vertexCount, swDec->VertexType(), inds, RemainingIndices(inds), numDecodedVerts_, VERTEX_BUFFER_MAX, &result);
		}
		StvSomThrough(result, result.action == SW_CLEAR, prim, inds, vertexCount);  // STV_SOMBRA_RECORTE_v1

		if (result.setSafeSize)
			framebufferManager_->SetSafeSize(result.safeWidth, result.safeHeight);

		// Only here, where we know whether to clear or to draw primitives, should we actually set the current framebuffer! Because that gives use the opportunity
		// to use a "pre-clear" render pass, for high efficiency on tilers.
		if (result.action == SW_DRAW_INDEXED) {
			if (textureNeedsApply) {
				gstate_c.pixelMapped = result.pixelMapped;
				gstate_c.dstSquared = false;
				textureCache_->ApplyTexture();
				gstate_c.pixelMapped = false;
				textureCache_->GetVulkanHandles(imageView, sampler);
				if (imageView == VK_NULL_HANDLE)
					imageView = (VkImageView)draw_->GetNativeObject(gstate_c.textureIsArray ? Draw::NativeObject::NULL_IMAGEVIEW_ARRAY : Draw::NativeObject::NULL_IMAGEVIEW);
				if (sampler == VK_NULL_HANDLE)
					sampler = nullSampler_;
				if (gstate_c.dstSquared) {
					gstate_c.Dirty(DIRTY_BLEND_STATE);
				}
			}
			if (!lastPipeline_ || gstate_c.IsDirty(DIRTY_BLEND_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_RASTER_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_GEOMETRYSHADER_STATE) || prim != lastPrim_) {
				if (prim != lastPrim_ || gstate_c.IsDirty(DIRTY_BLEND_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_RASTER_STATE | DIRTY_DEPTHSTENCIL_STATE)) {
					ConvertStateToVulkanKey(*framebufferManager_, shaderManager_, prim, pipelineKey_, dynState_);
				}

				VulkanVertexShader *vshader = nullptr;
				VulkanFragmentShader *fshader = nullptr;
				VulkanGeometryShader *gshader = nullptr;

				shaderManager_->GetShaders(prim, swDec->VertexType(), &vshader, &fshader, &gshader, pipelineState_, false, false, decOptions_.expandAllWeightsToFloat, true);
				if (fshader) stvUltimoFs = fshader->GetID();
			if (vshader) stvUltimoVs = vshader->GetID();
				_dbg_assert_msg_(!vshader->UseHWTransform(), "Bad vshader");
				VulkanPipeline *pipeline = pipelineManager_->GetOrCreatePipeline(renderManager, pipelineLayout_, pipelineKey_, &swDec->decFmt, vshader, fshader, gshader, false, 0, framebufferManager_->GetMSAALevel(), false);
				if (!pipeline || !pipeline->pipeline) {
					// Already logged, let's bail out.
					ResetAfterDraw();
					return;
				}
				BindShaderBlendTex();  // This might cause copies so super important to do before BindPipeline.

				if (!renderManager->BindPipeline(pipeline->pipeline, pipeline->pipelineFlags, pipelineLayout_)) {
					renderManager->ReportBadStateForDraw();
					ResetAfterDraw();
					return;
				}
				if (pipeline != lastPipeline_) {
					if (lastPipeline_ && !lastPipeline_->UsesBlendConstant() && pipeline->UsesBlendConstant()) {
						gstate_c.Dirty(DIRTY_BLEND_STATE);
					}
					lastPipeline_ = pipeline;
				}
				ApplyDrawStateLate(renderManager, result.setStencil, result.stencilValue, pipeline->UsesBlendConstant());
				gstate_c.Clean(DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_RASTER_STATE | DIRTY_VIEWPORTSCISSOR_STATE);
				gstate_c.Dirty(dirtyRequiresRecheck_);
				dirtyRequiresRecheck_ = 0;
				lastPipeline_ = pipeline;
			}

			lastPrim_ = prim;

			dirtyUniforms_ |= shaderManager_->UpdateUniforms(framebufferManager_->UseBufferedRendering());

			// Even if the first draw is through-mode, make sure we at least have one copy of these uniforms buffered
			UpdateUBOs();

			int descCount = 6;
			int descSetIndex;
			PackedDescriptor *descriptors = renderManager->PushDescriptorSet(descCount, &descSetIndex);
			StvTexSonda(imageView, sampler);  // STV_TEXSONDA_v1 (instrumento, apagado por defecto)
			descriptors[0].image.view = imageView;
			descriptors[0].image.sampler = sampler;
			descriptors[1].image.view = boundSecondary_;
			descriptors[1].image.sampler = samplerSecondaryNearest_;
			descriptors[2].image.view = boundDepal_;
			descriptors[2].image.sampler = (boundDepal_ && boundDepalSmoothed_) ? samplerSecondaryLinear_ : samplerSecondaryNearest_;
			descriptors[3].buffer.buffer = baseBuf;
			descriptors[3].buffer.range = sizeof(UB_VS_FS_Base);
			descriptors[3].buffer.offset = 0;
			descriptors[4].buffer.buffer = lightBuf;
			descriptors[4].buffer.range = sizeof(UB_VS_Lights);
			descriptors[4].buffer.offset = 0;
			descriptors[5].buffer.buffer = boneBuf;
			descriptors[5].buffer.range = sizeof(UB_VS_Bones);
			descriptors[5].buffer.offset = 0;

			const uint32_t dynamicUBOOffsets[3] = {
				baseUBOOffset, lightUBOOffset, boneUBOOffset,
			};

			PROFILE_THIS_SCOPE("renderman_q");

			VkBuffer vbuf, ibuf;
			u32 vbOffset = (uint32_t)pushVertex_->Push(result.drawBuffer, numDecodedVerts_ * sizeof(TransformedVertex), 4, &vbuf);
			u32 ibOffset = (uint32_t)pushIndex_->Push(inds, sizeof(short) * result.drawNumTrans, 4, &ibuf);
			// STV: describir el primer draw del paso y, si es opaco y cubre su caja,
			// avisar al runner (con la caja en pixeles del render target). DESPUES de
			// aplicar la textura: eso puede copiar y re-abrir el pase.
			StvDescribirPrimerDraw(renderManager, (int)prim, numDecodedVerts_, false);
			// STV (area): caja del draw en pixeles del render target, con el offset
			// del RT (framebuffers que arrancan dentro de otro) y 1 px de margen.
			int stvRx1 = 0, stvRy1 = 0, stvRx2 = 0, stvRy2 = 0;
			if (result.stvBboxValido) {
				// La escala sale del framebuffer virtual REAL al que se dibuja. La de
				// gstate_c (curRTRenderWidth/curRTWidth) quedo rancia en algun draw (el
				// relampago): caja de 480 px sin el 3x -> area cortada en 512 = costura.
				const VirtualFramebuffer *stvVfb = framebufferManager_->GetCurrentRenderVFB();
				float exG = gstate_c.curRTWidth ? (float)gstate_c.curRTRenderWidth / gstate_c.curRTWidth : 0.0f;
				float ex = (stvVfb && stvVfb->renderScaleFactor > 0.0f) ? stvVfb->renderScaleFactor : exG;
				float ey = ex;
				{	static int avisos = 0;
					if (avisos < 8 && stvVfb && fabsf(exG - ex) > 0.01f) { avisos++; STV_LOG("STVAREA escala rancia: gstate %.3f (rt %ux%u render %ux%u) vs vfb %.3f", exG, gstate_c.curRTWidth, gstate_c.curRTHeight, gstate_c.curRTRenderWidth, gstate_c.curRTRenderHeight, ex); }
				}
				const int offX = std::max(gstate_c.curRTOffsetX, 0), offY = std::max(gstate_c.curRTOffsetY, 0);   // como ConvertViewportAndScissor (renderX/renderY)
				if (ex > 0.0f) {
					stvRx1 = (int)floorf((result.stvBbox[0] + offX) * ex) - 1;
					stvRy1 = (int)floorf((result.stvBbox[1] + offY) * ey) - 1;
					stvRx2 = (int)ceilf((result.stvBbox[2] + offX) * ex) + 1;
					stvRy2 = (int)ceilf((result.stvBbox[3] + offY) * ey) + 1;
					renderManager->StvAcotarProximoDraw(std::max(0, stvRx1), std::max(0, stvRy1), stvRx2, stvRy2);
				}
			}
			{
				static int stvDc = -1;
				if (stvDc < 0) stvDc = StvPropInt("debug.stv.dcload");
				if (stvDc >= 1 && result.stvCubre && !gstate.isModeClear() && renderManager->StvPasoSinDraws()) {
					bool opaco = !gstate.isAlphaBlendEnabled()
						&& (!gstate.isAlphaTestEnabled() || gstate.getAlphaTestFunction() == GE_COMP_ALWAYS)
						&& !gstate.isColorTestEnabled()
						&& gstate.getColorMask() == 0
						&& (!gstate.isLogicOpEnabled() || gstate.getLogicOp() == GE_LOGIC_COPY)
						&& (!gstate.isDepthTestEnabled() || gstate.getDepthTestFunction() == GE_COMP_ALWAYS)
						&& (!gstate.isStencilTestEnabled() || gstate.getStencilTestFunction() == GE_COMP_ALWAYS);
					if (opaco) {
						renderManager->StvPrimerDrawOpaco(stvRx1 + 1, stvRy1 + 1, stvRx2 - 1, stvRy2 - 1);
					}
				}
			}
			stvDiTV = result.drawBuffer; stvDiTVn = numDecodedVerts_;  // STVDI3
			const bool stvSaltear = StvFsBits(stvUltimoFs, pipelineState_.blendState.blendEnabled) || StvClasif(result.drawNumTrans, pipelineKey_, (int)prim) || StvDrawInfo(result.drawNumTrans, pipelineKey_, (int)prim) || StvDrawNulo();  // STV_DRAW_NULO_v1
			stvDiTV = nullptr; stvDiTVn = 0;
			if (!stvSaltear)
			renderManager->DrawIndexed(descSetIndex, ARRAY_SIZE(dynamicUBOOffsets), dynamicUBOOffsets, vbuf, vbOffset, ibuf, ibOffset, result.drawNumTrans, 1);
		} else if (result.action == SW_CLEAR) {
			// Note: we won't get here if the clear is alpha but not color, or color but not alpha.
			bool clearColor = gstate.isClearModeColorMask();
			bool clearAlpha = gstate.isClearModeAlphaMask() || result.stvClearAlpha;  // and stencil (STV: clear por malla)
			bool clearDepth = gstate.isClearModeDepthMask();
			Draw::Aspect mask = Draw::Aspect::NO_BIT;
			// The Clear detection takes care of doing a regular draw instead if separate masking
			// of color and alpha is needed, so we can just treat them as the same.
			if (clearColor || clearAlpha) mask |= Draw::Aspect::COLOR_BIT;
			if (clearDepth) mask |= Draw::Aspect::DEPTH_BIT;
			if (clearAlpha) mask |= Draw::Aspect::STENCIL_BIT;
			// Note that since the alpha channel and the stencil channel are shared on the PSP,
			// when we clear alpha, we also clear stencil to the same value.
			draw_->Clear(mask, result.color, result.depth, result.color >> 24);
			if (gstate_c.Use(GPU_USE_CLEAR_RAM_HACK) && gstate.isClearModeColorMask() && (gstate.isClearModeAlphaMask() || gstate.FrameBufFormat() == GE_FORMAT_565)) {
				int scissorX1 = gstate.getScissorX1();
				int scissorY1 = gstate.getScissorY1();
				int scissorX2 = gstate.getScissorX2() + 1;
				int scissorY2 = gstate.getScissorY2() + 1;
				framebufferManager_->ApplyClearToMemory(scissorX1, scissorY1, scissorX2, scissorY2, result.color);
			}
		}
	}

	ResetAfterDrawInline();

	framebufferManager_->SetColorUpdated(gstate_c.skipDrawReason);

	gpuCommon_->NotifyFlush();
}

void DrawEngineVulkan::ResetAfterDraw() {
	indexGen.Reset();
	numDecodedVerts_ = 0;
	numDrawVerts_ = 0;
	numDrawInds_ = 0;
	vertexCountInDrawCalls_ = 0;
	decodeIndsCounter_ = 0;
	decodeVertsCounter_ = 0;
	gstate_c.vertexFullAlpha = true;
}

void DrawEngineVulkan::UpdateUBOs() {
	if ((dirtyUniforms_ & DIRTY_BASE_UNIFORMS) || baseBuf == VK_NULL_HANDLE) {
		baseUBOOffset = shaderManager_->PushBaseBuffer(pushUBO_, &baseBuf);
		dirtyUniforms_ &= ~DIRTY_BASE_UNIFORMS;
	}
	if ((dirtyUniforms_ & DIRTY_LIGHT_UNIFORMS) || lightBuf == VK_NULL_HANDLE) {
		lightUBOOffset = shaderManager_->PushLightBuffer(pushUBO_, &lightBuf);
		dirtyUniforms_ &= ~DIRTY_LIGHT_UNIFORMS;
	}
	if ((dirtyUniforms_ & DIRTY_BONE_UNIFORMS) || boneBuf == VK_NULL_HANDLE) {
		boneUBOOffset = shaderManager_->PushBoneBuffer(pushUBO_, &boneBuf);
		dirtyUniforms_ &= ~DIRTY_BONE_UNIFORMS;
	}
}

void TessellationDataTransferVulkan::SendDataToShader(const SimpleVertex *const *points, int size_u, int size_v, u32 vertType, const Spline::Weight2D &weights) {
	// SSBOs that are not simply float1 or float2 need to be padded up to a float4 size. vec3 members
	// also need to be 16-byte aligned, hence the padding.
	struct TessData {
		float pos[3]; float pad1;
		float uv[2]; float pad2[2];
		float color[4];
	};

	int size = size_u * size_v;

	int ssboAlignment = vulkan_->GetPhysicalDeviceProperties().properties.limits.minStorageBufferOffsetAlignment;
	uint8_t *data = (uint8_t *)push_->Allocate(size * sizeof(TessData), ssboAlignment, &bufInfo_[0].buffer, (uint32_t *)&bufInfo_[0].offset);
	bufInfo_[0].range = size * sizeof(TessData);

	float *pos = (float *)(data);
	float *tex = (float *)(data + offsetof(TessData, uv));
	float *col = (float *)(data + offsetof(TessData, color));
	int stride = sizeof(TessData) / sizeof(float);

	CopyControlPoints(pos, tex, col, stride, stride, stride, points, size, vertType);

	using Spline::Weight;

	// Weights U
	data = (uint8_t *)push_->Allocate(weights.size_u * sizeof(Weight), ssboAlignment, &bufInfo_[1].buffer, (uint32_t *)&bufInfo_[1].offset);
	memcpy(data, weights.u, weights.size_u * sizeof(Weight));
	bufInfo_[1].range = weights.size_u * sizeof(Weight);

	// Weights V
	data = (uint8_t *)push_->Allocate(weights.size_v * sizeof(Weight), ssboAlignment, &bufInfo_[2].buffer, (uint32_t *)&bufInfo_[2].offset);
	memcpy(data, weights.v, weights.size_v * sizeof(Weight));
	bufInfo_[2].range = weights.size_v * sizeof(Weight);
}

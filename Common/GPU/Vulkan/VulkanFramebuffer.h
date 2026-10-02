#pragma once

#include <algorithm>

#include "Common/Common.h"
#include "Common/GPU/Vulkan/VulkanContext.h"

class VKRRenderPass;
class VulkanBarrierBatch;

// Pipelines need to be created for the right type of render pass.
// TODO: Rename to RenderPassFlags?
// When you add more flags, don't forget to update rpTypeDebugNames[].
enum class RenderPassType {
	DEFAULT = 0,
	// These eight are organized so that bit 0 is DEPTH and bit 1 is INPUT and bit 2 is MULTIVIEW, so
	// they can be OR-ed together in MergeRPTypes.
	HAS_DEPTH = 1,
	MULTIVIEW = 2,
	MULTISAMPLE = 4,

	// This is the odd one out, and gets special handling in MergeRPTypes.
	// If this flag is set, none of the other flags can be set.
	// For the backbuffer we can always use CLEAR/DONT_CARE, so bandwidth cost for a depth channel is negligible
	// so we don't bother with a non-depth version.
	BACKBUFFER = 8,

	TYPE_COUNT = BACKBUFFER + 1,
};
ENUM_CLASS_BITOPS(RenderPassType);

// Simple independent framebuffer image.
struct VKRImage {
	// These four are "immutable".
	VkImage image;

	VkImageView rtView;  // Used for rendering to, and readbacks of stencil. 2D if single layer, 2D_ARRAY if multiple. Includes both depth and stencil if depth/stencil.

	// This is for texturing all layers at once. If aspect is depth/stencil, does not include stencil.
	VkImageView texAllLayersView;

	// If it's a layered image (for stereo), this is two 2D views of it, to make it compatible with shaders that don't yet support stereo.
	// If there's only one layer, layerViews[0] only is initialized.
	VkImageView texLayerViews[2]{};

	VmaAllocation alloc;
	VkFormat format;
	VkSampleCountFlagBits sampleCount;

	// This one is used by QueueRunner's Perform functions to keep track. CANNOT be used anywhere else due to sync issues.
	VkImageLayout layout;

	int numLayers;

	// For debugging.
	std::string tag;

	void Delete(VulkanContext *vulkan);
};

class VKRFramebuffer {
public:
	VKRFramebuffer(VulkanContext *vk, VulkanBarrierBatch *barriers, int _width, int _height, int _numLayers, int _multiSampleLevel, bool createDepthStencilBuffer, const char *tag);
	~VKRFramebuffer();

	VkFramebuffer Get(VKRRenderPass *compatibleRenderPass, RenderPassType rpType);

	int width = 0;
	int height = 0;
	int numLayers = 0;
	VkSampleCountFlagBits sampleCount;

	VKRImage color{};  // color.image is always there.
	VKRImage depth{};  // depth.image is allowed to be VK_NULL_HANDLE.

	// These are only initialized and used if numSamples > 1.
	VKRImage msaaColor{};
	VKRImage msaaDepth{};

	const char *Tag() const {
		return tag_.c_str();
	}

	void UpdateTag(const char *newTag);

	bool HasDepth() const {
		return depth.image != VK_NULL_HANDLE;
	}

	// STV_AUTOTEX_ALTERNA_v1: union de lo escrito en el color desde la ultima limpieza (pixeles).
	int stvSucio[4] = { 0, 0, 0, 0 };
	bool stvSucioHay = false;
	void StvMarcar(int x1, int y1, int x2, int y2) {
		StvMarcarAlias(x1, y1, x2, y2);  // STV_REINTERP_PARCIAL_v1
		x1 = std::max(x1, 0); y1 = std::max(y1, 0); x2 = std::min(x2, width); y2 = std::min(y2, height);
		if (x2 <= x1 || y2 <= y1) return;
		if (!stvSucioHay) { stvSucio[0] = x1; stvSucio[1] = y1; stvSucio[2] = x2; stvSucio[3] = y2; stvSucioHay = true; return; }
		stvSucio[0] = std::min(stvSucio[0], x1); stvSucio[1] = std::min(stvSucio[1], y1);
		stvSucio[2] = std::max(stvSucio[2], x2); stvSucio[3] = std::max(stvSucio[3], y2);
	}
	void StvMarcarTodo() { StvMarcar(0, 0, width, height); }
	// STV_REINTERP_PARCIAL_v1: otro rectangulo sucio, solo para el par de alias (misma memoria en otro
	// formato): lo escrito desde la ultima reinterpretacion con la pareja.
	int stvAlias[4] = { 0, 0, 0, 0 };
	bool stvAliasHay = false;
	VKRFramebuffer *stvAliasPar = nullptr;
	void StvMarcarAlias(int x1, int y1, int x2, int y2) {
		x1 = std::max(x1, 0); y1 = std::max(y1, 0); x2 = std::min(x2, width); y2 = std::min(y2, height);
		if (x2 <= x1 || y2 <= y1) return;
		if (!stvAliasHay) { stvAlias[0] = x1; stvAlias[1] = y1; stvAlias[2] = x2; stvAlias[3] = y2; stvAliasHay = true; return; }
		stvAlias[0] = std::min(stvAlias[0], x1); stvAlias[1] = std::min(stvAlias[1], y1);
		stvAlias[2] = std::max(stvAlias[2], x2); stvAlias[3] = std::max(stvAlias[3], y2);
	}

	VkImageView GetRTView() {
		if (sampleCount == VK_SAMPLE_COUNT_1_BIT) {
			return color.rtView;
		} else {
			return msaaColor.rtView;
		}
	}

	VulkanContext *Vulkan() const { return vulkan_; }
private:
	static void CreateImage(VulkanContext *vulkan, VulkanBarrierBatch *barriers, VKRImage &img, int width, int height, int numLayers, VkSampleCountFlagBits sampleCount, VkFormat format, VkImageLayout initialLayout, bool color, const char *tag);

	VkFramebuffer framebuf[(size_t)RenderPassType::TYPE_COUNT]{};

	VulkanContext *vulkan_;
	std::string tag_;
};

inline bool RenderPassTypeHasDepth(RenderPassType type) {
	return (type & RenderPassType::HAS_DEPTH) || type == RenderPassType::BACKBUFFER;
}

inline bool RenderPassTypeHasMultiView(RenderPassType type) {
	return (type & RenderPassType::MULTIVIEW) != 0;
}

inline bool RenderPassTypeHasMultisample(RenderPassType type) {
	return (type & RenderPassType::MULTISAMPLE) != 0;
}

VkSampleCountFlagBits MultiSampleLevelToFlagBits(int count);

// Must be the same order as Draw::RPAction
enum class VKRRenderPassLoadAction : uint8_t {
	KEEP,  // default. avoid when possible.
	CLEAR,
	DONT_CARE,
};

enum class VKRRenderPassStoreAction : uint8_t {
	STORE,  // default. avoid when possible.
	DONT_CARE,
};

struct RPKey {
	// Only render-pass-compatibility-volatile things can be here.
	VKRRenderPassLoadAction colorLoadAction;
	VKRRenderPassLoadAction depthLoadAction;
	VKRRenderPassLoadAction stencilLoadAction;
	VKRRenderPassStoreAction colorStoreAction;
	VKRRenderPassStoreAction depthStoreAction;
	VKRRenderPassStoreAction stencilStoreAction;
};

class VKRRenderPass {
public:
	explicit VKRRenderPass(const RPKey &key) : key_(key) {}

	VkRenderPass Get(VulkanContext *vulkan, RenderPassType rpType, VkSampleCountFlagBits sampleCount);
	void Destroy(VulkanContext *vulkan) {
		for (size_t i = 0; i < (size_t)RenderPassType::TYPE_COUNT; i++) {
			if (pass[i]) {
				vulkan->Delete().QueueDeleteRenderPass(pass[i]);
			}
		}
	}

private:
	// TODO: Might be better off with a hashmap once the render pass type count grows really large..
	VkRenderPass pass[(size_t)RenderPassType::TYPE_COUNT]{};
	VkSampleCountFlagBits sampleCounts[(size_t)RenderPassType::TYPE_COUNT]{};
	RPKey key_;
};

const char *GetRPTypeName(RenderPassType rpType);

/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0
*/

#include "chain.h"
#include "shaders/passthrough_spv.h"

#include <cstring>

namespace shaderglass {

namespace {

// The unit quad, in the layout libretro passes expect: a vec4 position and a vec2 texture
// coordinate, six floats a vertex, drawn as a triangle strip.
//
// Positions run 0..1 and the MVP maps them into clip space. Vulkan's NDC has y pointing down and the
// texture origin is top-left, so position y and texture v agree and neither needs flipping -- the
// vertical flip that catches ports of GL code out does not arise here.
struct Vertex {
    float x, y, z, w;
    float u, v;
};

const Vertex kQuad[4] = {
    {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f},  // top-left
    {1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},  // top-right
    {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f},  // bottom-left
    {1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f},  // bottom-right
};

// Column-major, mapping the unit quad to clip space: (0,0) -> (-1,-1), (1,1) -> (1,1).
const float kMvp[16] = {
    2.0f, 0.0f, 0.0f, 0.0f,   //
    0.0f, 2.0f, 0.0f, 0.0f,   //
    0.0f, 0.0f, 1.0f, 0.0f,   //
    -1.0f, -1.0f, 0.0f, 1.0f  //
};

// Bytes a pixel, for the formats ChainFormat can produce.
uint32_t FormatBytes(VkFormat f) {
    return f == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : 4u;
}

bool SelfTestWanted() {
    static const bool v = [] {
        const char* p = getenv("SHADERGLASS_SELFTEST");
        return p && p[0] == '1';
    }();
    return v;
}

void FillSize(float out[4], uint32_t w, uint32_t h) {
    out[0] = float(w);
    out[1] = float(h);
    out[2] = w ? 1.0f / float(w) : 0.0f;
    out[3] = h ? 1.0f / float(h) : 0.0f;
}

VkShaderModule MakeModule(const DeviceTable* vk, VkDevice device, const uint32_t* code,
                          size_t words) {
    VkShaderModuleCreateInfo ci {};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = words * sizeof(uint32_t);
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vk->vkCreateShaderModule(device, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

}  // namespace

Chain::Chain(const DeviceTable* vk, const InstanceTable* instance, VkDevice device,
             VkPhysicalDevice physical)
    : _vk(vk), _instance(instance), _device(device), _physical(physical) {
    if (!DeviceTableComplete(*vk)) {
        _reason = "the device does not expose everything a shader pass needs";
        Log("[chain] %s", _reason.c_str());
        return;
    }
    _usable = true;
}

Chain::~Chain() {
    DropSized();
    DropStatic();
}

void Chain::DropStatic() {
    if (_pipeline) _vk->vkDestroyPipeline(_device, _pipeline, nullptr);
    if (_pipelineLayout) _vk->vkDestroyPipelineLayout(_device, _pipelineLayout, nullptr);
    if (_setLayout) _vk->vkDestroyDescriptorSetLayout(_device, _setLayout, nullptr);
    if (_descriptorPool) _vk->vkDestroyDescriptorPool(_device, _descriptorPool, nullptr);
    if (_renderPass) _vk->vkDestroyRenderPass(_device, _renderPass, nullptr);
    if (_sampler) _vk->vkDestroySampler(_device, _sampler, nullptr);
    DropHostBuffer(_vk, _device, _vertices);
    DropHostBuffer(_vk, _device, _uniforms);

    _pipeline = VK_NULL_HANDLE;
    _pipelineLayout = VK_NULL_HANDLE;
    _setLayout = VK_NULL_HANDLE;
    _descriptorPool = VK_NULL_HANDLE;
    _renderPass = VK_NULL_HANDLE;
    _sampler = VK_NULL_HANDLE;
    _staticBuilt = false;
}

void Chain::DropSized() {
    if (_framebuffer) _vk->vkDestroyFramebuffer(_device, _framebuffer, nullptr);
    _framebuffer = VK_NULL_HANDLE;
    DropHostBuffer(_vk, _device, _selfTest);
    _selfTestHalf = 0;
    _selfTestRecorded = false;
    DropImage(_vk, _device, _frame);
    DropImage(_vk, _device, _source);
    DropImage(_vk, _device, _output);
    _swapWidth = _swapHeight = 0;
    _sourceWidth = _sourceHeight = 0;
}

// Everything that does not depend on the raster: the render pass, the pipeline, the sampler, the
// quad, the uniform ring and the descriptor sets. Built once for a given chain format.
bool Chain::BuildStatic() {
    if (_staticBuilt) return true;

    // The render pass writes the pass output and nothing else. Its contents are fully overwritten by
    // the draw, so there is nothing worth loading; it ends in TRANSFER_SRC because phase 2's single
    // pass goes straight back to the swapchain. Phase 4 varies the final layout per pass, since a
    // pass that feeds another ends in SHADER_READ_ONLY instead.
    VkAttachmentDescription colour {};
    colour.format = _chainFormat;
    colour.samples = VK_SAMPLE_COUNT_1_BIT;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colour.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colour.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference colourRef {};
    colourRef.attachment = 0;
    colourRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colourRef;

    // The sampled read of the source must complete before the attachment is written, and the copy
    // out must wait for the write. Stated rather than left to an implicit dependency, which would
    // only cover the first and only from TOP_OF_PIPE.
    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo rpci {};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &colour;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 2;
    rpci.pDependencies = deps;
    if (_vk->vkCreateRenderPass(_device, &rpci, nullptr, &_renderPass) != VK_SUCCESS) {
        _reason = "could not create the render pass";
        return false;
    }

    // Nearest, so a passthrough at matching rasters is bit-exact and the whole path can be proved
    // before a real shader is involved. Phase 4 takes the filter from each pass's own declaration.
    VkSamplerCreateInfo sci {};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    if (_vk->vkCreateSampler(_device, &sci, nullptr, &_sampler) != VK_SUCCESS) {
        _reason = "could not create the sampler";
        return false;
    }

    // binding 0 is the uniform block, binding 1 the source texture -- the layout the built-in
    // shaders declare. Phase 4 takes both from ShaderGC's reflection instead.
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dlci {};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 2;
    dlci.pBindings = bindings;
    if (_vk->vkCreateDescriptorSetLayout(_device, &dlci, nullptr, &_setLayout) != VK_SUCCESS) {
        _reason = "could not create the descriptor set layout";
        return false;
    }

    VkPipelineLayoutCreateInfo plci {};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &_setLayout;
    if (_vk->vkCreatePipelineLayout(_device, &plci, nullptr, &_pipelineLayout) != VK_SUCCESS) {
        _reason = "could not create the pipeline layout";
        return false;
    }

    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kRing};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kRing};

    VkDescriptorPoolCreateInfo dpci {};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = kRing;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = poolSizes;
    if (_vk->vkCreateDescriptorPool(_device, &dpci, nullptr, &_descriptorPool) != VK_SUCCESS) {
        _reason = "could not create the descriptor pool";
        return false;
    }

    VkDescriptorSetLayout layouts[kRing];
    for (uint32_t i = 0; i < kRing; ++i) layouts[i] = _setLayout;

    VkDescriptorSetAllocateInfo dsai {};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = _descriptorPool;
    dsai.descriptorSetCount = kRing;
    dsai.pSetLayouts = layouts;
    if (_vk->vkAllocateDescriptorSets(_device, &dsai, _sets) != VK_SUCCESS) {
        _reason = "could not allocate descriptor sets";
        return false;
    }

    if (!MakeHostBuffer(_vk, _instance, _device, _physical, _vertices, sizeof(kQuad),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
        _reason = "could not allocate the vertex buffer";
        return false;
    }
    std::memcpy(_vertices.mapped, kQuad, sizeof(kQuad));

    // A uniform buffer binding can be offset into, but only to a multiple of the device's own
    // alignment, so the ring's stride is the struct rounded up rather than the struct itself.
    VkPhysicalDeviceProperties props {};
    _instance->vkGetPhysicalDeviceProperties(_physical, &props);
    const VkDeviceSize align = props.limits.minUniformBufferOffsetAlignment
                                   ? props.limits.minUniformBufferOffsetAlignment
                                   : 1;
    _uboStride = ((sizeof(Ubo) + align - 1) / align) * align;

    if (!MakeHostBuffer(_vk, _instance, _device, _physical, _uniforms, _uboStride * kRing,
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
        _reason = "could not allocate the uniform ring";
        return false;
    }

    VkShaderModule vert =
        MakeModule(_vk, _device, kPassthroughVertSpv, kPassthroughVertSpvLen);
    VkShaderModule frag =
        MakeModule(_vk, _device, kPassthroughFragSpv, kPassthroughFragSpvLen);
    if (!vert || !frag) {
        if (vert) _vk->vkDestroyShaderModule(_device, vert, nullptr);
        if (frag) _vk->vkDestroyShaderModule(_device, frag, nullptr);
        _reason = "could not create the built-in shader modules";
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vb {};
    vb.binding = 0;
    vb.stride = sizeof(Vertex);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2] = {};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[0].offset = offsetof(Vertex, x);
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = offsetof(Vertex, u);

    VkPipelineVertexInputStateCreateInfo vi {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia {};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    // Viewport and scissor are dynamic so a resize does not rebuild the pipeline -- only the images
    // and the framebuffer follow the raster.
    VkPipelineViewportStateCreateInfo vp {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    const VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn {};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamics;

    VkPipelineRasterizationStateCreateInfo rs {};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms {};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend {};
    blend.blendEnable = VK_FALSE;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb {};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    VkGraphicsPipelineCreateInfo gp {};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = _pipelineLayout;
    gp.renderPass = _renderPass;
    gp.subpass = 0;

    const VkResult made =
        _vk->vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &gp, nullptr, &_pipeline);
    _vk->vkDestroyShaderModule(_device, vert, nullptr);
    _vk->vkDestroyShaderModule(_device, frag, nullptr);

    if (made != VK_SUCCESS) {
        _reason = "could not create the graphics pipeline";
        Log("[chain] vkCreateGraphicsPipelines -> %d", (int) made);
        return false;
    }

    _staticBuilt = true;
    return true;
}

bool Chain::BuildSized() {
    const VkImageUsageFlags sampled = VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags src = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkImageUsageFlags dst = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // The frame is copied from the swapchain and then sampled, or blitted down to the source raster.
    if (!MakeImage(_vk, _instance, _device, _physical, _frame, _swapWidth, _swapHeight, _chainFormat,
                   sampled | dst | src))
        return false;

    // Only when the shader is shown something other than the frame itself.
    if (_sourceWidth != _swapWidth || _sourceHeight != _swapHeight) {
        if (!MakeImage(_vk, _instance, _device, _physical, _source, _sourceWidth, _sourceHeight,
                       _chainFormat, sampled | dst))
            return false;
    }

    if (!MakeImage(_vk, _instance, _device, _physical, _output, _swapWidth, _swapHeight,
                   _chainFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | src | sampled))
        return false;

    if (SelfTestWanted()) {
        _selfTestHalf = VkDeviceSize(_swapWidth) * _swapHeight * FormatBytes(_chainFormat);
        if (!MakeHostBuffer(_vk, _instance, _device, _physical, _selfTest, _selfTestHalf * 2,
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            Log("[chain] self-test buffer could not be allocated; continuing without it");
            _selfTestHalf = 0;
        }
        _selfTestReported = false;
    }

    VkFramebufferCreateInfo fci {};
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass = _renderPass;
    fci.attachmentCount = 1;
    fci.pAttachments = &_output.view;
    fci.width = _swapWidth;
    fci.height = _swapHeight;
    fci.layers = 1;
    if (_vk->vkCreateFramebuffer(_device, &fci, nullptr, &_framebuffer) != VK_SUCCESS) {
        _reason = "could not create the framebuffer";
        return false;
    }

    return true;
}

bool Chain::Prepare(uint32_t swapWidth, uint32_t swapHeight, VkFormat swapFormat,
                    uint32_t sourceWidth, uint32_t sourceHeight) {
    if (!_usable) return false;

    if (_swapWidth == swapWidth && _swapHeight == swapHeight && _swapFormat == swapFormat &&
        _sourceWidth == sourceWidth && _sourceHeight == sourceHeight && _frame.image)
        return true;

    // Every internal surface uses the swapchain format's UNORM twin: sampling an _SRGB view would
    // decode to linear on the way in and re-encode on the way out, and the chain wants exactly the
    // numbers the game wrote.
    VkFormat chainFormat = swapFormat;
    switch (swapFormat) {
        case VK_FORMAT_B8G8R8A8_SRGB: chainFormat = VK_FORMAT_B8G8R8A8_UNORM; break;
        case VK_FORMAT_R8G8B8A8_SRGB: chainFormat = VK_FORMAT_R8G8B8A8_UNORM; break;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: chainFormat = VK_FORMAT_A8B8G8R8_UNORM_PACK32; break;
        default: break;
    }

    // The pass writes through a colour attachment and reads through a sampler, so the format has to
    // support both. A device that cannot do it leaves the game's frame alone rather than failing.
    const VkFormatFeatureFlags need =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (!FormatSupports(_instance, _physical, chainFormat, need)) {
        _reason = "this device cannot render to the swapchain's format";
        Log("[chain] %s (format %d)", _reason.c_str(), (int) chainFormat);
        _usable = false;
        return false;
    }

    // A format change means the render pass and pipeline go too; a raster change does not.
    if (_chainFormat != chainFormat) {
        DropSized();
        DropStatic();
        _chainFormat = chainFormat;
    } else {
        DropSized();
    }

    _swapWidth = swapWidth;
    _swapHeight = swapHeight;
    _swapFormat = swapFormat;
    _sourceWidth = sourceWidth;
    _sourceHeight = sourceHeight;

    if (!BuildStatic() || !BuildSized()) {
        Log("[chain] %s", _reason.empty() ? "could not build the chain" : _reason.c_str());
        DropSized();
        DropStatic();
        _usable = false;
        return false;
    }

    Log("[chain] built %ux%u, source %ux%u, format %d, 1 pass", _swapWidth, _swapHeight,
        _sourceWidth, _sourceHeight, (int) _chainFormat);
    _reason.clear();
    return true;
}

bool Chain::Record(VkCommandBuffer cb, VkImage swapchainImage, uint64_t frameCount) {
    if (!_usable || !_frame.image || !_pipeline) return false;

    const uint32_t slot = _slot;
    _slot = (_slot + 1) % kRing;

    // ---- the frame, as the game presented it ----
    TransitionForeign(_vk, cb, swapchainImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(_vk, cb, _frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy copy {};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {_swapWidth, _swapHeight, 1};
    _vk->vkCmdCopyImage(cb, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _frame.image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    // Straight back, so every path out of here -- including the ones that give up -- leaves the
    // image in the layout the presentation engine requires.
    TransitionForeign(_vk, cb, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // ---- the source raster ----
    Image* sampled = &_frame;
    if (_source.image) {
        // A linear blit, which is a placeholder: it is a bilinear reduction and aliases on a large
        // ratio. The real thing is an area average, and it arrives with the rest of the source-
        // resolution work in phase 6.
        Transition(_vk, cb, _frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(_vk, cb, _source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkImageBlit blit {};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {(int32_t) _swapWidth, (int32_t) _swapHeight, 1};
        blit.dstOffsets[1] = {(int32_t) _sourceWidth, (int32_t) _sourceHeight, 1};
        _vk->vkCmdBlitImage(cb, _frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _source.image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        sampled = &_source;
    }
    Transition(_vk, cb, *sampled, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // ---- the uniform block ----
    Ubo ubo {};
    std::memcpy(ubo.mvp, kMvp, sizeof(kMvp));
    FillSize(ubo.sourceSize, sampled->width, sampled->height);
    FillSize(ubo.originalSize, _swapWidth, _swapHeight);
    FillSize(ubo.outputSize, _swapWidth, _swapHeight);
    ubo.frameCount = (uint32_t) frameCount;
    std::memcpy((char*) _uniforms.mapped + _uboStride * slot, &ubo, sizeof(ubo));

    VkDescriptorBufferInfo bufferInfo {_uniforms.buffer, _uboStride * slot, sizeof(Ubo)};
    VkDescriptorImageInfo imageInfo {_sampler, sampled->view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = _sets[slot];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &bufferInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = _sets[slot];
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &imageInfo;
    _vk->vkUpdateDescriptorSets(_device, 2, writes, 0, nullptr);

    // ---- the pass ----
    // The attachment's initial layout is UNDEFINED, so whatever the output image held is discarded
    // rather than transitioned; the draw covers every pixel.
    _output.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkRenderPassBeginInfo rpbi {};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = _renderPass;
    rpbi.framebuffer = _framebuffer;
    rpbi.renderArea.extent = {_swapWidth, _swapHeight};
    _vk->vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport {};
    viewport.width = float(_swapWidth);
    viewport.height = float(_swapHeight);
    viewport.maxDepth = 1.0f;
    _vk->vkCmdSetViewport(cb, 0, 1, &viewport);

    VkRect2D scissor {};
    scissor.extent = {_swapWidth, _swapHeight};
    _vk->vkCmdSetScissor(cb, 0, 1, &scissor);

    _vk->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline);
    _vk->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipelineLayout, 0, 1,
                                 &_sets[slot], 0, nullptr);

    const VkDeviceSize offset = 0;
    _vk->vkCmdBindVertexBuffers(cb, 0, 1, &_vertices.buffer, &offset);
    _vk->vkCmdDraw(cb, 4, 1, 0, 0);

    _vk->vkCmdEndRenderPass(cb);

    // The render pass declared this as the attachment's final layout.
    _output.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    // ---- the self-test readback ----
    // Both copies are taken from what the pass actually consumed and produced, before anything else
    // touches either, so a mismatch can only come from the chain itself.
    _selfTestRecorded = false;
    if (_selfTestHalf && _selfTest.buffer && !_selfTestReported) {
        Transition(_vk, cb, _frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        VkBufferImageCopy r {};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {_swapWidth, _swapHeight, 1};
        _vk->vkCmdCopyImageToBuffer(cb, _frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    _selfTest.buffer, 1, &r);
        r.bufferOffset = _selfTestHalf;
        _vk->vkCmdCopyImageToBuffer(cb, _output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    _selfTest.buffer, 1, &r);
        _selfTestRecorded = true;
    }

    // ---- back into the swapchain ----
    TransitionForeign(_vk, cb, swapchainImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    _vk->vkCmdCopyImage(cb, _output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchainImage,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    TransitionForeign(_vk, cb, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    return true;
}

void Chain::ConsumeSelfTest() {
    if (!_selfTestRecorded || _selfTestReported || !_selfTest.mapped) return;
    _selfTestRecorded = false;
    _selfTestReported = true;

    const auto* before = (const uint8_t*) _selfTest.mapped;
    const auto* after = before + _selfTestHalf;

    if (_source.image) {
        // The shader was shown a reduced raster, so the result is not meant to match the frame.
        Log("[selftest] source %ux%u differs from the frame %ux%u; exactness is not expected here",
            _sourceWidth, _sourceHeight, _swapWidth, _swapHeight);
        return;
    }

    size_t differing = 0;
    size_t firstAt = 0;
    for (size_t i = 0; i < (size_t) _selfTestHalf; ++i) {
        if (before[i] != after[i]) {
            if (!differing) firstAt = i;
            ++differing;
        }
    }

    if (!differing) {
        Log("[selftest] PASS: the passthrough reproduced the frame exactly (%ux%u, %llu bytes)",
            _swapWidth, _swapHeight, (unsigned long long) _selfTestHalf);
    } else {
        Log("[selftest] FAIL: %llu of %llu bytes differ, first at %llu (%ux%u)",
            (unsigned long long) differing, (unsigned long long) _selfTestHalf,
            (unsigned long long) firstAt, _swapWidth, _swapHeight);
    }
}

}  // namespace shaderglass

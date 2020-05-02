#include "Core\TriangleEngine.hpp"
#include "Render\Vulkan\Debug.hpp"
#include "Render\Vulkan\Device.hpp"
#include "Core\EngineConfig.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <stddef.h>
#include "Render\Vulkan\SwapChain.hpp"
#include "entt\entity\registry.hpp"
#include "ECS\Components\CameraComponent.hpp"
#include "ECS\Systems\CameraSystem.hpp"
#include <memory>
#include "Render\Vulkan\ImGuiImpl.hpp"
#include "imgui\imgui.h"
#include "ECS\Systems\LightSystem.hpp"
#include "Render\Vulkan\Model.hpp"
#include "Render\Vulkan\Initializers.hpp"
#include "Render\Vulkan\Texture2D.hpp"

using namespace CG;

constexpr float DEFAULT_FOV = 60.0f;

CG::TriangleEngine::TriangleEngine(CG::EngineConfig& engineConfig)
    : CG::Engine(engineConfig)
{ }

CG::TriangleEngine::~TriangleEngine() = default;

void CG::TriangleEngine::RenderFrame(float deltaTime)
{
	PrepareFrame();

	imGui->UpdateUI(deltaTime);

	BuildCommandBuffers();

	// Pipeline stage at which the queue submission will wait (via pWaitSemaphores)
	VkPipelineStageFlags waitStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

	VkSubmitInfo localSubmitInfo = {};
	localSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	localSubmitInfo.pWaitDstStageMask = &waitStageMask;
	localSubmitInfo.pWaitSemaphores = &semaphores.presentComplete;
	localSubmitInfo.waitSemaphoreCount = 1;
	localSubmitInfo.pSignalSemaphores = &semaphores.renderComplete;
	localSubmitInfo.signalSemaphoreCount = 1;
	localSubmitInfo.pCommandBuffers = &drawCmdBuffers[currentBuffer];
	localSubmitInfo.commandBufferCount = 1;

	VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &localSubmitInfo, VK_NULL_HANDLE));

	SubmitFrame();
}

void CG::TriangleEngine::Prepare()
{
    Engine::Prepare();
	LoadModel();
	InitRayTracing();
	SetupSystems();

	PrepareUniformBuffers();
	SetupDescriptors();

	PreparePipelines();
	BuildCommandBuffers();
}

void CG::TriangleEngine::Cleanup()
{
	testModel = nullptr;
	imGui = nullptr;

	Engine::Cleanup();
}

void CG::TriangleEngine::FlushCommandBuffer(VkCommandBuffer commandBuffer)
{
	assert(commandBuffer != VK_NULL_HANDLE);

	vkDevice->FlushCommandBuffer(commandBuffer, queue, true);
}

void CG::TriangleEngine::PreparePipelines()
{
	VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCreateInfo = Vk::Initializers::PipelineInputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
	VkPipelineRasterizationStateCreateInfo rasterizationStateCreateInfo = Vk::Initializers::PipelineRasterizationStateCreateInfo(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0);
	VkPipelineColorBlendAttachmentState blendAttachmentStateCreateInfo = Vk::Initializers::PipelineColorBlendAttachmentState(0xf, VK_FALSE);
	VkPipelineColorBlendStateCreateInfo colorBlendStateCreateInfo = Vk::Initializers::PipelineColorBlendStateCreateInfo(1, &blendAttachmentStateCreateInfo);
	VkPipelineDepthStencilStateCreateInfo depthStencilStateCreateInfo = Vk::Initializers::PipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS_OR_EQUAL);
	VkPipelineViewportStateCreateInfo viewportStateCreateInfo = Vk::Initializers::PipelineViewportStateCreateInfo(1, 1, 0);
	VkPipelineMultisampleStateCreateInfo multisampleStateCreateInfo = Vk::Initializers::PipelineMultisampleStateCreateInfo(VK_SAMPLE_COUNT_1_BIT, 0);
	const std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicStateCI = Vk::Initializers::PipelineDynamicStateCreateInfo(dynamicStateEnables, 0);

	const std::vector<VkVertexInputBindingDescription> vertexInputBindings = {
		Vk::Initializers::VertexInputBindingDescription(0, sizeof(Vk::GLTFModel::Vertex), VK_VERTEX_INPUT_RATE_VERTEX),
	};
	const std::vector<VkVertexInputAttributeDescription> vertexInputAttributes = {
		Vk::Initializers::VertexInputAttributeDescription(0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vk::GLTFModel::Vertex, pos)),	// Location 0: Position	
		Vk::Initializers::VertexInputAttributeDescription(0, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vk::GLTFModel::Vertex, normal)),// Location 1: Normal
		Vk::Initializers::VertexInputAttributeDescription(0, 2, VK_FORMAT_R32G32_SFLOAT, offsetof(Vk::GLTFModel::Vertex, uv)),	// Location 2: Texture coordinates
		Vk::Initializers::VertexInputAttributeDescription(0, 3, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vk::GLTFModel::Vertex, color)),	// Location 3: Color
	};

	VkPipelineVertexInputStateCreateInfo vertexInputStateCreateInfo = Vk::Initializers::PipelineVertexInputStateCreateInfo();
	vertexInputStateCreateInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexInputBindings.size());
	vertexInputStateCreateInfo.pVertexBindingDescriptions = vertexInputBindings.data();
	vertexInputStateCreateInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexInputAttributes.size());
	vertexInputStateCreateInfo.pVertexAttributeDescriptions = vertexInputAttributes.data();

	const std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {
		LoadShader(GetAssetPath() + "shaders/compiled/mesh.vert.spv", VK_SHADER_STAGE_VERTEX_BIT),
		LoadShader(GetAssetPath() + "shaders/compiled/mesh.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT)
	};

	VkGraphicsPipelineCreateInfo pipelineCreateInfo = Vk::Initializers::PipelineCreateInfo(pipelineLayout, renderPass, 0);
	pipelineCreateInfo.pVertexInputState = &vertexInputStateCreateInfo;
	pipelineCreateInfo.pInputAssemblyState = &inputAssemblyStateCreateInfo;
	pipelineCreateInfo.pRasterizationState = &rasterizationStateCreateInfo;
	pipelineCreateInfo.pColorBlendState = &colorBlendStateCreateInfo;
	pipelineCreateInfo.pMultisampleState = &multisampleStateCreateInfo;
	pipelineCreateInfo.pViewportState = &viewportStateCreateInfo;
	pipelineCreateInfo.pDepthStencilState = &depthStencilStateCreateInfo;
	pipelineCreateInfo.pDynamicState = &dynamicStateCI;
	pipelineCreateInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
	pipelineCreateInfo.pStages = shaderStages.data();

	VK_CHECK_RESULT(vkCreateGraphicsPipelines(vkDevice->logicalDevice, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipelines.solid));

	// Wire frame rendering pipeline
	if (vkDevice->features.fillModeNonSolid) {
		rasterizationStateCreateInfo.polygonMode = VK_POLYGON_MODE_LINE;
		rasterizationStateCreateInfo.lineWidth = 1.0f;
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(vkDevice->logicalDevice, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipelines.wireframe));
	}
}

void CG::TriangleEngine::BuildCommandBuffers()
{
	VkCommandBufferBeginInfo cmdBufInfo = {};
	cmdBufInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	cmdBufInfo.pNext = nullptr;

	VkClearValue clearValues[2];
	clearValues[0].color = { uiData.bgColor.r, uiData.bgColor.g, uiData.bgColor.b, uiData.bgColor.a };
	clearValues[1].depthStencil = { 1.0f, 0 };

	VkRenderPassBeginInfo renderPassBeginInfo = {};
	renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassBeginInfo.pNext = nullptr;
	renderPassBeginInfo.renderPass = renderPass;
	renderPassBeginInfo.renderArea.offset.x = 0;
	renderPassBeginInfo.renderArea.offset.y = 0;
	renderPassBeginInfo.renderArea.extent.width = engineConfig.width;
	renderPassBeginInfo.renderArea.extent.height = engineConfig.height;
	renderPassBeginInfo.clearValueCount = 2;
	renderPassBeginInfo.pClearValues = clearValues;

	BuildUiCommandBuffers();

	for (int32_t i = 0; i < drawCmdBuffers.size(); ++i)
	{
		renderPassBeginInfo.framebuffer = frameBuffers[i];

		VK_CHECK_RESULT(vkBeginCommandBuffer(drawCmdBuffers[i], &cmdBufInfo));
		vkCmdBeginRenderPass(drawCmdBuffers[i], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

		VkViewport viewport = {};
		viewport.height = static_cast<float>(engineConfig.height);
		viewport.width = static_cast<float>(engineConfig.width);
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);

		VkRect2D scissor = {};
		scissor.extent.width = engineConfig.width;
		scissor.extent.height = engineConfig.height;
		scissor.offset.x = 0;
		scissor.offset.y = 0;
		vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);

		vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
		vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

		// VkDeviceSize offsets[1] = { 0 };
		// vkCmdBindVertexBuffers(drawCmdBuffers[i], 0, 1, &vertices.buffer, offsets);
		// vkCmdBindIndexBuffer(drawCmdBuffers[i], indices.buffer, 0, VK_INDEX_TYPE_UINT32);
		// vkCmdDrawIndexed(drawCmdBuffers[i], indices.count, 1, 0, 0, 1);

		imGui->DrawFrame(drawCmdBuffers[i]);

		vkCmdEndRenderPass(drawCmdBuffers[i]);

		VK_CHECK_RESULT(vkEndCommandBuffer(drawCmdBuffers[i]));
	}
}

void CG::TriangleEngine::SetupDescriptors()
{
	const std::vector<VkDescriptorPoolSize> poolSizes = 
	{
		Vk::Initializers::DescriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1),
		// One combined image sampler per model image/texture
		Vk::Initializers::DescriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 
			static_cast<uint32_t>(testModel->GetImages().size())),
	};

	// One set for matrices and one per model image/texture
	const uint32_t maxSetCount = static_cast<uint32_t>(testModel->GetImages().size()) + 1;

	VkDescriptorPoolCreateInfo descriptorPoolInfo = Vk::Initializers::DescriptorPoolCreateInfo(poolSizes, maxSetCount);
	VK_CHECK_RESULT(vkCreateDescriptorPool(vkDevice->logicalDevice, &descriptorPoolInfo, nullptr, &descriptorPool));

	const std::vector<VkDescriptorSetLayoutBinding> matricesSetLayoutBindings =
	{
		Vk::Initializers::DescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 0),
	};
	VkDescriptorSetLayoutCreateInfo matricesDescriptorSetLayoutCreateInfo = Vk::Initializers::DescriptorSetLayoutCreateInfo(matricesSetLayoutBindings);
	VK_CHECK_RESULT(vkCreateDescriptorSetLayout(vkDevice->logicalDevice, &matricesDescriptorSetLayoutCreateInfo, nullptr, &descriptorSetLayouts.matrices));

	const std::vector<VkDescriptorSetLayoutBinding> texturesSetLayoutBindings =
	{
		Vk::Initializers::DescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 0),
	};
	VkDescriptorSetLayoutCreateInfo texturesDescriptorSetLayoutCreateInfo = Vk::Initializers::DescriptorSetLayoutCreateInfo(texturesSetLayoutBindings);
	VK_CHECK_RESULT(vkCreateDescriptorSetLayout(vkDevice->logicalDevice, &texturesDescriptorSetLayoutCreateInfo, nullptr, &descriptorSetLayouts.textures));

	const std::vector<VkDescriptorSetLayout> setLayouts = { descriptorSetLayouts.matrices, descriptorSetLayouts.textures };
	VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = Vk::Initializers::PipelineLayoutCreateInfo(setLayouts);

	// For pushing primitive local matrices
	VkPushConstantRange pushConstantRange = Vk::Initializers::PushConstantRange(VK_SHADER_STAGE_VERTEX_BIT, sizeof(glm::mat4), 0);

	pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
	pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;
	VK_CHECK_RESULT(vkCreatePipelineLayout(vkDevice->logicalDevice, &pipelineLayoutCreateInfo, nullptr, &pipelineLayout));

	const VkDescriptorSetAllocateInfo matricesAllocInfo = Vk::Initializers::DescriptorSetAllocateInfo(descriptorPool, { descriptorSetLayouts.matrices });
	VK_CHECK_RESULT(vkAllocateDescriptorSets(vkDevice->logicalDevice, &matricesAllocInfo, &descriptorSet));
	VkWriteDescriptorSet matricesWriteDescriptorSet = Vk::Initializers::WriteDescriptorSet(descriptorSet, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &ubo.descriptor);
	vkUpdateDescriptorSets(vkDevice->logicalDevice, 1, &matricesWriteDescriptorSet, 0, nullptr);

	for (auto& image : testModel->GetImages())
	{
		const VkDescriptorSetAllocateInfo texturesAllocInfo = Vk::Initializers::DescriptorSetAllocateInfo(descriptorPool, { descriptorSetLayouts.textures });
		VK_CHECK_RESULT(vkAllocateDescriptorSets(vkDevice->logicalDevice, &texturesAllocInfo, &image.descriptorSet));
		VkWriteDescriptorSet texturesWriteDescriptorSet = Vk::Initializers::WriteDescriptorSet(image.descriptorSet, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0, &image.texture->descriptor);
		vkUpdateDescriptorSets(vkDevice->logicalDevice, 1, &texturesWriteDescriptorSet, 0, nullptr);
	}
}

void CG::TriangleEngine::PrepareUniformBuffers()
{
	auto cameraEntity = registry.create();
	CameraComponent& component = registry.assign<CameraComponent>(cameraEntity);

	component.vkDevice = vkDevice;
	component.uniformBufferVS.PrepareUniformBuffers(vkDevice, sizeof(component.uboVS));
	component.viewport.height = engineConfig.height;
	component.viewport.width = engineConfig.width;

	uniformBufferVS = &component.uniformBufferVS;

	camComp = &component;

	UpdateUniformBuffers();
}

void CG::TriangleEngine::UpdateUniformBuffers()
{
	uboData.projection = camComp->uboVS.projectionMatrix;
	uboData.view = camComp->uboVS.viewMatrix;
	uniformBufferVS->buffer.CopyTo(&uboData, sizeof(uboData));
}

void CG::TriangleEngine::SetupSystems()
{
	auto cameraSystem = std::make_unique<CameraSystem>();
	cameraSystem->SetDevice(vkDevice);
	systems.push_back(std::move(cameraSystem));

	auto lightSystem = std::make_unique<LightSystem>();
	systems.push_back(std::move(lightSystem));
}

void CG::TriangleEngine::DrawUI()
{
	ImGui::NewFrame();

	if (uiData.isActive)
	{
		// Init imGui windows and elements
		ImGui::Begin("Coldgaze overlay", &uiData.isActive, ImGuiWindowFlags_MenuBar);

		// Edit a color (stored as ~4 floats)
		ImGui::ColorEdit4("Background", &uiData.bgColor.x, ImGuiColorEditFlags_NoInputs);

		// Plot some values
		const float dummyData[] = { 0.2f, 0.1f, 1.0f, 0.5f, 0.9f, 2.2f };
		ImGui::PlotLines("Frame Times", dummyData, IM_ARRAYSIZE(dummyData));

		ImGui::End();
	}

	// Render to generate draw buffers
	ImGui::Render();
}

void CG::TriangleEngine::BuildUiCommandBuffers()
{
	DrawUI();
	imGui->UpdateBuffers();
}

void CG::TriangleEngine::LoadModel()
{
	testModel = std::make_unique<Vk::GLTFModel>();
	testModel->vkDevice = vkDevice;
	testModel->queue = queue;

	testModel->LoadFromFile(GetAssetPath() + "models/FlightHelmet/glTF/FlightHelmet.gltf");
}

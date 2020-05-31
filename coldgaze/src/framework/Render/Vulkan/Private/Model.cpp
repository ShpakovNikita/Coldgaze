#include "Render/Vulkan/Model.hpp"

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_MSC_SECURE_CRT

#pragma warning(push, 0)        
#include <glm/gtc/type_ptr.hpp>
#include "glm/ext/matrix_transform.hpp"
#pragma warning(pop)

#include "tinygltf/tiny_gltf.h"
#include "Render/Vulkan/Device.hpp"
#include "Render/Vulkan/Debug.hpp"
#include <mutex>
#include "Render/Vulkan/Exceptions.hpp"

namespace SGLTFModel
{
	using UString = std::basic_string<uint8_t>;

	UString FloatToUnsignedColor(const glm::vec4 color)
	{
		return { 
			static_cast<uint8_t>(color.r * 255.0f),
			static_cast<uint8_t>(color.g * 255.0f),
			static_cast<uint8_t>(color.b * 255.0f),
			static_cast<uint8_t>(color.a * 255.0f),
		};
	}

    CG::Vk::GLTFModel::Node* FindNode(CG::Vk::GLTFModel::Node* parent, uint32_t index) {
        CG::Vk::GLTFModel::Node* nodeFound = nullptr;
        if (parent->index == index) {
            return parent;
        }
        for (auto& child : parent->children) {
            nodeFound = FindNode(child.get(), index);
            if (nodeFound) {
                break;
            }
        }
        return nodeFound;
    }

    CG::Vk::GLTFModel::Node* NodeFromIndex(uint32_t index, const std::vector<std::unique_ptr<CG::Vk::GLTFModel::Node>>& nodes) {
        CG::Vk::GLTFModel::Node* nodeFound = nullptr;
        for (auto& node : nodes) {
            nodeFound = FindNode(node.get(), index);
            if (nodeFound) {
                break;
            }
        }
        return nodeFound;
    }
}

CG::Vk::GLTFModel::GLTFModel()
{ }

CG::Vk::GLTFModel::~GLTFModel()
{
	loaded = false;

    for (auto& texture : textures)
    {
        texture.texture.Destroy();
    }

	vertices.Destroy();
	indices.buffer.Destroy();
	indices.count = 0;
}

void CG::Vk::GLTFModel::LoadFromFile(const std::string& filename, float scale /*= 1.0f*/)
{
	tinygltf::Model glTFInput;
	tinygltf::TinyGLTF gltfContext;
	std::string error, warning;

	bool fileLoaded = gltfContext.LoadASCIIFromFile(&glTFInput, &error, &warning, filename);

	std::vector<uint32_t> indexBuffer;
	std::vector<Vertex> vertexBuffer;

	if (fileLoaded) 
	{
		LoadTextureSamplers(glTFInput);
		LoadTextures(glTFInput);
		LoadMaterials(glTFInput);

        const tinygltf::Scene& scene = glTFInput.scenes[glTFInput.defaultScene > -1 ? glTFInput.defaultScene : 0];
        for (size_t i = 0; i < scene.nodes.size(); i++) {
            const tinygltf::Node node = glTFInput.nodes[scene.nodes[i]];
            LoadNode(nullptr, node, scene.nodes[i], glTFInput, indexBuffer, vertexBuffer, scale);
        }

        LoadAnimations(glTFInput);
        LoadSkins(glTFInput);

        for (auto node : allNodes) {
            // Assign skins
            if (node->skinIndex > -1) {
                node->skin = skins[node->skinIndex].get();
            }
            // Initial pose
            if (node->mesh) {
                node->UpdateRecursive();
            }
        }
	}
	else {
		throw AssetLoadingException("Could not open the glTF file. Check, if it is correct");
		return;
	}

    extensions = glTFInput.extensionsUsed;

	size_t vertexBufferSize = vertexBuffer.size() * sizeof(Vertex);
	size_t indexBufferSize = indexBuffer.size() * sizeof(uint32_t);
	indices.count = static_cast<uint32_t>(indexBuffer.size());

	// We are creating this buffers to copy them on local memory for better performance
	Buffer vertexStaging, indexStaging;

	VK_CHECK_RESULT(vkDevice->CreateBuffer(
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&vertexStaging,
		vertexBufferSize,
		vertexBuffer.data()));

	VK_CHECK_RESULT(vkDevice->CreateBuffer(
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&indexStaging,
		indexBufferSize,
		indexBuffer.data()));

	VK_CHECK_RESULT(vkDevice->CreateBuffer(
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		&vertices,
		vertexBufferSize));
	VK_CHECK_RESULT(vkDevice->CreateBuffer(
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		&indices.buffer,
		indexBufferSize));

	VkCommandBuffer copyCmd = vkDevice->CreateCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
	VkBufferCopy copyRegion = {};

	copyRegion.size = vertexBufferSize;
	vkCmdCopyBuffer(
		copyCmd,
		vertexStaging.buffer,
		vertices.buffer,
		1,
		&copyRegion);

	copyRegion.size = indexBufferSize;
	vkCmdCopyBuffer(
		copyCmd,
		indexStaging.buffer,
		indices.buffer.buffer,
		1,
		&copyRegion);

	vkDevice->FlushCommandBuffer(copyCmd, queue, true);

	vertexStaging.Destroy();
	indexStaging.Destroy();
}

std::vector<CG::Vk::GLTFModel::Material>& CG::Vk::GLTFModel::GetMaterials()
{
    return materials;
}

const std::vector<CG::Vk::GLTFModel::Texture>& CG::Vk::GLTFModel::GetTextures() const
{
	return textures;
}

void CG::Vk::GLTFModel::Draw(VkCommandBuffer commandBuffer, VkPipelineLayout pipelineLayout)
{
	VkDeviceSize offsets[1] = { 0 };
	vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertices.buffer, offsets);
	vkCmdBindIndexBuffer(commandBuffer, indices.buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
	// Render all nodes at top-level
	for (auto& node : nodes) 
	{
		DrawNode(commandBuffer, pipelineLayout, *node);
	}
}

void CG::Vk::GLTFModel::DrawNode(VkCommandBuffer , VkPipelineLayout , const Node& )
{

}

bool CG::Vk::GLTFModel::IsLoaded() const
{
	return loaded;
}

void CG::Vk::GLTFModel::SetLoaded(bool aLoaded)
{
	loaded = aLoaded;
}

// from GLTF2 specs
VkSamplerAddressMode CG::Vk::GLTFModel::GetVkWrapMode(int32_t wrapMode)
{
    switch (wrapMode) 
	{
    case 10497:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 33071:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 33648:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

// from GLTF2 specs
VkFilter CG::Vk::GLTFModel::GetVkFilterMode(int32_t filterMode)
{
    switch (filterMode) 
	{
    case 9728:
        return VK_FILTER_NEAREST;
    case 9729:
        return VK_FILTER_LINEAR;
    case 9984:
        return VK_FILTER_NEAREST;
    case 9985:
        return VK_FILTER_NEAREST;
    case 9986:
        return VK_FILTER_LINEAR;
    case 9987:
        return VK_FILTER_LINEAR;
    default:
        return VK_FILTER_NEAREST;
    }
}

void CG::Vk::GLTFModel::LoadTextureSamplers(const tinygltf::Model& input)
{
    for (tinygltf::Sampler smpl : input.samplers) {
        TextureSampler sampler;
        sampler.minFilter = GetVkFilterMode(smpl.minFilter);
        sampler.magFilter = GetVkFilterMode(smpl.magFilter);
        sampler.addressModeU = GetVkWrapMode(smpl.wrapS);
        sampler.addressModeV = GetVkWrapMode(smpl.wrapT);
        sampler.addressModeW = sampler.addressModeV;
        textureSamplers.push_back(std::move(sampler));
    }
}

void CG::Vk::GLTFModel::LoadTextures(const tinygltf::Model& input)
{
    textures.resize(input.textures.size());

    for (const tinygltf::Texture& tex : input.textures) {
        const tinygltf::Image& image = input.images[tex.source];
        TextureSampler textureSampler;
        if (tex.sampler == -1) 
		{
            textureSampler.magFilter = VK_FILTER_LINEAR;
            textureSampler.minFilter = VK_FILTER_LINEAR;
            textureSampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            textureSampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            textureSampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
        else 
		{
            textureSampler = textureSamplers[tex.sampler];
        }
        Texture texture;
        texture.FromGLTFImage(image, textureSampler, vkDevice, queue);
        textures.push_back(std::move(texture));
    }
}

void CG::Vk::GLTFModel::LoadMaterials(const tinygltf::Model& input)
{
    for (const tinygltf::Material& mat : input.materials) {
        Material material = {};
        if (mat.values.find("baseColorTexture") != mat.values.end()) {
            material.baseColorTexture = &textures[mat.values.at("baseColorTexture").TextureIndex()];
            material.texCoordSets.baseColor = mat.values.at("baseColorTexture").TextureTexCoord();
        }
        if (mat.values.find("metallicRoughnessTexture") != mat.values.end()) {
            material.metallicRoughnessTexture = &textures[mat.values.at("metallicRoughnessTexture").TextureIndex()];
            material.texCoordSets.metallicRoughness = mat.values.at("metallicRoughnessTexture").TextureTexCoord();
        }
        if (mat.values.find("roughnessFactor") != mat.values.end()) {
            material.roughnessFactor = static_cast<float>(mat.values.at("roughnessFactor").Factor());
        }
        if (mat.values.find("metallicFactor") != mat.values.end()) {
            material.metallicFactor = static_cast<float>(mat.values.at("metallicFactor").Factor());
        }
        if (mat.values.find("baseColorFactor") != mat.values.end()) {
            material.baseColorFactor = glm::make_vec4(mat.values.at("baseColorFactor").ColorFactor().data());
        }
        if (mat.additionalValues.find("normalTexture") != mat.additionalValues.end()) {
            material.normalTexture = &textures[mat.additionalValues.at("normalTexture").TextureIndex()];
            material.texCoordSets.normal = mat.additionalValues.at("normalTexture").TextureTexCoord();
        }
        if (mat.additionalValues.find("emissiveTexture") != mat.additionalValues.end()) {
            material.emissiveTexture = &textures[mat.additionalValues.at("emissiveTexture").TextureIndex()];
            material.texCoordSets.emissive = mat.additionalValues.at("emissiveTexture").TextureTexCoord();
        }
        if (mat.additionalValues.find("occlusionTexture") != mat.additionalValues.end()) {
            material.occlusionTexture = &textures[mat.additionalValues.at("occlusionTexture").TextureIndex()];
            material.texCoordSets.occlusion = mat.additionalValues.at("occlusionTexture").TextureTexCoord();
        }
        if (mat.additionalValues.find("alphaMode") != mat.additionalValues.end()) {
            tinygltf::Parameter param = mat.additionalValues.at("alphaMode");
            if (param.string_value == "BLEND") {
                material.alphaMode = Material::eAlphaMode::kAlphaModeBlend;
            }
            if (param.string_value == "MASK") {
                material.alphaCutoff = 0.5f;
                material.alphaMode = Material::eAlphaMode::kAlphaModeMask;
            }
        }
        if (mat.additionalValues.find("alphaCutoff") != mat.additionalValues.end()) {
            material.alphaCutoff = static_cast<float>(mat.additionalValues.at("alphaCutoff").Factor());
        }
        if (mat.additionalValues.find("emissiveFactor") != mat.additionalValues.end()) {
            material.emissiveFactor = glm::vec4(glm::make_vec3(mat.additionalValues.at("emissiveFactor").ColorFactor().data()), 1.0);
            material.emissiveFactor = glm::vec4(0.0f);
        }

        materials.push_back(std::move(material));
    }
    // Push a default material at the end of the list for meshes with no material assigned
    materials.push_back(std::move(Material()));
}

void CG::Vk::GLTFModel::LoadAnimations(const tinygltf::Model& input)
{
    for (const tinygltf::Animation& anim : input.animations) {
        Animation animation = {};
        animation.name = anim.name;

        // Samplers
        for (const tinygltf::AnimationSampler& samp : anim.samplers) {
            AnimationSampler sampler = {};

            if (samp.interpolation == "LINEAR") {
                sampler.interpolation = AnimationSampler::eInterpolationType::kLinear;
            }
            if (samp.interpolation == "STEP") {
                sampler.interpolation = AnimationSampler::eInterpolationType::kStep;
            }
            if (samp.interpolation == "CUBICSPLINE") {
                sampler.interpolation = AnimationSampler::eInterpolationType::kCubicSpline;
            }

            // Read sampler input time values
            {
                const tinygltf::Accessor& accessor = input.accessors[samp.input];
                const tinygltf::BufferView& bufferView = input.bufferViews[accessor.bufferView];
                const tinygltf::Buffer& buffer = input.buffers[bufferView.buffer];

                assert(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT);

                const void* dataPtr = &buffer.data[accessor.byteOffset + bufferView.byteOffset];
                const float* buf = static_cast<const float*>(dataPtr);
                for (size_t index = 0; index < accessor.count; index++) {
                    sampler.inputs.push_back(buf[index]);
                }

                for (auto animInput : sampler.inputs) {
                    if (animInput < animation.start) {
                        animation.start = animInput;
                    };
                    if (animInput > animation.end) {
                        animation.end = animInput;
                    }
                }
            }

            // Read sampler output T/R/S values 
            {
                const tinygltf::Accessor& accessor = input.accessors[samp.output];
                const tinygltf::BufferView& bufferView = input.bufferViews[accessor.bufferView];
                const tinygltf::Buffer& buffer = input.buffers[bufferView.buffer];

                assert(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT);

                const void* dataPtr = &buffer.data[accessor.byteOffset + bufferView.byteOffset];

                switch (accessor.type) {
                case TINYGLTF_TYPE_VEC3: {
                    const glm::vec3* buf = static_cast<const glm::vec3*>(dataPtr);
                    for (size_t index = 0; index < accessor.count; index++) {
                        sampler.outputsVec4.push_back(glm::vec4(buf[index], 0.0f));
                    }
                    break;
                }
                case TINYGLTF_TYPE_VEC4: {
                    const glm::vec4* buf = static_cast<const glm::vec4*>(dataPtr);
                    for (size_t index = 0; index < accessor.count; index++) {
                        sampler.outputsVec4.push_back(buf[index]);
                    }
                    break;
                }
                default: {
                    std::cout << "unknown type" << std::endl;
                    break;
                }
                }
            }

            animation.samplers.push_back(std::move(sampler));
        }

        // Channels
        for (const tinygltf::AnimationChannel& source : anim.channels) {
            AnimationChannel channel = {};

            if (source.target_path == "rotation") {
                channel.path = AnimationChannel::ePathType::kRotation;
            }
            if (source.target_path == "translation") {
                channel.path = AnimationChannel::ePathType::kTranslation;
            }
            if (source.target_path == "scale") {
                channel.path = AnimationChannel::ePathType::kScale;
            }
            if (source.target_path == "weights") {
                std::cout << "weights not yet supported, skipping channel" << std::endl;
                continue;
            }
            channel.samplerIndex = source.sampler;
            channel.node = SGLTFModel::NodeFromIndex(source.target_node, nodes);
            if (!channel.node) {
                continue;
            }

            animation.channels.push_back(std::move(channel));
        }

        animations.push_back(std::move(animation));
    }
}

void CG::Vk::GLTFModel::LoadSkins(const tinygltf::Model& input)
{
    for (const tinygltf::Skin& source : input.skins) {
        std::unique_ptr<Skin> newSkin = std::make_unique<Skin>();
        newSkin->name = source.name;

        // Find skeleton root node
        if (source.skeleton > -1) {
            newSkin->skeletonRoot = SGLTFModel::NodeFromIndex(source.skeleton, nodes);
        }

        // Find joint nodes
        for (int jointIndex : source.joints) {
            Node* node = SGLTFModel::NodeFromIndex(jointIndex, nodes);
            if (node) {
                newSkin->joints.push_back(SGLTFModel::NodeFromIndex(jointIndex, nodes));
            }
        }

        // Get inverse bind matrices from buffer
        if (source.inverseBindMatrices > -1) {
            const tinygltf::Accessor& accessor = input.accessors[source.inverseBindMatrices];
            const tinygltf::BufferView& bufferView = input.bufferViews[accessor.bufferView];
            const tinygltf::Buffer& buffer = input.buffers[bufferView.buffer];
            newSkin->inverseBindMatrices.resize(accessor.count);
            memcpy(newSkin->inverseBindMatrices.data(), &buffer.data[accessor.byteOffset + bufferView.byteOffset], accessor.count * sizeof(glm::mat4));
        }

        skins.push_back(std::move(newSkin));
    }
}

void CG::Vk::GLTFModel::LoadNode(Node* parent, const tinygltf::Node& node, uint32_t nodeIndex, 
    const tinygltf::Model& input, std::vector<uint32_t>& indexBuffer, std::vector<Vertex>& vertexBuffer, float globalscale)
{
    std::unique_ptr<Node> newNode = std::make_unique<Node>();
    newNode->index = nodeIndex;
    newNode->parent = parent;
    newNode->name = node.name;
    newNode->skinIndex = node.skin;
    newNode->matrix = glm::mat4(1.0f);

    // Generate local node matrix
    glm::vec3 translation = glm::vec3(0.0f);
    if (node.translation.size() == 3) {
        translation = glm::make_vec3(node.translation.data());
        newNode->translation = translation;
    }
    glm::mat4 rotation = glm::mat4(1.0f);
    if (node.rotation.size() == 4) {
        glm::quat q = glm::make_quat(node.rotation.data());
        newNode->rotation = glm::mat4(q);
    }
    glm::vec3 scale = glm::vec3(1.0f);
    if (node.scale.size() == 3) {
        scale = glm::make_vec3(node.scale.data());
        newNode->scale = scale;
    }
    if (node.matrix.size() == 16) {
        newNode->matrix = glm::make_mat4x4(node.matrix.data());
    };

    // Node with children
    if (node.children.size() > 0) {
        for (size_t i = 0; i < node.children.size(); i++) {
            LoadNode(newNode.get(), input.nodes[node.children[i]], node.children[i], input, indexBuffer, vertexBuffer, globalscale);
        }
    }

    // Node contains mesh data
    if (node.mesh > -1) {
        const tinygltf::Mesh mesh = input.meshes[node.mesh];
        std::unique_ptr<Mesh> newMesh = std::make_unique<Mesh>(vkDevice, newNode->matrix);
        for (size_t j = 0; j < mesh.primitives.size(); j++) {
            const tinygltf::Primitive& primitive = mesh.primitives[j];
            uint32_t indexStart = static_cast<uint32_t>(indexBuffer.size());
            uint32_t vertexStart = static_cast<uint32_t>(vertexBuffer.size());
            uint32_t indexCount = 0;
            uint32_t vertexCount = 0;
            bool hasSkin = false;
            bool hasIndices = primitive.indices > -1;
            // Vertices
            {
                const float* bufferPos = nullptr;
                const float* bufferNormals = nullptr;
                const float* bufferTexCoordSet0 = nullptr;
                const float* bufferTexCoordSet1 = nullptr;
                const uint16_t* bufferJoints = nullptr;
                const float* bufferWeights = nullptr;

                int posByteStride = 0;
                int normByteStride = 0;
                int uv0ByteStride = 0;
                int uv1ByteStride = 0;
                int jointByteStride = 0;
                int weightByteStride = 0;

                // Position attribute is required
                assert(primitive.attributes.find("POSITION") != primitive.attributes.end());

                const tinygltf::Accessor& posAccessor = input.accessors[primitive.attributes.find("POSITION")->second];
                const tinygltf::BufferView& posView = input.bufferViews[posAccessor.bufferView];
                bufferPos = reinterpret_cast<const float*>(&(input.buffers[posView.buffer].data[posAccessor.byteOffset + posView.byteOffset]));
                vertexCount = static_cast<uint32_t>(posAccessor.count);
                posByteStride = posAccessor.ByteStride(posView) ? (posAccessor.ByteStride(posView) / sizeof(float)) : tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC3);

                // TODO: refactor after working scene establishing
                if (primitive.attributes.find("NORMAL") != primitive.attributes.end()) {
                    const tinygltf::Accessor& normAccessor = input.accessors[primitive.attributes.find("NORMAL")->second];
                    const tinygltf::BufferView& normView = input.bufferViews[normAccessor.bufferView];
                    bufferNormals = reinterpret_cast<const float*>(&(input.buffers[normView.buffer].data[normAccessor.byteOffset + normView.byteOffset]));
                    normByteStride = normAccessor.ByteStride(normView) ? (normAccessor.ByteStride(normView) / sizeof(float)) : tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC3);
                }

                if (primitive.attributes.find("TEXCOORD_0") != primitive.attributes.end()) {
                    const tinygltf::Accessor& uvAccessor = input.accessors[primitive.attributes.find("TEXCOORD_0")->second];
                    const tinygltf::BufferView& uvView = input.bufferViews[uvAccessor.bufferView];
                    bufferTexCoordSet0 = reinterpret_cast<const float*>(&(input.buffers[uvView.buffer].data[uvAccessor.byteOffset + uvView.byteOffset]));
                    uv0ByteStride = uvAccessor.ByteStride(uvView) ? (uvAccessor.ByteStride(uvView) / sizeof(float)) : tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC2);
                }

                if (primitive.attributes.find("TEXCOORD_1") != primitive.attributes.end()) {
                    const tinygltf::Accessor& uvAccessor = input.accessors[primitive.attributes.find("TEXCOORD_1")->second];
                    const tinygltf::BufferView& uvView = input.bufferViews[uvAccessor.bufferView];
                    bufferTexCoordSet1 = reinterpret_cast<const float*>(&(input.buffers[uvView.buffer].data[uvAccessor.byteOffset + uvView.byteOffset]));
                    uv1ByteStride = uvAccessor.ByteStride(uvView) ? (uvAccessor.ByteStride(uvView) / sizeof(float)) : tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC2);
                }

                if (primitive.attributes.find("JOINTS_0") != primitive.attributes.end()) {
                    const tinygltf::Accessor& jointAccessor = input.accessors[primitive.attributes.find("JOINTS_0")->second];
                    const tinygltf::BufferView& jointView = input.bufferViews[jointAccessor.bufferView];
                    bufferJoints = reinterpret_cast<const uint16_t*>(&(input.buffers[jointView.buffer].data[jointAccessor.byteOffset + jointView.byteOffset]));
                    jointByteStride = jointAccessor.ByteStride(jointView) ? (jointAccessor.ByteStride(jointView) / sizeof(bufferJoints[0])) : tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC4);
                }

                if (primitive.attributes.find("WEIGHTS_0") != primitive.attributes.end()) {
                    const tinygltf::Accessor& weightAccessor = input.accessors[primitive.attributes.find("WEIGHTS_0")->second];
                    const tinygltf::BufferView& weightView = input.bufferViews[weightAccessor.bufferView];
                    bufferWeights = reinterpret_cast<const float*>(&(input.buffers[weightView.buffer].data[weightAccessor.byteOffset + weightView.byteOffset]));
                    weightByteStride = weightAccessor.ByteStride(weightView) ? (weightAccessor.ByteStride(weightView) / sizeof(float)) : tinygltf::GetNumComponentsInType(TINYGLTF_TYPE_VEC4);
                }

                hasSkin = (bufferJoints && bufferWeights);

                for (size_t v = 0; v < posAccessor.count; ++v) {
                    Vertex vert = {};
                    vert.pos = glm::vec4(glm::make_vec3(&bufferPos[v * posByteStride]), 1.0f);
                    vert.normal = glm::normalize(glm::vec3(bufferNormals ? glm::make_vec3(&bufferNormals[v * normByteStride]) : glm::vec3(0.0f)));
                    vert.uv0 = bufferTexCoordSet0 ? glm::make_vec2(&bufferTexCoordSet0[v * uv0ByteStride]) : glm::vec2(0.0f);
                    vert.uv1 = bufferTexCoordSet1 ? glm::make_vec2(&bufferTexCoordSet1[v * uv1ByteStride]) : glm::vec2(0.0f);

                    vert.joint0 = hasSkin ? glm::vec4(glm::make_vec4(&bufferJoints[v * jointByteStride])) : glm::vec4(0.0f);
                    vert.weight0 = hasSkin ? glm::make_vec4(&bufferWeights[v * weightByteStride]) : glm::vec4(0.0f);
                    // Fix for all zero weights
                    if (glm::length(vert.weight0) == 0.0f) {
                        vert.weight0 = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
                    }
                    vertexBuffer.push_back(std::move(vert));
                }
            }
            // Indices
            if (hasIndices)
            {
                const tinygltf::Accessor& accessor = input.accessors[primitive.indices > -1 ? primitive.indices : 0];
                const tinygltf::BufferView& bufferView = input.bufferViews[accessor.bufferView];
                const tinygltf::Buffer& buffer = input.buffers[bufferView.buffer];

                indexCount = static_cast<uint32_t>(accessor.count);
                const void* dataPtr = &(buffer.data[accessor.byteOffset + bufferView.byteOffset]);

                switch (accessor.componentType) {
                case TINYGLTF_PARAMETER_TYPE_UNSIGNED_INT: {
                    const uint32_t* buf = static_cast<const uint32_t*>(dataPtr);
                    for (size_t index = 0; index < accessor.count; index++) {
                        indexBuffer.push_back(buf[index] + vertexStart);
                    }
                    break;
                }
                case TINYGLTF_PARAMETER_TYPE_UNSIGNED_SHORT: {
                    const uint16_t* buf = static_cast<const uint16_t*>(dataPtr);
                    for (size_t index = 0; index < accessor.count; index++) {
                        indexBuffer.push_back(buf[index] + vertexStart);
                    }
                    break;
                }
                case TINYGLTF_PARAMETER_TYPE_UNSIGNED_BYTE: {
                    const uint8_t* buf = static_cast<const uint8_t*>(dataPtr);
                    for (size_t index = 0; index < accessor.count; index++) {
                        indexBuffer.push_back(buf[index] + vertexStart);
                    }
                    break;
                }
                default:
                    std::cerr << "Index component type " << accessor.componentType << " not supported!" << std::endl;
                    return;
                }
            }

            // loading last material as default one
            std::unique_ptr<Primitive> newPrimitive = std::make_unique<Primitive>(indexStart, indexCount, vertexCount, 
                primitive.material > -1 ? materials[primitive.material] : materials.back());
            newMesh->primitives.push_back(std::move(newPrimitive));
        }

        newNode->mesh = std::move(newMesh);
    }
    if (parent) {
        parent->children.push_back(std::move(newNode));
        allNodes.push_back(parent->children.back().get());
    }
    else {
        nodes.push_back(std::move(newNode));
        allNodes.push_back(nodes.back().get());
    }
}

CG::Vk::GLTFModel::Mesh::Mesh(Device* vkDevice, const glm::mat4& meshMat)
{
	uniformBlock.matrix = meshMat;

    VK_CHECK_RESULT(vkDevice->CreateBuffer(
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &uniformBuffer.buffer,
        sizeof(uniformBlock)));

    uniformBuffer.buffer.Map(sizeof(uniformBlock));
    uniformBuffer.buffer.SetupDescriptor(sizeof(uniformBlock));
}

void CG::Vk::GLTFModel::Mesh::UpdateUniformBuffers()
{
	uniformBuffer.buffer.CopyTo(&uniformBlock, sizeof(uniformBlock));
}

glm::mat4 CG::Vk::GLTFModel::Node::GetLocalMatrix()
{
	return glm::translate(glm::mat4(1.0f), translation) * glm::mat4(rotation) * glm::scale(glm::mat4(1.0f), scale) * matrix;
}

glm::mat4 CG::Vk::GLTFModel::Node::GetWorldMatrix()
{
    glm::mat4 localMat = GetLocalMatrix();
    Node* currentParent = parent;
    while (currentParent) {
        localMat = currentParent->GetLocalMatrix() * localMat;
        currentParent = currentParent->parent;
    }
    return localMat;
}

void CG::Vk::GLTFModel::Node::UpdateRecursive()
{
    if (mesh) {
        glm::mat4 worldMat = GetWorldMatrix();
        if (skin) {
            mesh->uniformBlock.matrix = worldMat;

            glm::mat4 inverseTransform = glm::inverse(worldMat);
            size_t numJoints = std::min((uint32_t)skin->joints.size(), kMaxJointsCount);
            for (size_t i = 0; i < numJoints; i++) {
                Node* jointNode = skin->joints[i];
                glm::mat4 jointMat = jointNode->GetWorldMatrix() * skin->inverseBindMatrices[i];
                jointMat = inverseTransform * jointMat;
                mesh->uniformBlock.jointMatrix[i] = jointMat;
            }
            mesh->uniformBlock.jointcount = static_cast<float>(numJoints);
			mesh->UpdateUniformBuffers();
        }
        else {
			mesh->uniformBuffer.buffer.CopyTo(&worldMat, sizeof(glm::mat4));
        }
    }

    for (auto& child : children) {
        child->UpdateRecursive();
    }
}

void CG::Vk::GLTFModel::Texture::FromGLTFImage(const tinygltf::Image& glTFImage, TextureSampler textureSampler, Device* device, VkQueue copyQueue)
{
	VkDeviceSize bufferSize = 0;

    // We convert RGB-only images to RGBA, as most devices don't support RGB-formats in Vulkan
    if (glTFImage.component == 3) {
        uint64_t pixelsCount = static_cast<uint64_t>(glTFImage.width) * static_cast<uint64_t>(glTFImage.height);
        bufferSize = pixelsCount * 4;
        unsigned char* buffer = new unsigned char[bufferSize];
        unsigned char* rgba = buffer;
        const unsigned char* rgb = &glTFImage.image[0];
        for (size_t pixel = 0; pixel < pixelsCount; ++pixel) {
            for (int32_t j = 0; j < 3; ++j) {
                rgba[j] = rgb[j];
            }
            rgba[3] = 0;

            rgba += 4;
            rgb += 3;
        }

        texture.FromBuffer(buffer, bufferSize, VK_FORMAT_R8G8B8A8_UNORM, glTFImage.width, glTFImage.height,
            device, copyQueue, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_TILING_OPTIMAL, textureSampler);
    }
    else {
        const unsigned char* buffer = &glTFImage.image[0];
        bufferSize = glTFImage.image.size();

        texture.FromBuffer(buffer, bufferSize, VK_FORMAT_R8G8B8A8_UNORM, glTFImage.width, glTFImage.height,
            device, copyQueue, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_TILING_OPTIMAL, textureSampler);
    }
}

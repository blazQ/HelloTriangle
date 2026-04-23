#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

struct Vertex
{
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec2 texCoord;
    glm::vec3 normal;
    glm::vec4 tangent; // xyz = tangent direction in world space, w = bitangent sign (+1 or -1)

    bool operator==(const Vertex &o) const
    {
        return pos == o.pos && color == o.color &&
               texCoord == o.texCoord && normal == o.normal &&
               tangent == o.tangent;
    }
};

struct GltfImage {
    std::string          path;  // non-empty = external file
    std::vector<uint8_t> bytes; // non-empty = embedded GLB image
};

struct GltfPrimitive {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    glm::mat4             transform;
    GltfImage             baseColor;
    GltfImage             normalMap;
    GltfImage             metallicRoughness;
};



std::pair<std::vector<Vertex>, std::vector<uint32_t>> makeCube(glm::vec3 color, float size);
std::pair<std::vector<Vertex>, std::vector<uint32_t>> makePlane(glm::vec3 color, float size);
// size = radius. sectors = longitude divisions, stacks = latitude divisions.
// Normals point outward; tangents follow the direction of increasing longitude.
std::pair<std::vector<Vertex>, std::vector<uint32_t>> makeSphere(glm::vec3 color, float radius,
                                                                   uint32_t sectors = 32,
                                                                   uint32_t stacks  = 16);
std::vector<GltfPrimitive> loadGLTF(const std::filesystem::path &path, bool yUpToZUp = false);
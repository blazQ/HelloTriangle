#include "Scene.hpp"

#include <stdexcept>
#include <unordered_map>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

std::pair<std::vector<Vertex>, std::vector<uint32_t>> makeCube(glm::vec3 color, float size)
{
    float h = size * 0.5f;
    std::vector<Vertex> verts = {
        // +Z face (front)
        {{-h, -h, h}, color, {0, 0}, {0, 0, 1}},
        {{h, -h, h}, color, {1, 0}, {0, 0, 1}},
        {{h, h, h}, color, {1, 1}, {0, 0, 1}},
        {{-h, h, h}, color, {0, 1}, {0, 0, 1}},
        // -Z face (back)
        {{h, -h, -h}, color, {0, 0}, {0, 0, -1}},
        {{-h, -h, -h}, color, {1, 0}, {0, 0, -1}},
        {{-h, h, -h}, color, {1, 1}, {0, 0, -1}},
        {{h, h, -h}, color, {0, 1}, {0, 0, -1}},
        // +X face (right)
        {{h, -h, h}, color, {0, 0}, {1, 0, 0}},
        {{h, -h, -h}, color, {1, 0}, {1, 0, 0}},
        {{h, h, -h}, color, {1, 1}, {1, 0, 0}},
        {{h, h, h}, color, {0, 1}, {1, 0, 0}},
        // -X face (left)
        {{-h, -h, -h}, color, {0, 0}, {-1, 0, 0}},
        {{-h, -h, h}, color, {1, 0}, {-1, 0, 0}},
        {{-h, h, h}, color, {1, 1}, {-1, 0, 0}},
        {{-h, h, -h}, color, {0, 1}, {-1, 0, 0}},
        // +Y face (top)
        {{-h, h, h}, color, {0, 0}, {0, 1, 0}},
        {{h, h, h}, color, {1, 0}, {0, 1, 0}},
        {{h, h, -h}, color, {1, 1}, {0, 1, 0}},
        {{-h, h, -h}, color, {0, 1}, {0, 1, 0}},
        // -Y face (bottom)
        {{-h, -h, -h}, color, {0, 0}, {0, -1, 0}},
        {{h, -h, -h}, color, {1, 0}, {0, -1, 0}},
        {{h, -h, h}, color, {1, 1}, {0, -1, 0}},
        {{-h, -h, h}, color, {0, 1}, {0, -1, 0}},
    };
    std::vector<uint32_t> idxs;
    for (uint32_t f = 0; f < 6; ++f)
    {
        uint32_t b = f * 4;
        idxs.insert(idxs.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
    }
    return {verts, idxs};
}

std::pair<std::vector<Vertex>, std::vector<uint32_t>> loadOBJ(const std::string &path)
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, path.c_str()))
        throw std::runtime_error("loadOBJ failed for '" + path + "': " + warn + err);

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::unordered_map<Vertex, uint32_t> uniqueVertices;

    for (const auto &shape : shapes)
    {
        for (const auto &index : shape.mesh.indices)
        {
            Vertex v{};
            v.pos = {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2],
            };
            v.color = {1.0f, 1.0f, 1.0f};
            if (index.texcoord_index >= 0)
            {
                v.texCoord = {
                    attrib.texcoords[2 * index.texcoord_index + 0],
                    1.0f - attrib.texcoords[2 * index.texcoord_index + 1], // flip Y
                };
            }
            if (index.normal_index >= 0)
            {
                v.normal = {
                    attrib.normals[3 * index.normal_index + 0],
                    attrib.normals[3 * index.normal_index + 1],
                    attrib.normals[3 * index.normal_index + 2],
                };
            }

            if (uniqueVertices.count(v) == 0)
            {
                uniqueVertices[v] = static_cast<uint32_t>(vertices.size());
                vertices.push_back(v);
            }
            indices.push_back(uniqueVertices[v]);
        }
    }

    return {vertices, indices};
}

std::pair<std::vector<Vertex>, std::vector<uint32_t>> makePlane(glm::vec3 color, float size)
{
    float h = size * 0.5f;
    std::vector<Vertex> verts = {
        {{-h, -h, 0.0f}, color, {0, 0}, {0, 0, 1}},
        {{h, -h, 0.0f}, color, {1, 0}, {0, 0, 1}},
        {{h, h, 0.0f}, color, {1, 1}, {0, 0, 1}},
        {{-h, h, 0.0f}, color, {0, 1}, {0, 0, 1}},
    };
    std::vector<uint32_t> idxs = {0, 1, 2, 0, 2, 3};
    return {verts, idxs};
}

#include "SceneEditor.hpp"

#include <cmath>
#include <string>

#include "imgui.h"

void SceneEditor::draw(RenderSettings&          settings,
                       std::vector<Renderable>& renderables,
                       Camera&                  camera,
                       const DisplayInfo&       info)
{
    ImGui::GetIO().DisplaySize = ImVec2(
        static_cast<float>(info.resolution.width),
        static_cast<float>(info.resolution.height));

    ImGui::Begin("Info");

    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::Text("Frame time: %.3f ms", 1000.0f / ImGui::GetIO().Framerate);
    ImGui::Separator();
    ImGui::Text("Resolution: %ux%u", info.resolution.width, info.resolution.height);
    ImGui::Text("Device: %s", info.deviceName);
    ImGui::Text("Textures loaded: %u", info.textureCount);

    ImGui::Separator();

    bool vsync = (settings.pendingPresentMode == vk::PresentModeKHR::eFifo);
    if (ImGui::Checkbox("V-Sync", &vsync))
        settings.pendingPresentMode = vsync ? vk::PresentModeKHR::eFifo
                                            : vk::PresentModeKHR::eMailbox;

    static const vk::SampleCountFlagBits kSampleCounts[] = {
        vk::SampleCountFlagBits::e1,  vk::SampleCountFlagBits::e2,
        vk::SampleCountFlagBits::e4,  vk::SampleCountFlagBits::e8,
        vk::SampleCountFlagBits::e16, vk::SampleCountFlagBits::e32,
        vk::SampleCountFlagBits::e64};
    if (ImGui::BeginCombo("MSAA", vk::to_string(settings.pendingMsaaSamples).c_str()))
    {
        for (auto count : kSampleCounts)
        {
            if (!(info.supportedMsaa & count)) continue;
            bool selected = (count == settings.pendingMsaaSamples);
            if (ImGui::Selectable(vk::to_string(count).c_str(), selected))
                settings.pendingMsaaSamples = count;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();

    ImGui::Checkbox("Orbit light", &settings.lightOrbit);
    if (!settings.lightOrbit)
        ImGui::SliderAngle("Light angle", &settings.lightAngle, 0.0f, 360.0f);
    ImGui::SliderFloat("Shadow bias min",  &settings.shadowBiasMin, 0.0f, 0.01f,  "%.4f");
    ImGui::SliderFloat("Shadow bias max",  &settings.shadowBiasMax, 0.0f, 0.02f,  "%.4f");
    ImGui::SliderFloat("Shadow distance",  &settings.shadowFar,     5.0f, 200.0f, "%.1f");

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Parallax (POM)"))
    {
        ImGui::SliderFloat("Depth scale", &settings.pomDepthScale, 0.001f, 0.2f, "%.3f");
        ImGui::SliderFloat("Min steps",   &settings.pomMinSteps,   4.0f,  16.0f, "%.0f");
        ImGui::SliderFloat("Max steps",   &settings.pomMaxSteps,   8.0f,  64.0f, "%.0f");
        ImGui::TextDisabled("Assign 'heightMap' in scene.json to activate POM per object.");
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Camera"))
        camera.drawImGui();

    ImGui::Separator();

    ImGui::Checkbox("Tonemapping (ACES)", &settings.tonemapping);
    if (settings.tonemapping)
        ImGui::SliderFloat("Exposure",         &settings.exposure,         0.1f, 10.0f);
    ImGui::SliderFloat("Ambient",              &settings.ambient,          0.0f,  1.0f);
    ImGui::SliderFloat("Default Roughness",    &settings.defaultRoughness, 0.0f,  1.0f);
    ImGui::SliderFloat("Default Metallic",     &settings.defaultMetallic,  0.0f,  1.0f);

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Sky"))
    {
        ImGui::Checkbox("Enabled##sky", &settings.skyEnabled);
        if (settings.skyEnabled)
        {
            ImGui::ColorEdit3("Horizon",   &settings.skyPush.horizonColor.x);
            ImGui::ColorEdit3("Zenith",    &settings.skyPush.zenithColor.x);
            ImGui::ColorEdit3("Ground",    &settings.skyPush.groundColor.x);
            ImGui::ColorEdit3("Sun color", &settings.skyPush.sunParams.x);
            float sunDeg = glm::degrees(std::acos(settings.skyPush.sunParams.w));
            if (ImGui::SliderFloat("Sun size (deg)", &sunDeg, 0.1f, 10.0f))
                settings.skyPush.sunParams.w = std::cos(glm::radians(sunDeg));
        }
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Fog"))
    {
        ImGui::Checkbox("Enabled##fog", &settings.fogEnabled);
        if (settings.fogEnabled)
        {
            ImGui::SliderFloat("Density",       &settings.fogDensity,       0.0f, 0.1f,  "%.4f");
            ImGui::SliderFloat("Height falloff", &settings.fogHeightFalloff, 0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("Max opacity",    &settings.fogMaxOpacity,    0.0f, 1.0f,  "%.2f");
            ImGui::Checkbox("Sync color to sky horizon", &settings.fogSyncSky);
            if (!settings.fogSyncSky)
                ImGui::ColorEdit3("Fog color", &settings.fogColor.x);
            else
            {
                glm::vec3 c = settings.fogColor;
                ImGui::BeginDisabled();
                ImGui::ColorEdit3("Fog color (synced)", &c.x);
                ImGui::EndDisabled();
            }
        }
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Point Lights"))
    {
        bool atMax = static_cast<int>(settings.pointLights.size()) >= RenderSettings::MAX_POINT_LIGHTS;
        if (atMax) ImGui::BeginDisabled();
        if (ImGui::Button("Add Light"))
            settings.pointLights.push_back(PointLightData{});
        if (atMax) { ImGui::EndDisabled(); ImGui::SameLine();
                     ImGui::TextDisabled("(max %d)", RenderSettings::MAX_POINT_LIGHTS); }

        int removeIdx = -1;
        for (int i = 0; i < static_cast<int>(settings.pointLights.size()); ++i)
        {
            auto& pl = settings.pointLights[i];
            ImGui::PushID(i);
            std::string label = "Light " + std::to_string(i);
            if (ImGui::TreeNode(label.c_str()))
            {
                ImGui::Checkbox("Enabled",     &pl.enabled);
                ImGui::DragFloat3("Position",  &pl.position.x, 0.05f);
                ImGui::ColorEdit3("Color",     &pl.color.x);
                ImGui::SliderFloat("Intensity", &pl.intensity, 0.0f, 20.0f);
                ImGui::SliderFloat("Radius",    &pl.radius,    0.5f, 50.0f);
                if (ImGui::Button("Remove")) removeIdx = i;
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (removeIdx >= 0)
            settings.pointLights.erase(settings.pointLights.begin() + removeIdx);
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Objects"))
    {
        for (size_t i = 0; i < renderables.size(); ++i)
        {
            auto& r = renderables[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::TreeNode(r.label.c_str()))
            {
                bool changed = false;
                changed |= ImGui::DragFloat3("Position", &r.position.x,    0.05f);
                changed |= ImGui::DragFloat3("Rotation", &r.rotationDeg.x, 1.0f, -180.0f, 180.0f);
                changed |= ImGui::DragFloat("Scale",     &r.scale,          0.01f, 0.01f, 100.0f);
                if (changed)
                    r.modelMatrix = buildModelMatrix(r.position, r.rotationDeg, r.scale);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    ImGui::End();
}

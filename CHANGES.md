# HelloTriangle — Implementation Notes

A study guide for every significant change made since the single-texture, single-model starting point. Read top to bottom; each section builds on the previous.

---

## 1. Vulkan 1.2 Feature Requirements

### What changed
Three new GPU feature flags are now requested in `Device.cpp` when the logical device is created, and checked in `isDeviceSuitable` before accepting a physical device:

```cpp
// Inside the StructureChain for vk::PhysicalDeviceVulkan12Features:
.shaderSampledImageArrayNonUniformIndexing = true,
.descriptorBindingPartiallyBound           = true,
.runtimeDescriptorArray                    = true,
```

### Why each flag matters

**`runtimeDescriptorArray`**
Normally, descriptor arrays in shaders must have a compile-time size. This flag lifts that restriction. Without it you cannot write `Sampler2D textures[]` with no fixed count in a Slang/HLSL/GLSL shader — the compiler would reject it. With it, the array size is determined at pipeline creation time, not shader compilation time.

**`descriptorBindingPartiallyBound`**
When you allocate a descriptor set with 64 texture slots but only fill 3 of them, Vulkan by default requires every slot to be written before any draw call — even slots you never access. `PARTIALLY_BOUND` removes that requirement. As long as you don't *index into* an unwritten slot from a shader invocation, the driver is happy. This is essential for our design: we allocate `MAX_TEXTURES = 64` slots and fill only as many as the current scene needs.

**`shaderSampledImageArrayNonUniformIndexing`**
GPUs process many threads in lockstep groups called *wavefronts* or *subgroups*. When every thread in a group samples from the same texture, the hardware can optimize the fetch. When threads sample from *different* textures (as in bindless — each draw call picks a different index), the hardware must handle them independently. This flag tells the driver that your index is *non-uniform* across threads in a wave, enabling the driver to compile correct code for it. In the shader, this maps to the `NonUniformResourceIndex()` wrapper around the array index.

---

## 2. Exception Safety in `createImage` / `createBuffer`

### The bug
Both functions took output parameters by reference and wrote into them as they went:

```cpp
// Old, broken:
image       = vk::raii::Image(device, imageInfo);        // image is now set
imageMemory = vk::raii::DeviceMemory(device, allocInfo); // may throw OOM!
image.bindMemory(imageMemory, 0);
```

If `allocateMemory` threw (e.g. `ErrorOutOfDeviceMemory` during a window resize), `image` was already overwritten with the new `VkImage` but `imageMemory` remained null. The RAII destructor would later try to destroy the image, but the memory binding was never established — producing validation errors like *"object has not been destroyed"* and a crash.

### The fix
Use local RAII variables; only move into the output parameters after every allocation succeeds:

```cpp
// New, safe:
vk::raii::Image       newImage(device, imageInfo);
vk::raii::DeviceMemory newMemory(device, allocInfo); // if throws, newImage destructs here
newImage.bindMemory(newMemory, 0);

image       = std::move(newImage);   // only now do we touch the output params
imageMemory = std::move(newMemory);
```

If `allocateMemory` throws, the exception propagates, `newImage`'s destructor runs and destroys the `VkImage` cleanly, and the output parameters are left unchanged (still holding whatever they held before). This is the *strong exception guarantee* — either everything succeeds or nothing changes.

### Why `std::move` and not copy
`vk::raii` objects wrap a Vulkan handle and own it. They are *move-only* — copying would mean two objects think they own the same handle and both try to destroy it. `std::move` transfers ownership: `newImage` becomes null, `image` takes the handle.

---

## 3. `run()` — Guaranteeing `cleanup()` Always Fires

### The problem
```cpp
// Old:
initVulkan();
mainLoop();   // if this throws...
cleanup();    // ...this is skipped
```

If anything inside `mainLoop` — such as the OOM exception from a resize — propagated out, `cleanup()` was never called. Vulkan objects owned by member variables would be destroyed in arbitrary order when the stack unwound, causing more validation errors and potential crashes.

### The fix
```cpp
try {
    mainLoop();
} catch (...) {
    cleanup();  // always runs, then rethrows
    throw;
}
cleanup();      // runs on normal exit
```

`catch (...)` catches everything without caring what type it is. We call `cleanup()` to destroy Vulkan objects in the correct order, then `throw;` (no argument) rethrows the original exception so the caller still sees it. This pattern is equivalent to a destructor-based RAII guard but makes the intent explicit.

---

## 4. `pendingPresentMode` Synchronization

### What `pendingPresentMode` is
The renderer exposes a V-Sync toggle in ImGui. `pendingPresentMode` holds the mode the user *wants*. At the end of each frame, `drawFrame` compares `pendingPresentMode` to the current swapchain's mode and calls `recreateSwapChain()` if they differ.

### The bug
`pendingPresentMode` was initialized to `eMailbox` (triple-buffering, no vsync). Many hardware/driver/compositor combinations don't support mailbox and silently fall back to `eFifo` (vsync) when the swapchain is created. The swapchain was created with `eFifo`, but `pendingPresentMode` still said `eMailbox`. Every frame, `drawFrame` saw a mismatch and called `recreateSwapChain()` — infinite loop, crash.

### The fix
After the swapchain is created, sync `pendingPresentMode` from the actual result:

```cpp
swapchain = std::make_unique<Swapchain>(..., pendingPresentMode);
pendingPresentMode = swapchain->getPresentMode(); // sync to what was actually chosen
```

Now the initial state is consistent. The ImGui checkbox still works because changing it updates `pendingPresentMode`, creating a real mismatch that triggers one intentional recreation.

---

## 5. Bindless Texture System

This is the biggest architectural change. Previously, one texture was hard-coded for the whole scene. Now an arbitrary number of textures can coexist, and each draw call picks which one to use.

### Why not one descriptor set per object?
The "traditional" approach is to allocate one `VkDescriptorSet` per renderable, bind the object's texture to binding 1, draw, then bind the next set, draw. This works but has costs:
- `vkCmdBindDescriptorSets` is called once per object per frame.
- More descriptor pool memory.
- The pipeline state changes between draws, reducing GPU batching opportunities.

With bindless, you bind **one** descriptor set for the whole frame. Each draw call communicates its texture choice via a push constant — a handful of bytes pushed directly into the command buffer, not a descriptor bind.

### The descriptor layout

**Binding 0** — Uniform Buffer Object (UBO): view/proj matrices, light data, camera position. One per frame.

**Binding 1** — Texture array. Declared as `MAX_TEXTURES = 64` slots of `COMBINED_IMAGE_SAMPLER`. This is the bindless array. The `PARTIALLY_BOUND` flag allows empty slots.

**Binding 2** — Shadow map. One sampler with comparison (`Sampler2DShadow`), always bound.

```cpp
// The binding flags — only binding 1 needs PARTIALLY_BOUND
std::array<vk::DescriptorBindingFlags, 3> bindingFlags = {
    vk::DescriptorBindingFlags{},                    // binding 0: UBO
    vk::DescriptorBindingFlagBits::ePartiallyBound,  // binding 1: textures
    vk::DescriptorBindingFlags{}                     // binding 2: shadow map
};
```

### The `Texture` struct
Each loaded texture bundles its four Vulkan objects together:

```cpp
struct Texture {
    vk::raii::Image        image;
    vk::raii::DeviceMemory memory;
    vk::raii::ImageView    view;
    vk::raii::Sampler      sampler;
};
```

All textures live in `std::vector<Texture> textures`. A `std::unordered_map<std::string, uint32_t> textureCache` maps file paths to their index, so the same file is never loaded twice.

### `loadTexture(path)`
Replaces the old `createTextureImage` + `createTextureImageView` + `createTextureSampler` trio with a single function:

1. Cache check — return the existing index immediately if already loaded.
2. Load pixels from disk with `stb_image`.
3. Compute mip levels: `floor(log2(max(w, h))) + 1`.
4. Upload via staging buffer → device-local image (`createBuffer` + `createImage`).
5. Transition layout: `Undefined → TransferDst`, copy from staging buffer, generate mipmaps (each level downsamples the previous with a blit — see `generateMipmaps`).
6. Create `ImageView` and `Sampler`.
7. Append to `textures`, register in cache, return the index.

### The "no texture" sentinel
Objects without a texture field in the JSON get `textureIndex = 0xFFFFu`. The fragment shader checks for this value and substitutes `(1,1,1)` for `texColor`, making `baseColor = fragColor * (1,1,1) = fragColor` — the vertex color comes through unmodified. This avoids creating a placeholder texture just to act as a no-op multiplier.

```glsl
float3 texColor = (push.textureIndex == 0xFFFFu)
    ? float3(1.0, 1.0, 1.0)
    : textures[NonUniformResourceIndex(push.textureIndex)].Sample(uv).rgb;
```

### The descriptor pool size
The pool must pre-allocate enough descriptors for all possible slots across all frames in flight:

```cpp
MAX_FRAMES_IN_FLIGHT * (MAX_TEXTURES + 1)
// +1 for the shadow map at binding 2
```

### Writing descriptor sets
`createDescriptorSets` builds a flat `vector<DescriptorImageInfo>` from all currently loaded textures and writes them to binding 1 in a single `vkUpdateDescriptorSets` call. It skips the write entirely if no textures are loaded — Vulkan rejects `descriptorCount = 0`, and `PARTIALLY_BOUND` means not writing binding 1 at all is legal as long as the shader never accesses it.

---

## 6. OBJ Loading

### `std::hash<Vertex>` and `operator==`
OBJ files store positions, normals, and texture coordinates in three separate flat arrays. Each triangle face is a list of index triples `(pos_idx, uv_idx, normal_idx)`. Naively, every index triple becomes a vertex, producing many duplicates where adjacent triangles share a corner.

To deduplicate, `loadOBJ` uses an `unordered_map<Vertex, uint32_t>`. This requires the `Vertex` type to be hashable. The hash is implemented with the *Boost hash combine* idiom:

```cpp
seed ^= hash(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
```

The magic constant is `2^32 / φ` (φ = golden ratio ≈ 1.618). The bit shifts spread each field's contribution across different bit positions so that fields that happen to hash to similar values don't cancel out. Without this mixing, many different vertices could land in the same bucket, degrading the map to O(n) lookups.

### UV Y-flip
OBJ uses a bottom-left UV origin (V=0 at the bottom). Vulkan's `VK_IMAGE_ORIGIN_UPPER_LEFT` means V=0 is at the top. Without correction, textures appear vertically flipped:

```cpp
v.texCoord.y = 1.0f - attrib.texcoords[2 * index.texcoord_index + 1];
```

### Vertex color for OBJ meshes
OBJ has no per-vertex color field. The vertex color is set to white `(1,1,1)` so it doesn't tint the texture. Combined with the texture, the final `baseColor = (1,1,1) * texColor = texColor`.

---

## 7. JSON-Driven Scene Loading

The scene is described in `assets/scenes/scene.json`. Each object entry supports:

| Field | Type | Default | Description |
|---|---|---|---|
| `mesh` | string | required | `"cube"`, `"plane"`, or a path to an `.obj` file |
| `position` | `[x, y, z]` | required | World-space translation |
| `color` | `[r, g, b]` | `[1,1,1]` | Per-vertex color (modulates the texture) |
| `size` | float | `1.0` | Half-extent for cube/plane |
| `texture` | string | none | Path to texture file; omit for vertex-color-only |
| `rotation` | `[rx, ry, rz]` | `[0,0,0]` | XYZ Euler angles in degrees, applied in X→Y→Z order |
| `scale` | float | `1.0` | Uniform scale applied after rotation |

### Model matrix construction
The TRS (Translate-Rotate-Scale) order matters. Transforms applied in `loadScene`:

```
M = T * Rx * Ry * Rz * S
```

Rotations are applied in local space, from the object's own axes outward. This means rotating around X first, then Y, then Z in the *original* coordinate frame — not a world-space orbit. This is *intrinsic* rotation (Tait-Bryan XYZ). For small rotations the order rarely matters visually, but for large rotations the distinction is significant.

The decomposed transform (`position`, `rotationDeg`, `scale`) is stored alongside the baked matrix in `Renderable` so the ImGui sliders can re-derive the matrix at runtime without needing to read the JSON again.

### The CMake `file(COPY)` caveat
Assets are copied from `assets/` to the build directory at *CMake configure time*, not at build time. If you delete a file from `assets/`, it persists in `_build/` until you delete the build directory or manually remove it. This is why a deleted texture still appeared during testing.

---

## 8. Push Constants and the Draw Loop

### What push constants are
Push constants are a small block of data (spec minimum: 128 bytes) embedded directly in the command buffer. They are the fastest way to send per-draw data to shaders — no buffer allocation, no descriptor binding, just a `vkCmdPushConstants` call before each draw.

Previously, push constants only carried the model matrix (64 bytes, vertex stage only). Now they carry:

```cpp
struct PushConstants {
    glm::mat4 model;       // 64 bytes
    uint32_t  textureIndex; // 4 bytes (padded to 16 by the driver)
};
```

The stage flags changed from `eVertex` only to `eVertex | eFragment` because `textureIndex` is consumed by the fragment shader.

### The draw loop
For each `Renderable` in both the shadow pass and the main color pass:

```cpp
PushConstants pc{r.modelMatrix, r.textureIndex};
commandBuffer.pushConstants2(..., sizeof(PushConstants), &pc);
commandBuffer.bindVertexBuffers(0, *r.vertexBuffer, {0});
commandBuffer.bindIndexBuffer(*r.indexBuffer, 0, vk::IndexType::eUint32);
commandBuffer.drawIndexed(r.indexCount, 1, 0, 0, 0);
```

No descriptor set rebind happens between objects — one bind at the start of the pass covers all renderables.

---

## 9. Lighting — Blinn-Phong with Shadow Mapping

### The lighting model
The renderer uses **Blinn-Phong** — a local illumination model. "Local" means each fragment's colour is computed using only information available at that point: its normal, the light direction, and the view direction. It has no knowledge of other geometry in the scene, which is why it needs shadow mapping as a separate pass to approximate shadows.

The final colour is assembled in three components:

**Ambient** — a flat lift applied to everything regardless of lighting. Prevents surfaces facing away from the light from going completely black. Controlled by `ubo.materialParams.x`.

```glsl
ambient_contribution = baseColor * ambient
```

**Diffuse (Lambertian)** — how much light a surface receives depends on the angle between its normal and the light direction. A surface facing directly toward the light receives full illumination; a surface at 90° receives none:

```glsl
float diffuse = max(dot(norm, toLight), 0.0);
```

`max(..., 0)` clamps negative dot products (back-facing surfaces) to zero rather than subtracting light.

**Specular** — the sharp highlight caused by light reflecting off a surface toward the viewer. The reflection direction is computed with GLSL's `reflect()`:

```glsl
float3 reflDir  = reflect(-toLight, norm);
float  specular = pow(max(dot(viewDir, reflDir), 0.0), shininess) * specStrength;
```

`shininess` controls the spread of the highlight. Low values (2–8) give broad, matte-like highlights. High values (64–256) give tight, mirror-like highlights. It is the exponent in the power function: the higher it is, the faster the highlight falls off as the reflection vector diverges from the view direction.

The specular term is only computed when `diffuse > 0` — a fragment facing away from the light cannot have a specular highlight.

### Assembling the final colour
```glsl
float3 baseColor = fragColor * texColor;
float3 color = baseColor * (ambient + (1.0 - ambient) * diffuse * shadow)
             + specular * shadow;
```

`shadow` is a value in [0, 1] from the shadow map (0 = fully in shadow, 1 = fully lit). Shadows suppress both the diffuse and specular terms but not the ambient — ambient is meant to represent indirect light bouncing from everywhere, which shadows don't occlude.

### Shadow mapping
A separate render pass runs before the main pass. It renders the scene from the light's point of view into a depth-only image (`shadowMap`). The light uses an **orthographic** projection (parallel rays, no perspective — appropriate for a distant directional light).

In the main pass, each fragment's world position is re-projected into the light's clip space (`fragPosLightSpace`). The projected depth is compared against the stored shadow map depth using `SampleCmp`, which performs hardware-accelerated PCF (Percentage Closer Filtering) on most GPUs. The 3×3 kernel in the shader samples 9 neighbouring texels and averages the results, giving soft shadow edges:

```glsl
for (int x = -1; x <= 1; ++x)
    for (int y = -1; y <= 1; ++y)
        shadow += shadowMap.SampleCmp(shadowUV + float2(x,y) * texelSize, currentDepth);
shadow /= 9.0;
```

### What the lighting model is missing

**PBR (Physically Based Rendering)** — Blinn-Phong is an approximation that doesn't obey energy conservation. A mirror-like surface can reflect more light than it receives. PBR models replace ambient/diffuse/specular with a physically grounded BRDF (Bidirectional Reflectance Distribution Function), typically with roughness and metallic parameters. PBR requires either image-based lighting (IBL) or analytical light models, and usually at least two more textures per material (roughness map, metallic map).

**Indirect lighting / GI** — In reality, light bounces off surfaces and illuminates other surfaces. The flat ambient term is a crude approximation of this. SSAO approximates one part of it (ambient occlusion — nearby geometry blocking indirect light). Full global illumination requires ray tracing or techniques like voxel GI, light probes, or radiosity.

*Normal mapping, specular maps, point lights, ACES tonemapping, and gamma correction have since been implemented — see sections 11–15.*

---

## 10. Camera — Free-Fly Controls

### Coordinate system
The world uses **Z-up**. X and Y are the horizontal plane; Z is height. The camera's `up` vector is always `(0, 0, 1)`. This differs from many OpenGL tutorials that use Y-up, and the difference propagates into the cross product math.

### Spherical coordinates for orientation
The camera direction is described by two angles:

- **Yaw** — horizontal rotation in the XY plane (0° = facing +X, 90° = facing +Y)
- **Pitch** — elevation angle above the XY plane (0° = horizontal, 90° = straight up)

The forward direction vector is derived from these:

```cpp
glm::vec3 Camera::forward() const {
    float y = glm::radians(yaw);
    float p = glm::radians(pitch);
    return normalize(vec3{
        cos(y) * cos(p),   // x component
        sin(y) * cos(p),   // y component
        sin(p)             // z component (height)
    });
}
```

`cos(pitch)` projects the direction onto the horizontal plane. At pitch=0, the full length is horizontal. At pitch=±89°, `cos(89°) ≈ 0.017`, so nearly all length is vertical. Pitch is clamped to ±89° to prevent gimbal lock at the poles (looking straight up or down causes the right vector to become degenerate).

### The view matrix
```cpp
return glm::lookAt(position, position + forward(), vec3(0,0,1));
```

`lookAt` builds a matrix that transforms world-space coordinates into camera-space (a basis where the camera is at the origin facing -Z). The third argument is the world-up hint — used internally to compute the right vector of the camera's basis.

### The right vector and Z-up handedness
To move sideways, we need the camera's right vector:

```cpp
glm::vec3 right = normalize(cross(vec3(0,0,1), forward()));
```

This order matters. `cross(A, B)` follows the right-hand rule: curl fingers from A toward B, the thumb points along the result. In Z-up:

- `cross(forward, up)` — curl from forward to up — gives the *left* vector
- `cross(up, forward)` — curl from up to forward — gives the *right* vector

This is the opposite of Y-up cameras (where `cross(forward, up)` gives right). The Y-up convention appears in most OpenGL tutorials, but it produces a mirrored result in Z-up.

Mouse horizontal movement decreases yaw (not increases) for the same reason: in Z-up, increasing yaw rotates counterclockwise in the XY plane when viewed from above, which is a left turn from the camera's perspective.

### Fly mode activation
Camera mode activates by holding the **right mouse button** when the cursor is not over an ImGui widget (`!WantCaptureMouse`). On activation:
- `glfwSetInputMode(GLFW_CURSOR_DISABLED)` hides and locks the cursor to the window, enabling unlimited raw mouse movement.
- The previous mouse position is reset to the current position to prevent a jump delta on the first frame of camera mode.

On release (button up or ImGui recaptures):
- `GLFW_CURSOR_NORMAL` restores the cursor.

### Delta time
The main loop now tracks wall-clock time between frames:

```cpp
auto lastTime = std::chrono::steady_clock::now();
// per frame:
float dt = std::chrono::duration<float>(now - lastTime).count();
```

Movement speed is multiplied by `dt`, so the camera moves at a consistent world-space speed regardless of frame rate. Without delta-time, a camera moving at 60 FPS would travel twice as fast as at 30 FPS.

### Controls summary

| Input | Action |
|---|---|
| Hold RMB | Enter fly mode (cursor hidden) |
| W / S | Move forward / backward |
| A / D | Strafe left / right |
| Q / E | Move down / up (world Z) |
| Mouse X | Yaw (look left/right) |
| Mouse Y | Pitch (look up/down) |

### Per-object transform editing
Each `Renderable` stores its decomposed transform alongside the baked `modelMatrix`:

```cpp
std::string label;
glm::vec3   position;
glm::vec3   rotationDeg; // XYZ Euler, degrees
float       scale;
```

The ImGui "Objects" panel exposes `DragFloat3` widgets for position and rotation, and a `DragFloat` for scale. On any change:

```cpp
r.modelMatrix = buildModelMatrix(r.position, r.rotationDeg, r.scale);
```

`buildModelMatrix` applies the same T * Rx * Ry * Rz * S order as `loadScene`, keeping the runtime edits consistent with what was loaded from JSON. `DragFloat` lets you click-and-drag to scrub a value continuously, or Ctrl+click to type an exact number.

---

## 11. ACES Filmic Tonemapping and Gamma Correction

### The problem with linear HDR output
Lighting math happens in linear light space — doubling the light intensity doubles the value. But display hardware expects values in the [0, 1] range, and without tonemapping, any fragment brighter than 1.0 just clips to white. A sun, a specular highlight, and a bright sky would all look identical.

### The ACES approximation
The renderer uses the Krzysztof Narkowicz ACES filmic curve:

```glsl
float3 ACESFilmic(float3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
```

This is a rational approximation of the Academy Color Encoding System curve. It has a gentle toe (preserving shadow detail) and a compressed shoulder (preventing highlight clipping). It maps any positive HDR value into [0, 1].

### Gamma correction
The render target is declared as `eR8G8B8A8Srgb`. When writing to an sRGB image, the hardware automatically applies the sRGB transfer function (approximately γ ≈ 2.2) to the linear values. No manual `pow(color, 1/2.2)` is needed — and doing it manually would double-apply the curve.

### Exposure and the disable sentinel
A global `exposure` scalar multiplies the linear HDR color before the ACES curve. Values above 1 brighten the image; values below 1 darken it. To disable tonemapping entirely, `exposure` is passed as 0.0 into the UBO's `materialParams.w`. The shader treats 0 as a sentinel meaning "clamp only, skip ACES" — this avoids a separate boolean in the UBO.

---

## 12. Specular Maps

### What they add
The global `specStrength` slider applies uniform shininess to every pixel of an object. A specular map replaces this with a per-texel value — a rock texture can have bright highlights on wet stone and no highlight on dry stone within the same surface.

The specular map stores `rgb` rather than a single scalar. This allows *tinted* specular highlights: gold surfaces reflect warm light, copper reflects reddish light. The shader multiplies the specular factor by the map color rather than a scalar:

```glsl
float3 specMapColor = (push.specularMapIndex == 0xFFFFu)
    ? float3(specStrength, specStrength, specStrength)
    : textures[NonUniformResourceIndex(push.specularMapIndex)].Sample(uv).rgb;

float3 specular = specFactor * specMapColor;
```

The same `0xFFFFu` sentinel pattern used for albedo textures works here. No specular map → falls back to the uniform slider.

---

## 13. Normal Mapping

### Why per-vertex normals aren't enough
The lighting model uses the surface normal to compute diffuse and specular terms. A flat polygon has one normal interpolated smoothly across it — the surface looks smooth. A normal map stores a different normal at every texel, encoding microscopic bumps without adding geometry.

### Tangent space
Normal maps store directions in *tangent space* — a per-surface coordinate frame where Z is the geometric normal, X is the tangent (direction of increasing U in texture space), and Y is the bitangent (cross(N, T)). This makes normal maps reusable across differently-oriented surfaces.

### Vertex tangent storage
Each vertex now carries `glm::vec4 tangent`. The `xyz` components are the tangent direction. The `w` component stores a sign (+1 or -1) for the bitangent:

```cpp
glm::vec3 B = cross(N, T) * tangent.w;
```

The sign handles mirrored UV islands — where the texture is flipped horizontally, `cross(N, T)` points the wrong way, and `w = -1` corrects it without storing the full bitangent.

### Tangent generation
**Procedural meshes (cube, plane):** tangents are hardcoded analytically per face. The +Z face of the cube has U increasing along +X, so `tangent = (1, 0, 0, 1)`.

**OBJ meshes:** tangents are computed from UV deltas per triangle, accumulated into vertices, then orthogonalised:

```
For each triangle:
    edge1 = v1.pos - v0.pos
    edge2 = v2.pos - v0.pos
    dUV1 = v1.uv - v0.uv
    dUV2 = v2.uv - v0.uv
    inv = 1 / (dUV1.x * dUV2.y - dUV2.x * dUV1.y)
    tangent = inv * (dUV2.y * edge1 - dUV1.y * edge2)
    // accumulate into all three vertices

Per vertex (Gram-Schmidt):
    ortho = normalize(t - dot(t, n) * n)
    sign = (dot(cross(n, ortho), t) >= 0) ? 1 : -1
    vertex.tangent = vec4(ortho, sign)
```

### In the shader
The vertex shader builds the TBN matrix and passes world-space T, B, N to the fragment shader. The fragment shader uses it to transform the tangent-space normal from the map into world space:

```glsl
float3x3 TBN = float3x3(fragTangent, fragBitangent, fragNormal);
float3 sampledNormal = textures[normalIdx].Sample(uv).rgb * 2.0 - 1.0;
norm = normalize(mul(transpose(TBN), sampledNormal));
```

`mul(transpose(TBN), v)` transforms from tangent space to world space (TBN is orthogonal so its transpose is its inverse).

---

## 14. Shadow Map Slope-Scale Bias

### The problem: shadow acne
The shadow map stores depth at the light's resolution. When a surface samples its own shadow, floating-point precision means the surface reads itself as "in shadow" — producing fine-grained noise (acne) across lit surfaces.

### The fix: slope-scale bias
Rather than a fixed depth offset, the bias scales with the surface angle relative to the light. Surfaces nearly parallel to the light direction (low NdotL) have the highest acne risk and receive the most bias:

```glsl
float NdotL_geo  = max(dot(normalize(fragNormal), normalize(lightDir)), 0.0);
float shadowBias = lerp(ubo.shadowParams.x, ubo.shadowParams.y, NdotL_geo);
float biasedDepth = currentDepth - shadowBias;
```

`shadowParams.x` (biasMin) applies when the face is squarely lit — little bias needed. `shadowParams.y` (biasMax) applies at grazing angles. Both are exposed as ImGui sliders.

### Peter panning
Too much bias pushes the shadow off the caster. A front face of the cube shows a visibly inset shadow rectangle. The solution is to tune biasMin down until the shadow hugs the caster, then tune biasMax up just enough to suppress acne at edges. The live sliders make this iterative process practical.

The geometric normal (not the normal-mapped one) is used for the bias calculation — the shadow map was rendered with actual geometry, not normal-mapped geometry.

---

## 15. Point Lights

### Motivation
A single directional light produces flat, uniformly lit scenes. Point lights add fill, rim, and accent lighting that suggests multiple light sources and gives scenes visual depth.

### No shadow casting
Each omnidirectional shadow-casting point light would require rendering the scene 6 times into a cubemap (one per face: ±X, ±Y, ±Z). With 4 point lights that is 24 extra passes per frame. Instead, point lights are treated as fill lights that represent soft indirect illumination — in real scenes, fill lights approximate bounced light, which inherently wraps around geometry without hard shadows.

### Falloff model
The renderer uses a polynomial windowed falloff rather than inverse-square:

```glsl
float t   = clamp(dist / radius, 0.0, 1.0);
float att = (1.0 - t * t) * (1.0 - t * t) * intensity;
```

This is `(1 - t²)²`. At `dist=0`, `att = intensity`. At `dist=radius`, `att = 0` exactly — no discontinuity, no infinite tail. This is the approach used in Unreal Engine (Epic's spherical area light falloff). It trades physical accuracy for artist control: `intensity` and `radius` are intuitive dials.

### Data layout
Point lights are packed into the UBO as two `vec4[4]` arrays:
- `pointLightPos[i]` — `xyz` = world position, `w` = intensity
- `pointLightColor[i]` — `xyz` = color, `w` = radius

Only *active* lights are packed (disabled lights are skipped). `lightCounts.x` tells the shader how many to iterate over.

### Dynamic management
Point lights are stored in a `std::vector<PointLightData>` (up to `MAX_POINT_LIGHTS = 4`). The ImGui panel provides Add Light / Remove buttons per light, live position/color/intensity/radius controls, and an enabled toggle.

---

## 16. Parallax Occlusion Mapping (POM)

### What normal mapping cannot do
Normal mapping perturbs the lighting normal, creating the illusion of surface bumps. But the geometry is unchanged — the silhouette of the object remains flat. Looking at a brick wall at a shallow angle reveals perfectly flat geometry regardless of how detailed the normal map is.

POM goes further: the UV coordinates used for all texture samples are displaced based on the view direction and a height map. This creates genuine apparent depth within a polygon — bricks, cobblestone, and rock faces appear to have real relief.

### Algorithm overview
1. Convert the view direction (surface→camera) from world space into tangent space using the TBN matrix. The TBN rows are T, B, N, so `mul(TBN, viewWorld)` gives the tangent-space vector.
2. Project the tangent-space view direction onto the UV plane: `uvStep = -(viewTS.xy / viewTS.z) * depthScale / numSteps`. The negative sign is because a depressed area appears shifted opposite to the view direction.
3. **Linear search:** Start at `currentDepth = 0`, `currentUV = uv`. At each step, advance UV by `uvStep` and depth by `1/numSteps`. Stop when `currentDepth >= 1 - heightSample` — the ray has gone below the height field.
4. **Binary refinement:** 5 bisection steps between the last two linear samples to sharpen the intersection without a per-pixel loop.
5. Use the final displaced UV for all subsequent texture samples (albedo, normal, specular) so they all show the same surface point.

Height map convention: white (1.0) = raised, black (0.0) = maximally recessed. The condition `currentDepth < 1.0 - h` means: continue while the ray is still above the height field surface.

### Adaptive step count
```glsl
float numSteps = lerp(maxSteps, minSteps, saturate(viewTS.z));
```
`viewTS.z` ≈ cos(angle-with-normal). Looking straight at the surface (z ≈ 1) needs few steps; grazing angles (z ≈ 0) need many to avoid ray-skipping artifacts. This keeps cost low for head-on views and quality high at edges.

### Limitations
POM is a screen-space effect — it only displaces texture coordinates, not actual vertices. Silhouettes remain the polygon boundary. This is visible on the cube's edges when viewed at an angle. True geometry displacement requires tessellation shaders.

### Activation
Per-object: add `"heightMap": "textures/rock_height.jpg"` to the scene JSON entry. `heightMapIndex == 0xFFFFu` skips the POM block entirely — no cost on objects without a height map.

---

## 17. Procedural Skybox

### Motivation
Without a skybox, empty pixels show the clear color (black). This makes the scene look like it is floating in a void rather than in an environment.

### Implementation: fullscreen triangle
The skybox is a fullscreen triangle rendered after all scene geometry, in the same render pass. A fullscreen triangle is generated from `SV_VertexID` without a vertex buffer:

```
vertex 0: NDC (-1, -1)
vertex 1: NDC ( 3, -1)
vertex 2: NDC (-1,  3)
```

These three vertices, clipped to the viewport, cover the entire screen. The vertex is emitted at `z = w = 1.0`, so after perspective division `z/w = 1.0` — it sits at the far plane. With `depthCompareOp = eLessOrEqual` and `depthWriteEnable = false`, the sky only fills pixels where the depth buffer still holds its clear value of 1.0 (no geometry was drawn there).

### View ray reconstruction
The sky fragment needs to know which direction in world space each screen pixel points. This is done by storing two extra matrices in the UBO:

- `invProj` — inverse of the projection matrix
- `invViewRot` — inverse of the view rotation (translation zeroed out, then `transpose()` since rotation matrices are orthogonal)

In the fragment shader:

```glsl
float4 vsPos = mul(invProj, float4(ndc, 1.0, 1.0));
vsPos /= vsPos.w;  // perspective divide → view-space direction
float3 dir = normalize(mul(invViewRot, float4(vsPos.xyz, 0.0)).xyz);
```

### Sky gradient and sun
In Z-up world space, `dir.z` is the elevation: +1 is straight up, -1 is straight down. The fragment color blends between three zones:

- `elev < 0` → lerp(horizon, ground, `-elev * 4`)
- `elev >= 0` → lerp(horizon, zenith, `elev * 1.5`)

A sun disk is rendered by comparing the ray direction against the directional light direction:

```glsl
float cosAng = dot(dir, sunDir);
if (cosAng > sunEdge) {
    float blend = ((cosAng - sunEdge) / (1 - sunEdge))²;
    color = lerp(color, sunColor, blend);
}
```

`sunEdge = cos(half-angle)`. A value of 0.9998 ≈ cos(1.1°) gives a realistic but visible sun disk. All colors are configurable in ImGui and scene JSON.

### Separate pipeline
The sky uses its own `vk::raii::Pipeline`, `vk::raii::PipelineLayout`, `vk::raii::DescriptorSetLayout`, and descriptor pool. Its descriptor layout has only binding 0 (the UBO). Sky push constants carry the four color vectors (64 bytes, fragment stage only). This keeps the sky completely decoupled from the scene pipeline.

---

---

## 18. Exponential Height Fog

### Motivation
A flat ambient term makes distant objects look identical to nearby ones. Real atmosphere scatters light: distant objects lose contrast and shift toward a horizon color. Fog approximates this without ray marching through a participating-medium volume.

### Formula
```glsl
float dist       = length(cameraPos - worldPos);
float heightAtt  = exp(-fogHeightFalloff * max(worldPos.z, 0.0));
float fogFactor  = 1.0 - exp(-fogDensity * dist * heightAtt);
fogFactor        = clamp(fogFactor, 0.0, fogMaxOpacity);
color            = lerp(color, fogColor, fogFactor);
```

The inner exponential `exp(-density * dist)` gives standard *exponential fog* — density decays the transmittance exponentially with distance. The outer `exp(-heightFalloff * z)` multiplies in a *height falloff* term: fog is denser near the ground and thins out with altitude. At `z = 0` the falloff is 1 (full density); at large `z` it approaches 0 (no fog). `maxOpacity` caps how dark fog can make a fragment — a cap of 0.9 keeps distant silhouettes faintly visible rather than completely vanishing.

The operation is applied in linear space before tonemapping. Applying it after tonemapping would be incorrect — the fog color would be in a different perceptual space than the scene color.

### Sky color synchronisation
When `fogSyncSky = true`, the fog color is overwritten each frame from `skyPush.horizonColor` in `updateUniformBuffer`. This ensures the horizon seamlessly blends from the scene geometry into the sky gradient without a visible color seam at the geometry/sky boundary.

### UBO layout
```cpp
glm::vec4 fogParams;  // x = density, y = heightFalloff, z = maxOpacity
glm::vec4 fogColor;
```

Both `scene.slang` and `sky.slang` declare these fields in their UBO structs, even though the sky shader ignores them. The byte layout of the C++ `UniformBufferObject` must match every shader that binds to the same UBO descriptor — adding a field in one shader but not the others shifts all subsequent fields and silently corrupts lighting data.

### Controls
Four ImGui sliders: density (0.0–0.1), height falloff (0.0–2.0), max opacity (0.0–1.0), and a fog color picker. A "Sync to sky horizon" checkbox drives `fogSyncSky`.

---

## 19. Sphere Mesh Generation

### Why procedural geometry
glTF and OBJ can load arbitrary meshes, but having procedurally generated primitives directly in code is useful for visualising lights, placement helpers, and quick prototyping. A sphere is the canonical smooth body.

### UV sphere parameterisation (Z-up)
The sphere is parameterised by two angles:
- `phi ∈ [0, π]` — polar angle from +Z (north pole) to -Z (south pole)
- `theta ∈ [0, 2π]` — azimuthal angle around the Z axis

```
pos    = r * (sin(phi)*cos(theta),  sin(phi)*sin(theta),  cos(phi))
normal = (sin(phi)*cos(theta),  sin(phi)*sin(theta),  cos(phi))   // same, normalised
```

The normal is outward-pointing and equals the normalised position because a sphere centered at the origin has the property that the surface normal at any point is exactly the position vector divided by radius.

The tangent follows increasing longitude (increasing `theta`):
```
tangent = (-sin(theta),  cos(theta),  0,  1.0)
```

This is the derivative of `pos` with respect to `theta`, normalised. It always points along the parallel circle at that latitude. `w = +1` because the UV parameterisation is non-mirrored.

### Degenerate pole triangles
The grid produces `stacks × sectors` quads. At the north and south poles, the top edge of each quad degenerates to a single point (all vertices on the pole share the same position). The degenerate half of each pole quad — the triangle that would have zero area — is skipped:

```cpp
if (i != 0)          // skip degenerate top triangle at north pole
    indices.push_back({v00, v10, v11});
if (i != stacks - 1) // skip degenerate bottom triangle at south pole
    indices.push_back({v00, v11, v01});
```

Without this, the pole triangles still render (zero-area triangles produce no fragments), but they add unnecessary index count and confuse tangent generation.

### Scene JSON usage
```json
{ "mesh": "sphere", "size": 1.0, "sectors": 32, "stacks": 16, "position": [0,0,2] }
```
`size` is the radius. `sectors` is the longitudinal subdivision count (quality around the equator); `stacks` is the latitudinal count (quality from pole to pole). Both default to reasonable values if omitted.

---

## 20. glTF 2.0 Scene Loading

### What glTF is
glTF ("GL Transmission Format") is a JSON + binary format for 3D scenes. The JSON describes a **scene graph** — a tree of nodes, each with a local transform and an optional mesh reference. Meshes contain one or more **primitives**, each of which is one draw call with named vertex attribute accessors and a material. Binary geometry and embedded images live in a `.bin` file (for `.gltf`) or in the binary chunk of a self-contained `.glb` file.

The key difference from OBJ is that glTF carries the full scene hierarchy (node transforms, multiple materials, PBR parameters) rather than just geometry and a flat material list.

### fastgltf
Rather than parsing JSON and binary manually, the renderer uses **fastgltf** (added as a git submodule at `extern/fastgltf`). fastgltf validates the file, resolves accessors, and surfaces typed C++ structs: `asset.nodes`, `asset.meshes`, `asset.materials`, `asset.accessors`, `asset.bufferViews`, `asset.buffers`, `asset.images`. All JSON decoding and binary buffering is handled before any renderer code runs.

### Node tree traversal
`loadGLTF` starts from the default scene's root nodes and recurses with an accumulated world transform:

```cpp
void visitNode(asset, nodeIndex, parentTransform, baseDir, out, yUpToZUp):
    worldTransform = parentTransform * nodeTransform(node)
    if node.meshIndex:
        for each primitive in asset.meshes[node.meshIndex]:
            emit GltfPrimitive(primitive, worldTransform, ...)
    for child in node.children:
        visitNode(asset, child, worldTransform, ...)
```

A node's local transform is stored as either a pre-baked `float4x4` or a TRS triple (translation + rotation quaternion + scale). Both cases are handled:

```cpp
std::visit(fastgltf::visitor{
    [](const fmat4x4& m)  { return glm::make_mat4(m.data()); },
    [](const TRS& trs)    { return T * mat4_cast(R) * S; }
}, node.transform);
```

### Coordinate system conversion
glTF is Y-up, right-handed. The renderer is Z-up. When `"yUpToZUp": true` in scene.json, a root transform matrix is injected as the initial `parentTransform`:

```
M_yup_to_zup = | 1   0   0   0 |
               | 0   0  -1   0 |
               | 0   1   0   0 |
               | 0   0   0   1 |
```

This rotates 90° around X: Y (up in glTF) becomes Z (up in world), Z (depth in glTF) becomes -Y. The determinant is +1, so it is a proper rotation — winding order and handedness are preserved. Injecting it as the root transform means all node-local transforms automatically inherit the correction.

### GltfPrimitive and GltfImage
Each primitive is returned as:

```cpp
struct GltfImage {
    std::string          path;   // non-empty = external file
    std::vector<uint8_t> bytes;  // non-empty = GLB-embedded binary
};

struct GltfPrimitive {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    glm::mat4             transform;
    GltfImage             baseColor;
    GltfImage             normalMap;
    GltfImage             metallicRoughness;
};
```

This separation keeps `Scene.cpp` pure geometry code — it never touches Vulkan. `VulkanRenderer` receives the flat list and calls `loadTexture` or `loadTextureFromMemory` on each slot, producing bindless indices.

### GLB embedded images
GLB files embed images in the binary chunk. fastgltf surfaces them as `sources::BufferView` (an index into the buffer data), not as `sources::URI`. The resolver handles both:

```cpp
if (auto* uri = get_if<sources::URI>(&img.data))
    return { baseDir + "/" + uri->uri.path(), {} };

if (auto* bv = get_if<sources::BufferView>(&img.data)) {
    auto& bufView = asset.bufferViews[bv->bufferViewIndex];
    auto& buf     = asset.buffers[bufView.bufferIndex];
    if (auto* arr = get_if<sources::Array>(&buf.data)) {
        const uint8_t* start = reinterpret_cast<const uint8_t*>(arr->bytes.data())
                             + bufView.byteOffset;
        return { {}, vector<uint8_t>(start, start + bufView.byteLength) };
    }
}
```

### sRGB vs linear texture formats
All textures were previously uploaded as `eR8G8B8A8Srgb`. This is correct for albedo/diffuse (the GPU gamma-decodes on sample, matching the sRGB storage), but wrong for normal maps and metallic-roughness maps, which store linear data. Loading a normal map as sRGB gamma-decodes the direction components, producing subtly wrong normals — visible as split lighting on curved surfaces (e.g. Sponza arches).

The fix: `loadTexture` and `loadTextureFromMemory` now accept `bool linearFormat`. Normal maps are loaded with `eR8G8B8A8Unorm`; albedo textures use `eR8G8B8A8Srgb`:

```cpp
r.textureIndex   = resolveSlot(prim.baseColor, false); // sRGB — colour data
r.normalMapIndex = resolveSlot(prim.normalMap,  true);  // linear — direction data
```

Metallic-roughness is currently unused (set to `0xFFFFu`) until PBR shading is implemented. Feeding it into the Blinn-Phong specular slot causes green glow because the roughness channel (stored in green) dominates.

### Scene JSON usage
```json
{
    "mesh":      "models/sponza.glb",
    "yUpToZUp":  true,
    "position":  [0.0, 0.0, 0.0],
    "scale":     0.01
}
```

Note: `"scale"` is the transform scale for `buildModelMatrix`. `"size"` is only used for procedural meshes (sphere radius, cube/plane half-extent). They are separate fields.

### Configurable shadow frustum
The directional shadow map uses an orthographic projection. The previous fixed `±10` extent was sufficient for the small test scene but clips Sponza. Three new tuneable members control the frustum:

```cpp
float shadowOrthoSize = 20.0f;  // half-extent in X and Y
float shadowNear      = 0.1f;
float shadowFar       = 100.0f;
```

All three are exposed as ImGui sliders in the shadow section, allowing interactive tuning without recompilation.

### Bindless array size
`MAX_TEXTURES` was raised from 64 to 512 to accommodate Sponza (~130 unique textures across ~25 material groups). The descriptor pool size scales with `MAX_TEXTURES`.

---

---

## 21. CMakePresets and Build System Consolidation

### The problem with the old `run.sh`
The original `run.sh` was an imperative shell script that hard-coded the configure and build commands:

```bash
cmake -S . -B _build -DCMAKE_BUILD_TYPE=Debug
cmake --build _build -- -j$(nproc)
./_build/main
```

This worked but had several problems:
- The build type and flags were duplicated between the script and any IDE configuration.
- Adding a second configuration (e.g. `release`) meant duplicating the whole script or parsing arguments manually.
- CMake Tools in VSCode (and other IDE integrations) cannot discover configurations from a shell script — they need `CMakePresets.json`.

### `CMakePresets.json`
CMake 3.19+ supports a declarative preset file. The renderer now ships `CMakePresets.json` at the repo root with three sections:

**`configurePresets`** — each preset describes `cmake -S . -B _build -DCMAKE_BUILD_TYPE=...`. A hidden `"base"` preset holds the shared settings (`generator`, `binaryDir`) that all three configurations inherit:

```json
{
  "name": "base",
  "hidden": true,
  "generator": "Unix Makefiles",
  "binaryDir": "${sourceDir}/_build"
}
```

`"debug"`, `"release"`, and `"relwithdebinfo"` each inherit `"base"` and only override `CMAKE_BUILD_TYPE`. The `inherits` mechanism works exactly like class inheritance: the child gets everything from the parent and can override individual fields.

**`buildPresets`** — each build preset references its matching configure preset by name. CMake automatically configures first if the build directory doesn't exist yet.

**`workflowPresets`** — a workflow chains a configure step and a build step into a single command:

```bash
cmake --workflow --preset debug
```

This is equivalent to `cmake -S . -B _build -DCMAKE_BUILD_TYPE=Debug && cmake --build _build`, but the parameters live in version-controlled JSON rather than in a script that might get out of sync.

### Updated `run.sh`
The script became a two-line wrapper:

```bash
cmake --workflow --preset "${1:-debug}"
./_build/main
```

`${1:-debug}` uses the first argument if provided, otherwise defaults to `debug`. This keeps the script as the convenient one-command entry point while delegating all the build logic to CMake.

### VSCode CMake Tools integration
Once `CMakePresets.json` is present, the CMake Tools extension reads it automatically. The workflow preset names appear in the status bar dropdown (bottom of the window). Selecting a preset and clicking the build button runs the same `cmake --workflow` command without opening a terminal.

---

## 22. ASSET_DIR — Platform-Agnostic Asset Path Resolution

### The problem
The shaders, scenes, and textures all live under `assets/` in the source tree. The binary is built into `_build/`. CMakeLists.txt already has:

```cmake
file(COPY assets/ DESTINATION "${CMAKE_BINARY_DIR}/assets")
```

...which copies assets to `_build/assets/` at configure time. But `stbi_load("assets/textures/rock.jpg")` resolves relative to the *current working directory at runtime*, not the binary's location. Running `./_build/main` from the project root happens to work. Running it from inside `_build/` (`./main`) does not — the `assets/` directory is not in `_build/assets` from `_build/`'s perspective... wait, actually it is, but the prefix `assets/` is still relative to wherever the shell is. Any other working directory breaks it.

### The fix: bake the asset directory at compile time
`CMakeLists.txt` now passes the build directory as a preprocessor macro:

```cmake
target_compile_definitions(main PRIVATE
    ASSET_DIR="${CMAKE_BINARY_DIR}"
)
```

`${CMAKE_BINARY_DIR}` is the absolute path to `_build/`. It is expanded at configure time (when you run `cmake --workflow`), so the resulting string is something like `/home/user/blaz-vulkan/_build`. This absolute path is embedded directly into the compiled binary as a string literal — the binary carries the knowledge of where its assets live.

In `VulkanRenderer.cpp`, `run()` reads this macro:

```cpp
basePath_ = std::filesystem::path(ASSET_DIR);
```

Every asset access prepends `basePath_`:

```cpp
// readFile (shaders):
std::ifstream file(basePath_ / path, std::ios::ate | std::ios::binary);

// loadTexture (images):
stbi_load((basePath_ / path).string().c_str(), ...);

// loadScene (JSON, OBJ, GLB):
auto assetPath = [&](const std::string& rel) { return basePath_ / rel; };
```

The `/` operator on `std::filesystem::path` is path concatenation. `basePath_ / "assets/textures/rock.jpg"` produces `/home/user/blaz-vulkan/_build/assets/textures/rock.jpg` — an absolute path that works regardless of working directory.

### Why `std::filesystem::path` instead of `std::string`
`std::string` concatenation of paths is error-prone: you must manually handle trailing slashes, directory separators (`/` vs `\`), and relative vs absolute paths. `std::filesystem::path` handles all of this:
- `path / "subdir" / "file.ext"` always inserts the correct separator.
- `.string()` converts back to a `std::string` (needed for C APIs like `stbi_load`).
- `.parent_path()`, `.filename()`, `.extension()` decompose paths without string parsing.

The `loadTexture` and `loadTextureFromMemory` signatures changed from `const std::string&` to `const std::filesystem::path&`. The `textureCache` keys were changed from a raw string to `path.string()` to ensure the cache hit logic compares canonical strings.

### The `assetPath` lambda in `loadScene`
`loadScene` reads JSON that contains relative paths like `"textures/rock.jpg"`. Rather than prepending `basePath_` at every use site, a local lambda wraps the conversion:

```cpp
auto assetPath = [&](const std::string& rel) { return basePath_ / rel; };

// Usage:
r.textureIndex = loadTexture(assetPath(texturePath));
Scene::loadGLTF(assetPath(meshPath), ...);
```

This makes the intent clear at each call site and keeps the path construction in one place.

---

## 23. Code Coherence Improvements

After the main features were in place, several cross-cutting inconsistencies were addressed. None of these change behavior — they improve readability and make the codebase internally consistent.

### `std::filesystem::path` migration
All file-related function parameters and local variables were migrated from `std::string` to `std::filesystem::path`:

- `VulkanRenderer::readFile(const std::filesystem::path& path)` — was `const std::string&`
- `VulkanRenderer::loadTexture(const std::filesystem::path& path, bool linearFormat)` — was `const std::string&`
- `Scene::loadOBJ(const std::filesystem::path& path, bool yUpToZUp)` — was `const std::string&`
- `Scene::loadGLTF(const std::filesystem::path& path, bool yUpToZUp)` — was `const std::string&`

Inside `loadGLTF`, the `baseDir` variable (used to resolve external texture URIs) changed from `std::string` to `std::filesystem::path`:

```cpp
// Old: string concatenation
std::string texPath = baseDir + "/" + uri->uri.path();

// New: path concatenation
std::filesystem::path texPath = baseDir / uri->uri.path();
```

### Error message normalisation
Error messages were a mix of capitalisation and tone styles:
- `"Failed to open file!"` (capital F, exclamation)
- `"loadTextureFromMemory: stbi decode failed"` (function prefix, no punctuation)
- `"Device not suitable"` (no context)

All error messages were unified to lowercase `"failed to <verb> <object>'"` style:

```cpp
// Before:
throw std::runtime_error("Failed to open file!");
throw std::runtime_error("loadTextureFromMemory: stbi decode failed");

// After:
throw std::runtime_error("failed to open file '" + path.string() + "'");
throw std::runtime_error("failed to decode texture from memory");
```

Including the failing filename in the message (where available) makes debugging faster — you see immediately which asset caused the problem instead of having to add a breakpoint.

### `printf` / `snprintf` → `std::cout` / `std::string`
Two uses of C-style formatted output were replaced:

```cpp
// Before: C stdio
printf("Loaded %zu unique textures.\n", textures.size());

// After: C++ streams
std::cout << "Loaded " << textures.size() << " unique textures\n";
```

```cpp
// Before: char label[32] + snprintf
char label[32];
snprintf(label, sizeof(label), "Light %d", i);
ImGui::TreeNode(label);

// After: std::string
std::string label = "Light " + std::to_string(i);
ImGui::TreeNode(label.c_str());
```

The renderer already uses `std::cout` everywhere else. Mixing `printf` in one or two places creates visual noise. The `snprintf` case was also fragile — a very long label could silently truncate.

### Removed redundant `assert` before `throw`
One location had:

```cpp
assert(result == vk::Result::eTimeout || result == vk::Result::eSuccess);
throw std::runtime_error("vkWaitForFences timed out");
```

The `assert` fires only in debug builds and is disabled in release. The `throw` on the next line always fires. The two lines contradict each other (assert says "this is impossible", throw says "this happened"). The assert was removed; the throw remains.

### `std::invalid_argument` → `std::runtime_error`
One error path used `std::invalid_argument` for what is actually a runtime condition (an unsupported mesh type read from JSON). `std::invalid_argument` conventionally signals programmer error with a bad function argument. A bad mesh name read from a scene file is a data error, not a programming error. Changed to `std::runtime_error` for consistency with all other error paths.

---

## 24. PBR — Cook-Torrance BRDF replacing Blinn-Phong

### Why replace Blinn-Phong
Blinn-Phong violates energy conservation: a surface can reflect more light than it receives. It also uses two separate lighting paths (diffuse and specular) that don't naturally interact with material properties like metallic. The `specStrength` and `shininess` sliders have no physical interpretation — tuning them is guesswork.

**Cook-Torrance** is the industry-standard microfacet BRDF. It models surfaces as made up of tiny perfect mirrors (microfacets). Three functions govern the BRDF:

- **D** — Normal Distribution Function: what fraction of microfacets are oriented to reflect light toward the viewer.
- **G** — Geometry/shadowing term: how much microfacets self-shadow and self-occlude each other.
- **F** — Fresnel term: how much light is reflected vs refracted at the surface boundary.

### The metallic workflow
Two material parameters replace all the old Blinn-Phong knobs:

- **roughness** (0=mirror, 1=fully diffuse) — controls the spread of the microfacet distribution.
- **metallic** (0=dielectric/plastic, 1=metal) — controls how much the albedo color tints the specular reflection.

For dielectrics (plastic, stone, wood), F0 (reflectance at normal incidence) is approximately `0.04` (4%). For metals, F0 equals the albedo color itself — metals have colored specular reflections. The shader lerps between these two cases:

```slang
float3 F0 = lerp(float3(0.04), albedo, metallic);
```

### The three microfacet functions

**D_GGX — Trowbridge-Reitz normal distribution:**
```slang
float D_GGX(float NdotH, float alpha) {
    float a2    = alpha * alpha;
    float denom = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}
```
`alpha = roughness²` — squaring roughness gives a more perceptually linear response. `NdotH` is the cosine of the angle between the surface normal and the halfway vector H = normalize(L + V). When NdotH ≈ 1 (H aligned with N), the distribution peaks. As NdotH decreases, the distribution falls off more steeply for low alpha (smooth surfaces) and more gently for high alpha (rough surfaces).

**G_Smith — Schlick-Beckmann geometry function:**
```slang
float G_SchlickGGX(float NdotX, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotX / (NdotX * (1.0 - k) + k);
}
float G_Smith(float NdotV, float NdotL, float roughness) {
    return G_SchlickGGX(NdotV, roughness) * G_SchlickGGX(NdotL, roughness);
}
```
The geometry function models two effects: *shadowing* (incoming light blocked by microfacets before it hits the surface) and *masking* (outgoing light blocked before it reaches the eye). Smith factorises this into two independent GGX evaluations — one for the view direction (masking) and one for the light direction (shadowing). The `k` term uses Epic's remapping of roughness for analytical lights (`(r+1)²/8`), which gives less darkening at grazing angles compared to the IBL remapping (`r²/2`).

**F_Schlick — Fresnel approximation:**
```slang
float3 F_Schlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}
```
At normal incidence (cosTheta = 1), the result is exactly `F0`. At grazing incidence (cosTheta → 0), the result approaches `(1, 1, 1)` — all surfaces become mirror-like at shallow angles (Fresnel effect). `clamp` prevents NaN from negative cosTheta due to floating point.

### Assembling the full BRDF
```slang
float3 PBR(float3 N, float3 V, float3 L, float3 albedo,
           float metallic, float roughness, float3 F0, float3 radiance) {
    float3 H      = normalize(V + L);
    float  NdotL  = max(dot(N, L), 0.0);
    float  NdotV  = max(dot(N, V), 0.0);
    float  NdotH  = max(dot(N, H), 0.0);
    float  HdotV  = max(dot(H, V), 0.0);

    float  alpha  = roughness * roughness;
    float  D      = D_GGX(NdotH, alpha);
    float  G      = G_Smith(NdotV, NdotL, roughness);
    float3 F      = F_Schlick(HdotV, F0);

    // Cook-Torrance specular lobe
    float3 spec   = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);

    // Energy conservation: kD + kS = 1.
    // Metals have no diffuse (all light is specular).
    float3 kD     = (float3(1.0) - F) * (1.0 - metallic);
    float3 diffuse = kD * albedo / PI;

    return (diffuse + spec) * radiance * NdotL;
}
```

`kD = (1 - F) * (1 - metallic)` is the key energy-conservation term. `(1 - F)` ensures that light not reflected specularly is available for diffuse. `(1 - metallic)` drives metals' diffuse contribution to zero — metals absorb what they don't specularly reflect rather than scattering it diffusely.

The denominator `4 * NdotV * NdotL` normalises the microfacet BRDF for the solid angle. The `max(..., 0.001)` prevents division by zero when either vector is perpendicular to the surface.

### Ambient term
Proper PBR ambient requires Image Based Lighting (IBL) — a prefiltered environment map that integrates incoming radiance from all directions. Without IBL, a simple approximation is used:

```slang
float3 ambient = ubo.materialParams.x * (albedo * (1.0 - metallic) + F0 * metallic);
```

For dielectrics, ambient scales the diffuse albedo. For metals, ambient scales F0 (the base reflectance color). This is a crude approximation — it doesn't capture directional ambient variation or ambient occlusion — but it avoids the black-metal problem where metallic objects with no direct light become completely invisible.

### Metallic-roughness texture maps
glTF's metallic-roughness map stores roughness in the green channel and metallic in the blue channel:

```slang
float roughness = ubo.materialParams.y; // default from slider
float metallic  = ubo.materialParams.z; // default from slider
if (push.metallicRoughnessIndex != 0xFFFFu) {
    float4 mrSample = textures[NonUniformResourceIndex(push.metallicRoughnessIndex)].Sample(uv);
    roughness = mrSample.g;
    metallic  = mrSample.b;
}
```

This uses the same `0xFFFFu` sentinel pattern as other texture slots. Objects without a metallic-roughness map use the global default sliders. The glTF loader already extracted and stored the metallic-roughness map index in `r.metallicRoughnessIndex`; PBR just now actually uses it.

The push constants struct was updated: `specularMapIndex` → `metallicRoughnessIndex`. The `Renderable` struct and the JSON loader (`"specularMap"` field → populates `metallicRoughnessIndex`) were updated consistently.

### Why the visual difference is subtle without IBL
Cook-Torrance is physically correct for *direct* analytical lights. Without IBL:
- Ambient is still a flat term, not a hemisphere integral.
- There are no environment reflections.
- The full difference between PBR and Blinn-Phong only becomes apparent for extreme roughness/metallic combinations under multiple lights.

The foundation is now correct for the next step (IBL) even if the current result looks similar to Blinn-Phong under normal lighting conditions.

---

## 25. Camera-Fitted Shadow Projection

### The problem with a fixed ortho frustum
The old shadow map used a fixed orthographic projection:

```cpp
glm::mat4 lightProj = glm::ortho(-shadowOrthoSize, +shadowOrthoSize,
                                  -shadowOrthoSize, +shadowOrthoSize,
                                  shadowNear, shadowFar);
```

`shadowOrthoSize` was a manually-tuned slider. Too small → objects outside the frustum cast no shadow. Too large → the shadow map resolution (2048×2048 texels) covers a huge area, and each texel represents a large world-space footprint, producing blocky shadows (texel aliasing). There is no setting that works well for both small and large scenes without retuning.

### The fix: fit the shadow frustum to the camera frustum
The shadow frustum should cover exactly what the camera can see — no more, no less. The algorithm:

**Step 1 — Unproject the camera's view frustum corners to world space.**

The camera frustum in NDC is a unit cube with corners at `(±1, ±1, 0..1)` (Vulkan depth). The inverse of the combined view-projection matrix transforms these 8 corners from NDC back to world space:

```cpp
glm::mat4 camProj = glm::perspective(glm::radians(75.0f), aspect, 0.5f, shadowFar);
camProj[1][1] *= -1;  // Vulkan Y flip
glm::mat4 invCamVP = glm::inverse(camProj * camera.getViewMatrix());

std::array<glm::vec3, 8> corners;
int ci = 0;
for (float x : {-1.0f, 1.0f})
    for (float y : {-1.0f, 1.0f})
        for (float z : {0.0f, 1.0f}) {
            glm::vec4 pt = invCamVP * glm::vec4(x, y, z, 1.0f);
            corners[ci++] = glm::vec3(pt) / pt.w;
        }
```

`shadowFar` is used as the far plane rather than the camera's own far plane. This limits how far out the shadow is computed — very distant geometry doesn't need shadows. The slider is now "shadow distance" instead of "shadow ortho size".

**Step 2 — Find the frustum centroid.**

```cpp
glm::vec3 frustumCenter{};
for (auto& c : corners) frustumCenter += c;
frustumCenter /= 8.0f;
```

The centroid is used as the look-at target for the light camera. Centering the shadow frustum on the centroid minimises wasted texels at the edges.

**Step 3 — Position the light camera on the correct side.**

```cpp
glm::vec3 lightDir = glm::normalize(lightPos);
glm::mat4 lightView = glm::lookAt(
    frustumCenter + lightDir * shadowFar,  // camera position: upstream of the frustum
    frustumCenter,                          // look at the frustum center
    glm::vec3(0.0f, 0.0f, 1.0f));          // Z-up world
```

`lightDir` (normalised) points from the scene toward the sun. The light camera is placed at `frustumCenter + lightDir * shadowFar` — sufficiently far upstream that no scene geometry is behind the light. A subtle but critical sign: `+ lightDir * shadowFar`, not `- lightDir * shadowFar`. `lightDir` already points *toward* the sun (away from the scene), so adding it moves the camera to the sun side. Subtracting it would place the camera in the shadow side, behind the frustum — the shadow map would capture nothing.

**Step 4 — Fit an AABB around the corners in light space.**

```cpp
glm::vec3 minLS( 1e9f), maxLS(-1e9f);
for (auto& c : corners) {
    glm::vec4 ls = lightView * glm::vec4(c, 1.0f);
    minLS = glm::min(minLS, glm::vec3(ls));
    maxLS = glm::max(maxLS, glm::vec3(ls));
}
```

The orthographic bounds are now exactly `[minLS.x, maxLS.x]` × `[minLS.y, maxLS.y]`. The depth bounds need a margin for geometry outside the view frustum that can still cast shadows into it:

```cpp
float margin  = 20.0f;
float nearClip = max(0.01f, -maxLS.z);  // glm::lookAt puts -Z forward, so near is -maxZ
float farClip  = -minLS.z + margin;
```

In glm's view space, the camera looks toward -Z. The frustum corners at the near plane have the least negative Z (closest to 0), and corners at the far plane have the most negative Z. So the near clip is `-maxLS.z` and the far clip is `-minLS.z`. The `margin` extends the far clip backward to catch objects behind the view frustum that still cast shadows into it.

**Step 5 — Build the orthographic projection.**

```cpp
ubo.lightSpaceMatrix = glm::ortho(minLS.x, maxLS.x, minLS.y, maxLS.y, nearClip, farClip) * lightView;
```

This matrix transforms world-space positions into the light's clip space. It is uploaded to the UBO and used by both the shadow pass (to transform vertices) and the main pass (to project fragment positions for the shadow map lookup).

### Result
Shadow quality is now automatically correct at any scale — small test scenes, large indoor spaces like Sponza, and far outdoor terrain all get appropriate shadow map coverage without manual tuning. The only manual parameter is `shadowFar` (shadow distance), which controls the trade-off between shadow range and resolution.

---

## Summary — What exists, what is next

| System | Status |
|---|---|
| Vulkan device + swapchain | Done |
| MSAA | Done |
| Depth buffer | Done |
| Shadow mapping (PCF, slope-scale bias, camera-fitted projection) | Done |
| Bindless textures (up to 2048) | Done |
| OBJ loading (Y-up→Z-up, tangent generation) | Done |
| glTF 2.0 loading (fastgltf, GLB embedded images, node tree) | Done |
| JSON scene (objects, skybox, point lights) | Done |
| PBR shading (Cook-Torrance, metallic-roughness) | Done |
| Normal mapping | Done |
| Parallax Occlusion Mapping (POM) | Done |
| ACES tonemapping + exposure | Done |
| Point lights (up to 4, add/remove at runtime) | Done |
| Procedural skybox (gradient + sun) | Done |
| Exponential height fog (sky-synced color) | Done |
| Procedural sphere mesh | Done |
| sRGB vs linear texture format distinction | Done |
| Per-object transform editor | Done |
| Free-fly camera | Done |
| CMakePresets.json (debug/release/relwithdebinfo workflows) | Done |
| ASSET_DIR compile definition (CWD-independent asset paths) | Done |
| Code reorganisation (TextureManager, ImGuiLayer, Device GPU utilities) | Done |
| Image Based Lighting (IBL) for ambient | Not started |
| Bloom | Not started |
| SSAO | Not started |
| Alpha cutout / transparency | Not started |
| Render graph abstraction | Not started |

---

## References and Further Reading

### Books — read in this order

**1. "Vulkan Programming Guide" — Graham Sellers, John Kessenich (2016)**
The closest thing to an official Vulkan textbook. Covers device creation, render passes, pipelines, and synchronisation from first principles. Read chapters 1–7 before touching the swapchain code. Dense but authoritative.

**2. "Real-Time Rendering" (4th ed.) — Akenine-Möller, Haines, Hoffman (2018)**
The standard reference for everything that happens inside the shader: lighting models, shadow algorithms, PBR, normal mapping, tone mapping. Sections 9 (physically-based shading), 11 (global illumination), and 7 (shadows) map directly to the systems in this renderer. Has no Vulkan code — it is API-agnostic, covering the mathematics.

**3. "Physically Based Rendering: From Theory to Implementation" (4th ed., free online) — Pharr, Jakob, Humphreys**
The canonical PBR reference. More than you need for real-time, but chapter 8 (reflection models) and chapter 5 (colour and radiometry) are essential background for understanding why Blinn-Phong is an approximation and what PBR replaces it with.

**4. "GPU Gems" series — NVIDIA (free online)**
Collected chapters from practitioners. Particularly relevant: GPU Gems 1 chapter 11 (shadow map antialiasing — PCF), GPU Gems 2 chapter 18 (parallax occlusion mapping — the algorithm this renderer implements), GPU Gems 3 chapter 8 (summed-area variance shadow maps).

**5. "3D Math Primer for Graphics and Game Development" (2nd ed.) — Dunn, Parberry**
The math foundation: vectors, matrices, coordinate spaces, quaternions, Euler angles. Read this if the TBN matrix construction, the cross-product handedness rules, or the projection derivation feel opaque.

### Online resources

**vkguide.dev** — The most practical Vulkan tutorial. Covers dynamic rendering (the approach used here, without render pass objects), descriptor indexing, and push constants. Chapters 4–5 cover the bindless architecture directly.

**learnopengl.com** — API-agnostic explanations of normal mapping (chapter "Normal Mapping"), shadow mapping ("Shadow Mapping"), PBR ("PBR Theory" and "PBR Lighting"), and HDR/tonemapping. The concepts translate directly; only the API calls differ.

**Khronos glTF 2.0 specification** (`KhronosGroup/glTF` on GitHub) — The authoritative reference for the format. Section 3 (Concepts) explains nodes, meshes, accessors, and materials precisely. Read alongside fastgltf's documentation.

**fastgltf documentation** (`fastgltf.readthedocs.io`) — API reference for the library used here. The "Loading a glTF" and "Accessing data" sections explain how to iterate accessors, handle embedded images, and walk the scene graph.

**Filament design documents** (Google, free on GitHub) — The design doc for Google's open-source PBR renderer. Section on materials explains the metallic-roughness workflow, the BRDF derivation, and image-based lighting in implementation-ready terms. More practical than Pharr et al. for real-time.

### Exercises

**Shadow mapping:**
- Implement variance shadow maps (VSM) as an alternative to PCF. VSM stores mean and variance of depth in a two-channel texture and uses Chebyshev's inequality to compute shadow probability analytically.
- Make the shadow frustum automatically fit the camera's view frustum (cascaded shadow maps, CSM). Split the frustum into 2–4 depth ranges, render a shadow map per range.

**Lighting:**
- Replace Blinn-Phong with a Cook-Torrance BRDF. Start with the GGX NDF (`D`), Schlick Fresnel (`F`), and Smith-GGX geometric term (`G`). The metallic-roughness textures already loaded can then be used.
- Add image-based lighting (IBL): precompute a diffuse irradiance cubemap and a specular prefiltered environment map from an HDRI. The split-sum approximation (Brian Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013) is the standard approach.

**Normal and displacement mapping:**
- Implement proper relief mapping (Policarpo, 2005) as an upgrade to POM — it handles occlusion of nearby relief features and self-shadowing within the height field.
- Add `gl_FrontFacing` detection in the fragment shader to flip normals on double-sided glTF materials (Sponza curtains and leaves currently light incorrectly from the back).

**Scene loading:**
- Implement alpha cutout: read `material.alphaMode` from glTF (`MASK` mode uses `alphaCutoff`). In the fragment shader, `discard` if `albedo.a < alphaCutoff`. Apply the same discard in the shadow pass so alpha-tested geometry casts correct shadows.
- Add a texture cache keyed on `(path, linearFormat)` so the same image can be loaded as both sRGB and linear without duplication (currently the cache only keys on path).
- Support glTF `extras` fields to pass custom per-primitive data (height map paths, custom material parameters) through the glTF file rather than a separate scene.json.

**Post-processing:**
- Implement screen-space ambient occlusion (SSAO). Render the scene to a G-buffer (world normals, depth), then in a fullscreen pass sample a hemisphere of random points and test their depth against the depth buffer. Blur the result. Add it as a multiplier on the ambient term.
- Implement bloom: downsample the HDR framebuffer through a chain of half-resolution images, apply a threshold to keep only bright regions, then upsample and add back. The dual-kawase blur is a common efficient implementation.

**Architecture:**
- Implement a render graph (also called a frame graph). Instead of manually ordering passes in `recordCommandBuffer`, declare each pass as a node with input/output attachments. The graph automatically infers barriers and pass ordering. Reference: "FrameGraph: Extensible Rendering Architecture in Frostbite" (Halén, GDC 2017, free slides online).
- Move to per-frame descriptor set updates using `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC` with a single large ring buffer for UBO data, rather than one buffer per frame in flight.

---

## 26. Code Reorganisation — TextureManager, ImGuiLayer, Device GPU Utilities

### The problem: `VulkanRenderer.cpp` as a monolith

`VulkanRenderer.cpp` had grown to ~2150 lines and had absorbed three unrelated concerns:

- **GPU memory utilities** (`createBuffer`, `createImage`, `copyBuffer`, `copyBufferToImage`, `transitionImageLayout`, `beginSingleTimeCommands`, `endSingleTimeCommands`) — these belong on the device, not the renderer
- **Texture loading and management** (`loadTexture`, `loadTextureFromMemory`, `generateMipmaps`, the `Texture` struct, `textures` vector, `textureCache` map) — a standalone resource system
- **ImGui frame lifecycle** (`imGuiInit`, the descriptor pool, `beginFrame`/`endFrame`, `beginRendering`/`endRendering` around draw data) — a UI integration layer

In real engines, these live in separate modules. In Unreal: `FRHICommandList` for GPU commands, `FTextureManager` / `FStreamingManager` for textures, `SImGuiWidget` for UI. In Unity: `RenderTexture`, `TextureImporter`, `IMGUIModule`. The pattern is consistent: separate what changes independently.

### Why not `VulkanRenderer_textures.cpp`?

A common reflex is to split one `.cpp` into several `ClassName_topic.cpp` files. This is not a class extraction — it is just a compilation unit split. All the methods still live on `VulkanRenderer`; the header still grows with every new method; the coupling stays. It also has no real-world precedent: industry codebases do not use the `_` suffix as a splitting convention.

The right move is to extract actual classes with their own headers, their own lifetimes, and their own interfaces.

### What changed

#### `Device` — GPU utilities moved here

The `Device` class is the natural home for "do something with the GPU once": buffer creation, image creation, one-shot command submission. These operations need the logical device, physical device, graphics queue, and queue family index — all of which `Device` already owns.

A dedicated `uploadCommandPool_` is created with `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT` at device construction time. The transient flag is a hint to the driver that command buffers allocated from this pool are short-lived and can be returned to the pool immediately after submission — appropriate for one-shot upload commands. This is separate from `VulkanRenderer`'s per-frame command pool (which lives for the full duration of the frame).

New public methods on `Device`:

```cpp
void createImage(uint32_t w, uint32_t h, uint32_t mipLevels,
                 vk::SampleCountFlagBits samples, vk::Format format,
                 vk::ImageTiling tiling, vk::ImageUsageFlags usage,
                 vk::MemoryPropertyFlags properties,
                 vk::raii::Image& image, vk::raii::DeviceMemory& memory);

void createBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage,
                  vk::MemoryPropertyFlags properties,
                  vk::raii::Buffer& buffer, vk::raii::DeviceMemory& memory);

std::unique_ptr<vk::raii::CommandBuffer> beginSingleTimeCommands();
void endSingleTimeCommands(vk::raii::CommandBuffer& cmd);

void copyBuffer(vk::raii::Buffer& src, vk::raii::Buffer& dst, vk::DeviceSize size);
void copyBufferToImage(const vk::raii::Buffer& buffer, vk::raii::Image& image,
                       uint32_t width, uint32_t height);
void transitionImageLayout(const vk::raii::Image& image,
                           vk::ImageLayout oldLayout, vk::ImageLayout newLayout);
```

#### `TextureManager` — bindless texture ownership

`TextureManager` owns the texture array that backs the bindless descriptor. Responsibilities:

- Load from a file path (with a path-keyed cache to deduplicate).
- Load from in-memory bytes (for glTF embedded images).
- Upload pixels to a device-local image (with full mipmap generation).
- Return a `std::vector<vk::DescriptorImageInfo>` that `VulkanRenderer` writes into the descriptor set.

The `init(Device&)` pattern — default-construct, then call `init` after the device is ready — is used because the device doesn't exist at the time `VulkanRenderer` is constructed (it's created inside `initVulkan()`). The `TextureManager` stores a raw non-owning pointer (`Device* device_`) to the device. Raw pointers for non-owning references are idiomatic C++ when the pointed-to object is guaranteed to outlive the pointer (the device is owned by `VulkanRenderer` and outlives `textureManager_`). A `unique_ptr` would imply ownership; a `reference_wrapper` would be more awkward to reset on cleanup.

`STB_IMAGE_IMPLEMENTATION` is defined here and only here. The STB single-file library pattern requires exactly one translation unit to define the implementation macro before the include; all other units that include the header get only the declarations. Moving the define to `TextureManager.cpp` ensures `VulkanRenderer.cpp` (and any future file) can include `<stb_image.h>` for declarations without violating the one-definition rule.

Cleanup is explicit: `textureManager_ = TextureManager{}` in `cleanup()` destroys all GPU textures (images, memory, views, samplers) before `vulkanDevice.reset()`. If this were left to the destructor, C++ would destroy members in reverse declaration order — `textureManager_` before `vulkanDevice` — which happens to work, but only as a side effect of declaration order. Making it explicit is less fragile.

#### `ImGuiLayer` — UI integration lifecycle

`ImGuiLayer` wraps the three-phase ImGui integration:

| Phase | When | What |
|---|---|---|
| `init` | Startup | Create descriptor pool, call `ImGui_ImplVulkan_Init` |
| `beginFrame` / `endFrame` | Each frame, before record | `NewFrame` … widgets … `Render` |
| `renderDrawData` | Inside command buffer | `beginRendering` → `RenderDrawData` → `endRendering` |

The `renderDrawData` method takes ownership of the `beginRendering`/`endRendering` scope. Previously these were inline in `recordCommandBuffer`. Inlining them there meant `VulkanRenderer` had to know about ImGui's rendering requirements (attachment format, load op). Encapsulating them in `ImGuiLayer` means `VulkanRenderer` only calls one function and doesn't need to know how ImGui uses the command buffer.

The `struct GLFWwindow;` forward declaration in `ImGuiLayer.hpp` avoids pulling GLFW into every translation unit that includes the header. GLFW defines `GLFWwindow` as a struct, so a forward declaration is sufficient for a pointer parameter in a function declaration. The full GLFW include is deferred to `ImGuiLayer.cpp` where it's actually needed.

`shutdown()` sets `pool_ = nullptr` to release the Vulkan descriptor pool, then the RAII destructor of `pool_` is a no-op on a null handle. This pattern avoids a double-free if `ImGuiLayer`'s destructor is called after the device has already been destroyed.

### Destruction ordering and RAII

GPU objects must be destroyed before the Vulkan device that owns them. With RAII this is automatic if members are declared after the device in the struct — C++ destroys members in reverse declaration order. But when objects are scattered across multiple classes, it is better to make the ordering explicit:

```cpp
void VulkanRenderer::cleanup()
{
    vulkanDevice->getLogicalDevice().waitIdle();
    imguiLayer_.shutdown();          // releases ImGui pool before device
    textureManager_ = TextureManager{};  // releases all texture GPU memory
    // ... pipelines, buffers, sync objects (all RAII) ...
    swapchain.reset();
    vulkanDevice.reset();            // device last
}
```

Relying on implicit destruction order is brittle: adding a new member above `vulkanDevice` would silently break it. Explicit calls in `cleanup()` are self-documenting and order-independent.

### Dead code removed: `hasStencilComponent`

A static helper `hasStencilComponent(vk::Format)` was declared in `VulkanRenderer.hpp`, defined in `VulkanRenderer.cpp`, but never called anywhere in the codebase. It was removed entirely. (It was a leftover from an early depth-buffer implementation; the stencil check it provided was never wired into `transitionImageLayout`.)

### Result

| File | Before | After |
|---|---|---|
| `VulkanRenderer.cpp` | ~2150 lines | ~1400 lines |
| `TextureManager.cpp` | — | ~205 lines (new) |
| `ImGuiLayer.cpp` | — | ~97 lines (new) |
| `Device.cpp` | — | +~150 lines (GPU utilities) |

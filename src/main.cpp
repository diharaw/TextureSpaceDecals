#define _USE_MATH_DEFINES
#include <application.h>
#include <mesh.h>
#include <camera.h>
#include <material.h>
#include <memory>
#include <iostream>
#include <stack>
#include <random>
#include <chrono>
#include <random>
#include <rtccore.h>
#include <rtcore_geometry.h>
#include <rtcore_common.h>
#include <rtcore_ray.h>
#include <rtcore_device.h>
#include <rtcore_scene.h>

#define CAMERA_FAR_PLANE 1000.0f
#define PROJECTOR_BACK_OFF_DISTANCE 10.0f;
#define ALBEDO_TEXTURE_SIZE 4096
#define DEPTH_TEXTURE_SIZE 512

struct GlobalUniforms
{
    DW_ALIGNED(16)
    glm::mat4 view_proj;
    DW_ALIGNED(16)
    glm::mat4 light_view_proj;
    DW_ALIGNED(16)
    glm::vec4 cam_pos;
};

class TextureSpaceDecals : public dw::Application
{
protected:
    // -----------------------------------------------------------------------------------------------------------------------------------

    bool init(int argc, const char* argv[]) override
    {
        // Create GPU resources.
        if (!create_shaders())
            return false;

        if (!create_uniform_buffer())
            return false;

        // Load scene.
        if (!load_scene())
            return false;

        if (!load_decals())
            return false;

        if (!initialize_embree())
            return false;

        create_framebuffers();

        // Create camera.
        create_camera();

        m_transform = glm::mat4(1.0f);

        init_texture();

        return true;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void update(double delta) override
    {
        // Update camera.
        update_camera();

        update_global_uniforms(m_global_uniforms);

        if (m_debug_gui)
            ui();

        if (m_requires_update)
        {
            m_requires_update = false;
            render_depth_map();
            apply_decals();
            m_albedo_texture->generate_mipmaps();
        }

        render_lit_scene();

        if (m_visualize_albedo_map)
            visualize_albedo_map();

        if (m_hit_distance != INFINITY)
        {
            if (m_visualize_hit_point)
                m_debug_draw.sphere(2.0f, m_hit_pos, glm::vec3(1.0f, 0.0f, 0.0f));

            if (m_visualize_projection_frustum)
                m_debug_draw.frustum(m_global_uniforms.light_view_proj, glm::vec3(0.0f, 1.0f, 0.0f));

            if (m_visualize_hit_point || m_visualize_projection_frustum)
                m_debug_draw.render(nullptr, m_width, m_height, m_global_uniforms.view_proj);
        }
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void shutdown() override
    {
        rtcReleaseGeometry(m_embree_triangle_mesh);
        rtcReleaseScene(m_embree_scene);
        rtcReleaseDevice(m_embree_device);

        dw::Mesh::unload(m_mesh);
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void window_resized(int width, int height) override
    {
        // Override window resized method to update camera projection.
        m_main_camera->update_projection(60.0f, 0.1f, CAMERA_FAR_PLANE, float(m_width) / float(m_height));

        create_framebuffers();
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void key_pressed(int code) override
    {
        // Handle forward movement.
        if (code == GLFW_KEY_W)
            m_heading_speed = m_camera_speed;
        else if (code == GLFW_KEY_S)
            m_heading_speed = -m_camera_speed;

        // Handle sideways movement.
        if (code == GLFW_KEY_A)
            m_sideways_speed = -m_camera_speed;
        else if (code == GLFW_KEY_D)
            m_sideways_speed = m_camera_speed;

        if (code == GLFW_KEY_SPACE)
            m_mouse_look = true;

        if (code == GLFW_KEY_G)
            m_debug_gui = !m_debug_gui;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void key_released(int code) override
    {
        // Handle forward movement.
        if (code == GLFW_KEY_W || code == GLFW_KEY_S)
            m_heading_speed = 0.0f;

        // Handle sideways movement.
        if (code == GLFW_KEY_A || code == GLFW_KEY_D)
            m_sideways_speed = 0.0f;

        if (code == GLFW_KEY_SPACE)
            m_mouse_look = false;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void mouse_pressed(int code) override
    {
        if (code == GLFW_MOUSE_BUTTON_LEFT)
        {
            double xpos, ypos;
            glfwGetCursorPos(m_window, &xpos, &ypos);

            glm::vec4 ndc_pos      = glm::vec4((2.0f * float(xpos)) / float(m_width) - 1.0f, 1.0 - (2.0f * float(ypos)) / float(m_height), -1.0f, 1.0f);
            glm::vec4 view_coords  = glm::inverse(m_main_camera->m_projection) * ndc_pos;
            glm::vec4 world_coords = glm::inverse(m_main_camera->m_view) * glm::vec4(view_coords.x, view_coords.y, -1.0f, 0.0f);

            glm::vec3 ray_dir = glm::normalize(glm::vec3(world_coords));

            RTCRayHit rayhit;

            rayhit.ray.dir_x = ray_dir.x;
            rayhit.ray.dir_y = ray_dir.y;
            rayhit.ray.dir_z = ray_dir.z;

            rayhit.ray.org_x = m_main_camera->m_position.x;
            rayhit.ray.org_y = m_main_camera->m_position.y;
            rayhit.ray.org_z = m_main_camera->m_position.z;

            rayhit.ray.tnear     = 0;
            rayhit.ray.tfar      = INFINITY;
            rayhit.ray.mask      = 0;
            rayhit.ray.flags     = 0;
            rayhit.hit.geomID    = RTC_INVALID_GEOMETRY_ID;
            rayhit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

            rtcIntersect1(m_embree_scene, &m_embree_intersect_context, &rayhit);

            if (rayhit.ray.tfar != INFINITY)
            {
                m_hit_pos      = m_main_camera->m_position + ray_dir * rayhit.ray.tfar;
                m_hit_normal   = glm::vec3(rayhit.hit.Ng_x, rayhit.hit.Ng_y, rayhit.hit.Ng_z);
                m_hit_distance = rayhit.ray.tfar;

                m_projector_pos = m_hit_pos + m_hit_normal * PROJECTOR_BACK_OFF_DISTANCE;
                m_projector_dir = -m_hit_normal;

                m_requires_update = true;

                if (m_randomize_decals)
                {
                    std::random_device                    rd;
                    std::mt19937                          gen(rd());
                    std::uniform_real_distribution<float> scale_dis(5.0, 20.0);
                    std::uniform_real_distribution<float> rotation_dis(-90.0, 90.0);
                    std::uniform_int_distribution<>       index_dis(0, 3);

                    m_selected_decal     = index_dis(gen);
                    m_projector_size     = scale_dis(gen);
                    m_projector_rotation = rotation_dis(gen);
                }
            }
            else
                rayhit.ray.tfar = INFINITY;
        }

        // Enable mouse look.
        if (code == GLFW_MOUSE_BUTTON_RIGHT)
            m_mouse_look = true;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void mouse_released(int code) override
    {
        // Disable mouse look.
        if (code == GLFW_MOUSE_BUTTON_RIGHT)
            m_mouse_look = false;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

protected:
    // -----------------------------------------------------------------------------------------------------------------------------------

    dw::AppSettings intial_app_settings() override
    {
        dw::AppSettings settings;

        settings.resizable    = true;
        settings.maximized    = false;
        settings.refresh_rate = 60;
        settings.major_ver    = 4;
        settings.width        = 1920;
        settings.height       = 1080;
        settings.title        = "Texture Space Decals (c) 2019 Dihara Wijetunga";

        return settings;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

private:
    // -----------------------------------------------------------------------------------------------------------------------------------

    void init_texture()
    {
        if (m_enable_conservative_raster)
        {
            if (GLAD_GL_NV_conservative_raster)
                glEnable(GL_CONSERVATIVE_RASTERIZATION_NV);
            else if (GLAD_GL_INTEL_conservative_rasterization)
                glEnable(GL_INTEL_conservative_rasterization);
        }

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);

        glDisable(GL_CULL_FACE);

        m_albedo_fbo->bind();

        glViewport(0, 0, ALBEDO_TEXTURE_SIZE, ALBEDO_TEXTURE_SIZE);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Bind shader program.
        m_texture_init_program->use();

        // Bind uniform buffers.
        m_global_ubo->bind_base(0);

        m_texture_init_program->set_uniform("u_Model", m_transform);

        // Bind vertex array.
        m_mesh->mesh_vertex_array()->bind();

        dw::SubMesh* submeshes = m_mesh->sub_meshes();

        for (uint32_t i = 0; i < m_mesh->sub_mesh_count(); i++)
        {
            dw::SubMesh& submesh = submeshes[i];

            // Issue draw call.
            glDrawElementsBaseVertex(GL_TRIANGLES, submesh.index_count, GL_UNSIGNED_INT, (void*)(sizeof(unsigned int) * submesh.base_index), submesh.base_vertex);
        }

        if (m_enable_conservative_raster)
        {
            if (GLAD_GL_NV_conservative_raster)
                glDisable(GL_CONSERVATIVE_RASTERIZATION_NV);
            else if (GLAD_GL_INTEL_conservative_rasterization)
                glDisable(GL_INTEL_conservative_rasterization);
        }

        m_albedo_texture->generate_mipmaps();
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void apply_decals()
    {
        if (m_enable_conservative_raster)
        {
            if (GLAD_GL_NV_conservative_raster)
                glEnable(GL_CONSERVATIVE_RASTERIZATION_NV);
            else if (GLAD_GL_INTEL_conservative_rasterization)
                glEnable(GL_INTEL_conservative_rasterization);
        }

        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glDisable(GL_CULL_FACE);

        m_albedo_fbo->bind();

        glViewport(0, 0, ALBEDO_TEXTURE_SIZE, ALBEDO_TEXTURE_SIZE);

        // Bind shader program.
        m_decal_program->use();

        // Bind uniform buffers.
        m_global_ubo->bind_base(0);

        m_decal_program->set_uniform("u_Model", m_transform);

        // Bind vertex array.
        m_mesh->mesh_vertex_array()->bind();

        dw::SubMesh* submeshes = m_mesh->sub_meshes();

        for (uint32_t i = 0; i < m_mesh->sub_mesh_count(); i++)
        {
            dw::SubMesh& submesh = submeshes[i];

            if (m_decal_program->set_uniform("s_Decal", 0))
                m_decal_textures[m_selected_decal]->bind(0);

            if (m_decal_program->set_uniform("s_Depth", 1))
                m_depth_texture->bind(1);

            // Issue draw call.
            glDrawElementsBaseVertex(GL_TRIANGLES, submesh.index_count, GL_UNSIGNED_INT, (void*)(sizeof(unsigned int) * submesh.base_index), submesh.base_vertex);
        }

        glDisable(GL_BLEND);

        if (m_enable_conservative_raster)
        {
            if (GLAD_GL_NV_conservative_raster)
                glDisable(GL_CONSERVATIVE_RASTERIZATION_NV);
            else if (GLAD_GL_INTEL_conservative_rasterization)
                glDisable(GL_INTEL_conservative_rasterization);
        }
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void render_depth_map()
    {
        render_scene(m_depth_fbo.get(), m_depth_program, 0, 0, DEPTH_TEXTURE_SIZE, DEPTH_TEXTURE_SIZE, GL_BACK);
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void render_lit_scene()
    {
        render_scene(nullptr, m_mesh_program, 0, 0, m_width, m_height, GL_BACK);
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void visualize_albedo_map()
    {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);

        glViewport(0, 0, 512, 512);

        // Bind shader program.
        m_visualize_program->use();

        if (m_visualize_program->set_uniform("s_Texture", 0))
            m_albedo_texture->bind(0);

        // Render fullscreen triangle
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    bool create_shaders()
    {
        {
            // Create general shaders
            m_uv_space_vs      = std::unique_ptr<dw::Shader>(dw::Shader::create_from_file(GL_VERTEX_SHADER, "shader/uv_space_vs.glsl"));
            m_texture_init_fs  = std::unique_ptr<dw::Shader>(dw::Shader::create_from_file(GL_FRAGMENT_SHADER, "shader/texture_init_fs.glsl"));
            m_decal_project_fs = std::unique_ptr<dw::Shader>(dw::Shader::create_from_file(GL_FRAGMENT_SHADER, "shader/decal_project_fs.glsl"));
            m_mesh_vs          = std::unique_ptr<dw::Shader>(dw::Shader::create_from_file(GL_VERTEX_SHADER, "shader/mesh_vs.glsl"));
            m_mesh_fs          = std::unique_ptr<dw::Shader>(dw::Shader::create_from_file(GL_FRAGMENT_SHADER, "shader/mesh_fs.glsl"));
            m_triangle_vs      = std::unique_ptr<dw::Shader>(dw::Shader::create_from_file(GL_VERTEX_SHADER, "shader/fullscreen_triangle_vs.glsl"));
            m_visualize_fs     = std::unique_ptr<dw::Shader>(dw::Shader::create_from_file(GL_FRAGMENT_SHADER, "shader/visualize_albedo_fs.glsl"));
            m_depth_vs         = std::unique_ptr<dw::Shader>(dw::Shader::create_from_file(GL_VERTEX_SHADER, "shader/depth_vs.glsl"));
            m_depth_fs         = std::unique_ptr<dw::Shader>(dw::Shader::create_from_file(GL_FRAGMENT_SHADER, "shader/depth_fs.glsl"));

            {
                if (!m_uv_space_vs || !m_decal_project_fs)
                {
                    DW_LOG_FATAL("Failed to create Shaders");
                    return false;
                }

                // Create general shader program
                dw::Shader* shaders[] = { m_uv_space_vs.get(), m_decal_project_fs.get() };
                m_decal_program       = std::make_unique<dw::Program>(2, shaders);

                if (!m_decal_program)
                {
                    DW_LOG_FATAL("Failed to create Shader Program");
                    return false;
                }

                m_decal_program->uniform_block_binding("GlobalUniforms", 0);
            }

            {
                if (!m_uv_space_vs || !m_texture_init_fs)
                {
                    DW_LOG_FATAL("Failed to create Shaders");
                    return false;
                }

                // Create general shader program
                dw::Shader* shaders[]  = { m_uv_space_vs.get(), m_texture_init_fs.get() };
                m_texture_init_program = std::make_unique<dw::Program>(2, shaders);

                if (!m_texture_init_program)
                {
                    DW_LOG_FATAL("Failed to create Shader Program");
                    return false;
                }

                m_texture_init_program->uniform_block_binding("GlobalUniforms", 0);
            }

            {
                if (!m_depth_vs || !m_depth_fs)
                {
                    DW_LOG_FATAL("Failed to create Shaders");
                    return false;
                }

                // Create general shader program
                dw::Shader* shaders[] = { m_depth_vs.get(), m_depth_fs.get() };
                m_depth_program       = std::make_unique<dw::Program>(2, shaders);

                if (!m_texture_init_program)
                {
                    DW_LOG_FATAL("Failed to create Shader Program");
                    return false;
                }

                m_depth_program->uniform_block_binding("GlobalUniforms", 0);
            }

            {
                if (!m_mesh_vs || !m_mesh_fs)
                {
                    DW_LOG_FATAL("Failed to create Shaders");
                    return false;
                }

                // Create general shader program
                dw::Shader* shaders[] = { m_mesh_vs.get(), m_mesh_fs.get() };
                m_mesh_program        = std::make_unique<dw::Program>(2, shaders);

                if (!m_mesh_program)
                {
                    DW_LOG_FATAL("Failed to create Shader Program");
                    return false;
                }

                m_mesh_program->uniform_block_binding("GlobalUniforms", 0);
            }

            {
                if (!m_triangle_vs || !m_visualize_fs)
                {
                    DW_LOG_FATAL("Failed to create Shaders");
                    return false;
                }

                // Create general shader program
                dw::Shader* shaders[] = { m_triangle_vs.get(), m_visualize_fs.get() };
                m_visualize_program   = std::make_unique<dw::Program>(2, shaders);

                if (!m_visualize_program)
                {
                    DW_LOG_FATAL("Failed to create Shader Program");
                    return false;
                }

                m_visualize_program->uniform_block_binding("GlobalUniforms", 0);
            }
        }

        return true;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void create_framebuffers()
    {
        m_albedo_texture = std::make_unique<dw::Texture2D>(ALBEDO_TEXTURE_SIZE, ALBEDO_TEXTURE_SIZE, 1, 1, 1, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE);

        m_albedo_texture->set_wrapping(GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
        m_albedo_texture->set_min_filter(GL_LINEAR_MIPMAP_LINEAR);
        m_albedo_texture->set_mag_filter(GL_LINEAR);

        m_albedo_fbo = std::make_unique<dw::Framebuffer>();

        m_albedo_fbo->attach_render_target(0, m_albedo_texture.get(), 0, 0);

        m_depth_texture = std::make_unique<dw::Texture2D>(DEPTH_TEXTURE_SIZE, DEPTH_TEXTURE_SIZE, 1, 1, 1, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_HALF_FLOAT);

        m_depth_texture->set_wrapping(GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);

        m_depth_fbo = std::make_unique<dw::Framebuffer>();

        m_depth_fbo->attach_depth_stencil_target(m_depth_texture.get(), 0, 0);
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    bool create_uniform_buffer()
    {
        // Create uniform buffer for global data
        m_global_ubo = std::make_unique<dw::UniformBuffer>(GL_DYNAMIC_DRAW, sizeof(GlobalUniforms));

        return true;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void ui()
    {
        if (!m_randomize_decals)
        {
            ImGui::DragFloat("Decal Rotation", &m_projector_rotation, 1.0f, -180.0f, 180.0f);
            ImGui::DragFloat("Decal Size", &m_projector_size, 1.0f, 0.1f, 20.0f);

            const char* listbox_items[] = { "OpenGL", "Vulkan", "DirectX", "Metal" };
            ImGui::ListBox("Selected Decal", &m_selected_decal, listbox_items, IM_ARRAYSIZE(listbox_items), 4);
        }

        ImGui::Checkbox("Randomize Decals", &m_randomize_decals);
        ImGui::Checkbox("Visualize Projector Frustum", &m_visualize_projection_frustum);
        ImGui::Checkbox("Visualize Hit Point", &m_visualize_hit_point);
        ImGui::Checkbox("Visualize Albedo Map", &m_visualize_albedo_map);
        ImGui::Checkbox("Conservative Rasterization", &m_enable_conservative_raster);

        if (ImGui::Button("Clear Texture"))
            init_texture();

        if (!GLAD_GL_NV_conservative_raster && !GLAD_GL_INTEL_conservative_rasterization)
        {
            ImGui::Separator();
            ImGui::Text("Note: Conservative Rasterization not supported on this GPU.");
        }
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    bool load_scene()
    {
        m_mesh = dw::Mesh::load("mesh/teapot_smooth.obj");

        if (!m_mesh)
        {
            DW_LOG_FATAL("Failed to load mesh!");
            return false;
        }

        return true;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    bool load_decals()
    {
        m_decal_textures.resize(4);
        m_decal_textures[0] = std::unique_ptr<dw::Texture2D>(dw::Texture2D::create_from_files("texture/opengl.png", true));
        m_decal_textures[0]->set_min_filter(GL_LINEAR_MIPMAP_LINEAR);
        m_decal_textures[0]->set_mag_filter(GL_LINEAR);

        m_decal_textures[1] = std::unique_ptr<dw::Texture2D>(dw::Texture2D::create_from_files("texture/vulkan.png", true));
        m_decal_textures[1]->set_min_filter(GL_LINEAR_MIPMAP_LINEAR);
        m_decal_textures[1]->set_mag_filter(GL_LINEAR);

        m_decal_textures[2] = std::unique_ptr<dw::Texture2D>(dw::Texture2D::create_from_files("texture/directx.png", true));
        m_decal_textures[2]->set_min_filter(GL_LINEAR_MIPMAP_LINEAR);
        m_decal_textures[2]->set_mag_filter(GL_LINEAR);

        m_decal_textures[3] = std::unique_ptr<dw::Texture2D>(dw::Texture2D::create_from_files("texture/metal.png", true));
        m_decal_textures[3]->set_min_filter(GL_LINEAR_MIPMAP_LINEAR);
        m_decal_textures[3]->set_mag_filter(GL_LINEAR);

        return true;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    bool initialize_embree()
    {
        m_embree_device = rtcNewDevice(nullptr);

        RTCError embree_error = rtcGetDeviceError(m_embree_device);

        if (embree_error == RTC_ERROR_UNSUPPORTED_CPU)
            throw std::runtime_error("Your CPU does not meet the minimum requirements for embree");
        else if (embree_error != RTC_ERROR_NONE)
            throw std::runtime_error("Failed to initialize embree!");

        m_embree_scene = rtcNewScene(m_embree_device);

        m_embree_triangle_mesh = rtcNewGeometry(m_embree_device, RTC_GEOMETRY_TYPE_TRIANGLE);

        std::vector<glm::vec3> vertices(m_mesh->vertex_count());
        std::vector<uint32_t>  indices(m_mesh->index_count());
        uint32_t               idx        = 0;
        dw::Vertex*            vertex_ptr = m_mesh->vertices();
        uint32_t*              index_ptr  = m_mesh->indices();

        for (int i = 0; i < m_mesh->vertex_count(); i++)
            vertices[i] = vertex_ptr[i].position;

        for (int i = 0; i < m_mesh->sub_mesh_count(); i++)
        {
            dw::SubMesh& submesh = m_mesh->sub_meshes()[i];

            for (int j = submesh.base_index; j < (submesh.base_index + submesh.index_count); j++)
                indices[idx++] = submesh.base_vertex + index_ptr[j];
        }

        void* data = rtcSetNewGeometryBuffer(m_embree_triangle_mesh, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, sizeof(glm::vec3), m_mesh->vertex_count());
        memcpy(data, vertices.data(), vertices.size() * sizeof(glm::vec3));

        data = rtcSetNewGeometryBuffer(m_embree_triangle_mesh, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(uint32_t), m_mesh->index_count() / 3);
        memcpy(data, indices.data(), indices.size() * sizeof(uint32_t));

        rtcCommitGeometry(m_embree_triangle_mesh);
        rtcAttachGeometry(m_embree_scene, m_embree_triangle_mesh);
        rtcCommitScene(m_embree_scene);

        rtcInitIntersectContext(&m_embree_intersect_context);

        return true;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void create_camera()
    {
        m_main_camera = std::make_unique<dw::Camera>(60.0f, 0.1f, CAMERA_FAR_PLANE, float(m_width) / float(m_height), glm::vec3(150.0f, 20.0f, 0.0f), glm::vec3(-1.0f, 0.0, 0.0f));
        m_main_camera->set_rotatation_delta(glm::vec3(0.0f, -90.0f, 0.0f));
        m_main_camera->update();
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void render_mesh(dw::Mesh* mesh, glm::mat4 model, std::unique_ptr<dw::Program>& program)
    {
        program->set_uniform("u_Model", model);

        // Bind vertex array.
        mesh->mesh_vertex_array()->bind();

        dw::SubMesh* submeshes = mesh->sub_meshes();

        for (uint32_t i = 0; i < mesh->sub_mesh_count(); i++)
        {
            dw::SubMesh& submesh = submeshes[i];

            if (program->set_uniform("s_Texture", 0))
                m_albedo_texture->bind(0);

            // Issue draw call.
            glDrawElementsBaseVertex(GL_TRIANGLES, submesh.index_count, GL_UNSIGNED_INT, (void*)(sizeof(unsigned int) * submesh.base_index), submesh.base_vertex);
        }
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void render_scene(dw::Framebuffer* fbo, std::unique_ptr<dw::Program>& program, int x, int y, int w, int h, GLenum cull_face, bool clear = true)
    {
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);

        if (cull_face == GL_NONE)
            glDisable(GL_CULL_FACE);
        else
        {
            glEnable(GL_CULL_FACE);
            glCullFace(cull_face);
        }

        if (fbo)
            fbo->bind();
        else
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

        glViewport(x, y, w, h);

        if (clear)
        {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClearDepth(1.0);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }

        // Bind shader program.
        program->use();

        // Bind uniform buffers.
        m_global_ubo->bind_base(0);

        // Draw scene.
        render_mesh(m_mesh, m_transform, program);
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void update_global_uniforms(const GlobalUniforms& global)
    {
        void* ptr = m_global_ubo->map(GL_WRITE_ONLY);
        memcpy(ptr, &global, sizeof(GlobalUniforms));
        m_global_ubo->unmap();
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void update_transforms(dw::Camera* camera)
    {
        // Update camera matrices.
        m_global_uniforms.view_proj = camera->m_projection * camera->m_view;
        m_global_uniforms.cam_pos   = glm::vec4(camera->m_position, 0.0f);

        glm::mat4 rotate = glm::mat4(1.0f);

        rotate = glm::rotate(rotate, glm::radians(m_projector_rotation), m_projector_dir);

        glm::vec4 rotated_axis = rotate * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);

        float ratio                = float(m_decal_textures[m_selected_decal]->height()) / float(m_decal_textures[m_selected_decal]->width());
        float proportionate_height = m_projector_size * ratio;

        m_projector_view = glm::lookAt(m_projector_pos, m_hit_pos, glm::vec3(rotated_axis));
        m_projector_proj = glm::ortho(-m_projector_size, m_projector_size, -proportionate_height, proportionate_height, 0.1f, CAMERA_FAR_PLANE);

        if (m_hit_distance != INFINITY)
            m_global_uniforms.light_view_proj = m_projector_proj * m_projector_view;
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

    void update_camera()
    {
        dw::Camera* current = m_main_camera.get();

        float forward_delta = m_heading_speed * m_delta;
        float right_delta   = m_sideways_speed * m_delta;

        current->set_translation_delta(current->m_forward, forward_delta);
        current->set_translation_delta(current->m_right, right_delta);

        m_camera_x = m_mouse_delta_x * m_camera_sensitivity;
        m_camera_y = m_mouse_delta_y * m_camera_sensitivity;

        if (m_mouse_look)
        {
            // Activate Mouse Look
            current->set_rotatation_delta(glm::vec3((float)(m_camera_y),
                                                    (float)(m_camera_x),
                                                    (float)(0.0f)));
        }
        else
        {
            current->set_rotatation_delta(glm::vec3((float)(0),
                                                    (float)(0),
                                                    (float)(0)));
        }

        current->update();
        update_transforms(current);
    }

    // -----------------------------------------------------------------------------------------------------------------------------------

private:
    // General GPU resources.
    std::unique_ptr<dw::Shader> m_uv_space_vs;
    std::unique_ptr<dw::Shader> m_texture_init_fs;
    std::unique_ptr<dw::Shader> m_decal_project_fs;
    std::unique_ptr<dw::Shader> m_mesh_vs;
    std::unique_ptr<dw::Shader> m_mesh_fs;
    std::unique_ptr<dw::Shader> m_triangle_vs;
    std::unique_ptr<dw::Shader> m_visualize_fs;
    std::unique_ptr<dw::Shader> m_depth_vs;
    std::unique_ptr<dw::Shader> m_depth_fs;

    std::unique_ptr<dw::Program> m_texture_init_program;
    std::unique_ptr<dw::Program> m_decal_program;
    std::unique_ptr<dw::Program> m_mesh_program;
    std::unique_ptr<dw::Program> m_visualize_program;
    std::unique_ptr<dw::Program> m_depth_program;

    std::unique_ptr<dw::Texture2D>              m_albedo_texture;
    std::vector<std::unique_ptr<dw::Texture2D>> m_decal_textures;
    std::unique_ptr<dw::Texture2D>              m_depth_texture;

    std::unique_ptr<dw::Framebuffer> m_albedo_fbo;
    std::unique_ptr<dw::Framebuffer> m_depth_fbo;

    std::unique_ptr<dw::UniformBuffer> m_global_ubo;

    // Camera.
    std::unique_ptr<dw::Camera> m_main_camera;

    GlobalUniforms m_global_uniforms;

    // Scene
    dw::Mesh* m_mesh;
    glm::mat4 m_transform;

    // Camera controls.
    bool  m_mouse_look         = false;
    float m_heading_speed      = 0.0f;
    float m_sideways_speed     = 0.0f;
    float m_camera_sensitivity = 0.05f;
    float m_camera_speed       = 0.02f;
    bool  m_enable_dither      = true;
    bool  m_debug_gui          = true;

    // Embree structure
    RTCDevice           m_embree_device        = nullptr;
    RTCScene            m_embree_scene         = nullptr;
    RTCGeometry         m_embree_triangle_mesh = nullptr;
    RTCIntersectContext m_embree_intersect_context;

    // Last hit
    glm::vec3 m_hit_pos;
    glm::vec3 m_hit_normal;
    float     m_hit_distance = INFINITY;

    // Projector
    glm::vec3 m_projector_pos;
    glm::vec3 m_projector_dir;
    glm::mat4 m_projector_view;
    glm::mat4 m_projector_proj;
    float     m_projector_size     = 10.0f;
    float     m_projector_rotation = 0.0f;
    bool      m_requires_update    = false;

    // Debug
    bool    m_visualize_albedo_map         = true;
    bool    m_visualize_projection_frustum = false;
    bool    m_visualize_hit_point          = false;
    bool    m_enable_conservative_raster   = true;
    bool    m_randomize_decals             = true;
    int32_t m_selected_decal               = 0;

    // Camera orientation.
    float m_camera_x;
    float m_camera_y;
};

DW_DECLARE_MAIN(TextureSpaceDecals)
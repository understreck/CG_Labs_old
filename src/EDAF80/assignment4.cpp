#include "assignment4.hpp"
#include "parametric_shapes.hpp"

#include "config.hpp"
#include "core/Bonobo.h"
#include "core/FPSCamera.h"
#include "core/helpers.hpp"
#include "core/node.hpp"
#include "core/ShaderProgramManager.hpp"

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <tinyfiledialogs.h>

#include <clocale>
#include <stdexcept>

edaf80::Assignment4::Assignment4(WindowManager& windowManager) :
            mCamera(0.5f * glm::half_pi<float>(),
                    static_cast<float>(config::resolution_x)
                            / static_cast<float>(config::resolution_y),
                    0.01f,
                    1000.0f),
            inputHandler(),
            mWindowManager(windowManager),
            window(nullptr)
{
    WindowManager::WindowDatum window_datum{
            inputHandler,
            mCamera,
            config::resolution_x,
            config::resolution_y,
            0,
            0,
            0,
            0};

    window = mWindowManager.CreateGLFWWindow(
            "EDAF80: Assignment 4",
            window_datum,
            config::msaa_rate);
    if(window == nullptr) {
        throw std::runtime_error("Failed to get a window: aborting!");
    }

    bonobo::init();
}

edaf80::Assignment4::~Assignment4()
{
    bonobo::deinit();
}

void
edaf80::Assignment4::run()
{
    // Set up the camera
    mCamera.mWorld.SetTranslate(glm::vec3(-40.0f, 14.0f, 6.0f));
    mCamera.mWorld.LookAt(glm::vec3(0.0f));
    mCamera.mMouseSensitivity = glm::vec2(0.003f);
    mCamera.mMovementSpeed    = glm::vec3(3.0f);    // 3 m/s => 10.8 km/h
    auto camera_position      = mCamera.mWorld.GetTranslation();

    // Create the shader programs
    ShaderProgramManager program_manager;
    GLuint fallback_shader = 0u;
    program_manager.CreateAndRegisterProgram(
            "Fallback",
            {{ShaderType::vertex, "common/fallback.vert"},
             {ShaderType::fragment, "common/fallback.frag"}},
            fallback_shader);
    if(fallback_shader == 0u) {
        LogError("Failed to load fallback shader");
        return;
    }

    GLuint skybox_shader = 0u;
    program_manager.CreateAndRegisterProgram(
            "Skybox",
            {{ShaderType::vertex, "EDAF80/skybox.vert"},
             {ShaderType::fragment, "EDAF80/skybox.frag"}},
            skybox_shader);
    if(skybox_shader == 0u) {
        LogError("Failed to load skybox shader");
        return;
    }

    GLuint water_shader = 0u;
    program_manager.CreateAndRegisterProgram(
            "water",
            {{ShaderType::vertex, "EDAF80/water.vert"},
             {ShaderType::fragment, "EDAF80/water.frag"}},
            water_shader);
    if(water_shader == 0u) {
        LogError("Failed to load water shader");
        return;
    }

    float elapsed_time_s = 0.0f;

    auto const skybox_shape =
            parametric_shapes::createSphere(200.0f, 100u, 100u);
    if(skybox_shape.vao == 0u) {
        LogError("Failed to load skybox mesh");
        return;
    }

    auto const commonCubemapPath = std::string{"res/cubemaps/NissiBeach2/"};
    auto const skybox_texture    = bonobo::loadTextureCubeMap(
            commonCubemapPath + "posx.jpg",
            commonCubemapPath + "negx.jpg",
            commonCubemapPath + "posy.jpg",
            commonCubemapPath + "negy.jpg",
            commonCubemapPath + "posz.jpg",
            commonCubemapPath + "negz.jpg");
    if(skybox_texture == 0u) {
        LogError("Failed to load skybox texture");
        return;
    }

    auto const water_texture = bonobo::loadTexture2D("res/textures/waves.png");
    if(water_texture == 0u) {
        LogError("Failed to load water texture");
        return;
    }

    Node skybox;
    skybox.set_geometry(skybox_shape);
    skybox.add_texture("skybox", skybox_texture, GL_TEXTURE_CUBE_MAP);
    skybox.set_program(&skybox_shader, [&](GLuint program) {
        glUniformMatrix4fv(
                glGetUniformLocation(program, "center_on_camera"),
                1,
                false,
                glm::value_ptr(mCamera.mWorld.GetTranslationMatrix()));
    });

    auto constexpr waterHeight = 100.f;
    auto constexpr waterWidth  = 100.f;
    auto const quadMesh =
            parametric_shapes::createQuad(waterHeight, waterWidth, 1000, 1000);
    if(quadMesh.vao == 0u) {
        LogError("Failed to load quad mesh");
        return;
    }

    auto waveSharpness = 0.2f;

    auto const setWaterUniforms = [&elapsed_time_s,
                                   &waveSharpness,
                                   &mCamera = this->mCamera,
                                   waterHeight,
                                   waterWidth](GLuint program) {
        glUniform1fv(
                glGetUniformLocation(program, "elapsed_time_s"),
                1,
                &elapsed_time_s);
        glUniform1fv(
                glGetUniformLocation(program, "wave_sharpness"),
                1,
                &waveSharpness);
        glUniform1fv(glGetUniformLocation(program, "height"), 1, &waterHeight);
        glUniform1fv(glGetUniformLocation(program, "width"), 1, &waterWidth);
        glUniform3fv(
                glGetUniformLocation(program, "camera_position"),
                1,
                glm::value_ptr(mCamera.mWorld.GetTranslation()));
    };

    auto water = Node{};
    water.set_geometry(quadMesh);
    water.add_texture("normal_texture", water_texture, GL_TEXTURE_2D);
    water.add_texture("skybox_texture", skybox_texture, GL_TEXTURE_CUBE_MAP);
    water.set_program(&water_shader, setWaterUniforms);
    water.get_transform().Translate({-waterHeight / 2, 0.f, -waterWidth / 2});

    glClearDepthf(1.0f);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glEnable(GL_DEPTH_TEST);

    auto lastTime = std::chrono::high_resolution_clock::now();

    bool pause_animation        = true;
    bool use_orbit_camera       = false;
    auto cull_mode              = bonobo::cull_mode_t::disabled;
    auto polygon_mode           = bonobo::polygon_mode_t::fill;
    bool show_logs              = true;
    bool show_gui               = true;
    bool shader_reload_failed   = false;
    bool show_basis             = false;
    float basis_thickness_scale = 1.0f;
    float basis_length_scale    = 1.0f;

    changeCullMode(cull_mode);

    while(!glfwWindowShouldClose(window)) {
        auto const nowTime = std::chrono::high_resolution_clock::now();
        auto const deltaTimeUs =
                std::chrono::duration_cast<std::chrono::microseconds>(
                        nowTime - lastTime);
        lastTime = nowTime;
        if(!pause_animation) {
            elapsed_time_s += std::chrono::duration<float>(deltaTimeUs).count();
        }

        auto& io = ImGui::GetIO();
        inputHandler.SetUICapture(io.WantCaptureMouse, io.WantCaptureKeyboard);

        glfwPollEvents();
        inputHandler.Advance();
        mCamera.Update(deltaTimeUs, inputHandler);
        if(use_orbit_camera) {
            mCamera.mWorld.LookAt(glm::vec3(0.0f));
        }
        camera_position = mCamera.mWorld.GetTranslation();

        if(inputHandler.GetKeycodeState(GLFW_KEY_R) & JUST_PRESSED) {
            shader_reload_failed = !program_manager.ReloadAllPrograms();
            if(shader_reload_failed)
                tinyfd_notifyPopup(
                        "Shader Program Reload Error",
                        "An error occurred while reloading shader programs; see the logs for details.\n"
                        "Rendering is suspended until the issue is solved. Once fixed, just reload the shaders again.",
                        "error");
        }
        if(inputHandler.GetKeycodeState(GLFW_KEY_F3) & JUST_RELEASED)
            show_logs = !show_logs;
        if(inputHandler.GetKeycodeState(GLFW_KEY_F2) & JUST_RELEASED)
            show_gui = !show_gui;
        if(inputHandler.GetKeycodeState(GLFW_KEY_F11) & JUST_RELEASED)
            mWindowManager.ToggleFullscreenStatusForWindow(window);

        // Retrieve the actual framebuffer size: for HiDPI monitors,
        // you might end up with a framebuffer larger than what you
        // actually asked for. For example, if you ask for a 1920x1080
        // framebuffer, you might get a 3840x2160 one instead.
        // Also it might change as the user drags the window between
        // monitors with different DPIs, or if the fullscreen status is
        // being toggled.
        int framebuffer_width, framebuffer_height;
        glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
        glViewport(0, 0, framebuffer_width, framebuffer_height);

        //
        // Todo: If you need to handle inputs, you can do it here
        //

        mWindowManager.NewImGuiFrame();

        glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
        bonobo::changePolygonMode(polygon_mode);

        if(!shader_reload_failed) {
            skybox.render(mCamera.GetWorldToClipMatrix());
            water.render(mCamera.GetWorldToClipMatrix());
        }

        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        //
        // Todo: If you want a custom ImGUI window, you can set it up
        //       here
        //

        bool opened =
                ImGui::Begin("Scene Control", nullptr, ImGuiWindowFlags_None);
        if(opened) {
            ImGui::Checkbox("Pause animation", &pause_animation);
            ImGui::Checkbox("Use orbit camera", &use_orbit_camera);
            ImGui::SliderFloat("Wave steepness", &waveSharpness, 0.0f, 1.0f);
            ImGui::Separator();
            auto const cull_mode_changed =
                    bonobo::uiSelectCullMode("Cull mode", cull_mode);
            if(cull_mode_changed) {
                changeCullMode(cull_mode);
            }
            bonobo::uiSelectPolygonMode("Polygon mode", polygon_mode);
            ImGui::Separator();
            ImGui::Checkbox("Show basis", &show_basis);
            ImGui::SliderFloat(
                    "Basis thickness scale",
                    &basis_thickness_scale,
                    0.0f,
                    100.0f);
            ImGui::SliderFloat(
                    "Basis length scale",
                    &basis_length_scale,
                    0.0f,
                    100.0f);
        }
        ImGui::End();

        if(show_basis)
            bonobo::renderBasis(
                    basis_thickness_scale,
                    basis_length_scale,
                    mCamera.GetWorldToClipMatrix());
        if(show_logs)
            Log::View::Render();
        mWindowManager.RenderImGuiFrame(show_gui);

        glfwSwapBuffers(window);
    }
}

int
main()
{
    std::setlocale(LC_ALL, "");

    Bonobo framework;

    try {
        edaf80::Assignment4 assignment4(framework.GetWindowManager());
        assignment4.run();
    }
    catch(std::runtime_error const& e) {
        LogError(e.what());
    }
}

// SPDX-License-Identifier: Apache-2.0
#include "renderer.hpp"
#include "stats_bitmap.hpp"
#include <pocketvr/transport.hpp>
#include <android/choreographer.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <android/looper.h>
#include <android/surface_texture_jni.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <cstring>
#include <cstdio>
#ifndef EGL_MUTABLE_RENDER_BUFFER_BIT_KHR
#define EGL_MUTABLE_RENDER_BUFFER_BIT_KHR 0x1000
#endif
#ifndef EGL_FRONT_BUFFER_AUTO_REFRESH_ANDROID
#define EGL_FRONT_BUFFER_AUTO_REFRESH_ANDROID 0x314C
#endif
#include <sys/resource.h>
#include <unistd.h>

namespace pocketvr::android {
namespace {
std::mutex display_mutex;
unsigned display_users{};
EGLDisplay shared_display = EGL_NO_DISPLAY;
EGLDisplay acquire_display() {
    std::lock_guard lock(display_mutex);
    if (!display_users) {
        shared_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (shared_display == EGL_NO_DISPLAY || !eglInitialize(shared_display, nullptr, nullptr))
            throw std::runtime_error("EGL initialization failed");
    }
    ++display_users;
    return shared_display;
}
void release_display() {
    std::lock_guard lock(display_mutex);
    if (display_users && --display_users == 0) {
        eglTerminate(shared_display);
        shared_display = EGL_NO_DISPLAY;
    }
}
constexpr const char *vertex_shader = R"glsl(#version 300 es
layout(location=0) in vec2 position;
layout(location=1) in vec2 ray;
out vec3 eyeRay;
out vec2 screen;
void main() { gl_Position=vec4(position,0,1); eyeRay=vec3(ray,-1); screen=position; }
)glsl";
constexpr const char *fragment_shader = R"glsl(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
uniform samplerExternalOES video;
uniform mat4 textureTransform;
uniform vec4 rotation;
uniform vec4 projection;
uniform vec4 renderProjection;
uniform vec4 vignette;
uniform float eye;
uniform int edgeMode;
uniform bool chromatic;
uniform bool gridMode;
uniform bool videoReady;
uniform sampler2D statsBitmap;
uniform int statsMode;
uniform float hudHeight;
in vec3 eyeRay;
in vec2 screen;
out vec4 color;
vec3 rotateRay(vec3 v) { return v+2.0*cross(rotation.xyz,cross(rotation.xyz,v)+rotation.w*v); }
vec2 bounded(vec2 uv) {
    if(edgeMode==2) return 1.0-abs(mod(uv,2.0)-1.0);
    return clamp(uv,vec2(0.001),vec2(0.999));
}
vec3 sampleEye(vec2 uv) {
    // Black mode clips the fixed optical aperture in main(). A late rotation
    // that exhausts guard pixels extends this eye's edge instead of creating
    // another black rectangle that moves with every pose/frame update.
    uv=bounded(uv);
    // Projection UVs use top-left origin; SurfaceTexture's matrix applies the
    // producer's crop/orientation to OpenGL's bottom-left texture coordinates.
    vec2 source=vec2((uv.x+eye)*0.5,1.0-uv.y);
    return texture(video,(textureTransform*vec4(source,0,1)).xy).rgb;
}
void main() {
    vec3 ray=gridMode?eyeRay:rotateRay(eyeRay);
    if(ray.z>=-0.00001) { color=vec4(0,0,0,1); return; }
    vec4 sourceProjection=gridMode?projection:renderProjection;
    vec2 uv=vec2((ray.x/-ray.z+sourceProjection.x)/(sourceProjection.x+sourceProjection.y),
                 (sourceProjection.z-ray.y/-ray.z)/(sourceProjection.z+sourceProjection.w));
    // Clip the optical aperture in fixed screen coordinates, before rotation.
    // Previously its black edge followed the reprojected texture with the gyro.
    vec2 aperture=vec2((eyeRay.x+projection.x)/(projection.x+projection.y),
                       (projection.z-eyeRay.y)/(projection.z+projection.w));
    if(edgeMode==0 &&
       (any(lessThan(aperture,vec2(0))) || any(greaterThan(aperture,vec2(1))))) {
        color=vec4(0,0,0,1); return;
    }
    vec3 rgb;
    if(gridMode) {
        vec2 lines=abs(fract(uv*10.0+0.5)-0.5);
        float line=1.0-smoothstep(0.012,0.03,min(lines.x,lines.y));
        rgb=mix(vec3(0.015,0.025,0.04),vec3(0.15,0.8,0.9),line);
        if(length(uv-vec2(0.5))<0.012) rgb=vec3(1,0.3,0.1);
    } else if(!videoReady) rgb=vec3(0);
    else if(chromatic) {
        vec2 d=(uv-vec2(0.5))*0.002;
        rgb=vec3(sampleEye(uv+d).r,sampleEye(uv).g,sampleEye(uv-d).b);
    } else rgb=sampleEye(uv);
    float radius=length(screen);
    float shade=smoothstep(vignette.y,max(vignette.y+vignette.z,vignette.y+0.001),radius);
    color=vec4(rgb*(1.0-vignette.x*shade),1);
    // Compose in undistorted eye space so both eyes see the same lens-corrected
    // panel. It stays head-locked rather than rotating with the video frame.
    vec2 hud=(vec2((eyeRay.x/-eyeRay.z+projection.x)/(projection.x+projection.y),
                   (projection.z-eyeRay.y/-eyeRay.z)/(projection.z+projection.w))
              -vec2(0.1,0.15))/vec2(0.8,hudHeight);
    float rows=statsMode==1?0.5:1.0;
    if(statsMode>0 && hud.x>=0.0 && hud.x<1.0 && hud.y>=0.0 && hud.y<rows) {
        float ink=texture(statsBitmap,hud).r;
        color.rgb=mix(color.rgb*0.15,vec3(0.4,1.0,0.75),ink);
    }
}
)glsl";
GLuint shader(GLenum kind, const char *source) {
    const auto result = glCreateShader(kind);
    glShaderSource(result, 1, &source, nullptr);
    glCompileShader(result);
    GLint ok{};
    glGetShaderiv(result, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        std::array<char, 1024> log{};
        glGetShaderInfoLog(result, static_cast<GLsizei>(log.size()), nullptr, log.data());
        glDeleteShader(result);
        throw std::runtime_error(std::string("Renderer shader: ") + log.data());
    }
    return result;
}
struct Cadence {
    int64_t timestamp{};
    bool ready{};
    static void tick(int64_t time, void *context) {
        auto &self = *static_cast<Cadence *>(context);
        self.timestamp = time;
        self.ready = true;
    }
};
} // namespace
Renderer::Renderer(JNIEnv *env, jobject texture, std::shared_ptr<ANativeWindow> display,
                   bool single_buffer)
    : display_(std::move(display)), single_buffer_(single_buffer) {
    if (!display_ || !texture || env->GetJavaVM(&vm_) != JNI_OK)
        throw std::invalid_argument("Renderer surfaces are unavailable");
    texture_ref_ = env->NewGlobalRef(texture);
    texture_ = ASurfaceTexture_fromSurfaceTexture(env, texture);
    if (texture_)
        decoder_window_ = std::shared_ptr<ANativeWindow>(
            ASurfaceTexture_acquireANativeWindow(texture_), ANativeWindow_release);
    if (!texture_ref_ || !texture_ || !decoder_window_) {
        if (texture_)
            ASurfaceTexture_release(texture_);
        if (texture_ref_)
            env->DeleteGlobalRef(texture_ref_);
        throw std::runtime_error("Cannot acquire decoder SurfaceTexture");
    }
    configure(Settings{}, 1);
    worker_ = Worker([this](StopToken token) { run(token); });
}
Renderer::~Renderer() {
    stop_.store(true);
    worker_.request_stop();
    if (worker_.joinable())
        worker_.join();
    decoder_window_.reset();
    if (texture_)
        ASurfaceTexture_release(texture_);
    JNIEnv *env{};
    bool attached = false;
    if (vm_->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        attached = vm_->AttachCurrentThread(&env, nullptr) == JNI_OK;
    }
    if (env && texture_ref_)
        env->DeleteGlobalRef(texture_ref_);
    if (attached)
        vm_->DetachCurrentThread();
}
void Renderer::configure(const Settings &settings, uint32_t projection_id) {
    Config next;
    next.geometry = settings.viewer_geometry();
    next.geometry.id = projection_id;
    next.lens = {settings.get<double>("lens.k1"), settings.get<double>("lens.k2")};
    next.strength = settings.get<float>("display.vignette_strength");
    next.size = settings.get<float>("display.vignette_size");
    next.softness = settings.get<float>("display.vignette_softness");
    next.dynamic = settings.get<bool>("display.vignette_dynamic");
    next.chromatic = settings.get<bool>("display.chromatic_correction");
    next.reprojection = settings.get<bool>("reprojection.enabled");
    const auto edge = settings.get<std::string>("display.edge_mode");
    next.edge = edge == "mirror" ? 2 : edge == "clamp" ? 1 : 0;
    const auto stats = settings.get<std::string>("ui.stats_overlay");
    next.stats = stats == "full" ? 2 : stats == "minimal" ? 1 : 0;
    std::lock_guard lock(config_mutex_);
    next.revision = config_.revision + 1;
    config_ = next;
}
void Renderer::tracking(std::weak_ptr<Sensors> sensors) {
    std::lock_guard lock(config_mutex_);
    sensors_ = std::move(sensors);
}
void Renderer::arrived() {
    // Choreographer timestamps use CLOCK_MONOTONIC, which monotonic_ns() does not.
    timespec clock{};
    if (clock_gettime(CLOCK_MONOTONIC, &clock))
        return;
    const auto now = static_cast<uint64_t>(clock.tv_sec) * 1'000'000'000 + static_cast<uint64_t>(clock.tv_nsec);
    const auto vsync = vsync_ns_.load();
    const auto period = static_cast<uint64_t>(std::llround(1e9 / display_hz_.load()));
    if (!vsync || now < vsync)
        return;
    const auto lead = period - (now - vsync) % period;
    std::lock_guard lock(arrival_mutex_);
    arrivals_[arrivals_seen_++ % arrivals_.size()] = static_cast<float>(static_cast<double>(lead) / 1e6);
}
nlohmann::json Renderer::status() const {
    std::array<float, 64> recent{};
    size_t samples{};
    {
        std::lock_guard lock(arrival_mutex_);
        samples = static_cast<size_t>(std::min<uint64_t>(arrivals_seen_, recent.size()));
        recent = arrivals_;
    }
    double lead{};
    if (samples) {
        std::nth_element(recent.begin(), recent.begin() + samples / 2, recent.begin() + samples);
        lead = recent[samples / 2];
    }
    std::lock_guard lock(error_mutex_);
    return {{"running", running_.load()},     {"draws", draws_.load()},
            {"arrival_lead_ms", lead},        {"arrival_samples", samples},
            {"latched", latched_.load()},     {"repeated", repeated_.load()},
            {"missed_vsync", missed_.load()}, {"frame_id", frame_id_.load()},
            {"render_ms", render_ms_.load()}, {"display_fps", display_fps_.load()},
            {"video_fps", video_fps_.load()}, {"stats_mode", stats_mode_.load()},
            {"grid", grid_.load()}, {"cooling", cooling_.load()},
            {"single_buffer", single_buffer_active_.load()}, {"error", error_}};
}
void Renderer::run(StopToken token) noexcept {
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    ALooper *looper{};
    GLuint program{}, vertex{}, fragment{}, texture{}, vao{}, vertices{}, indices{},
        stats_texture{};
    bool attached{};
    try {
        (void)setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), -8);
        looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
        if (!looper)
            throw std::runtime_error("Renderer looper unavailable");
        ALooper_acquire(looper);
        egl_display = acquire_display();
        const auto *extensions = eglQueryString(egl_display, EGL_EXTENSIONS);
        bool single_buffer = single_buffer_ && extensions &&
                             std::strstr(extensions, "EGL_KHR_mutable_render_buffer");
        EGLint attributes[]{EGL_RENDERABLE_TYPE,
                                  EGL_OPENGL_ES3_BIT,
                                  EGL_SURFACE_TYPE,
                                  single_buffer ? EGL_WINDOW_BIT | EGL_MUTABLE_RENDER_BUFFER_BIT_KHR
                                                : EGL_WINDOW_BIT,
                                  EGL_RED_SIZE,
                                  8,
                                  EGL_GREEN_SIZE,
                                  8,
                                  EGL_BLUE_SIZE,
                                  8,
                                  EGL_ALPHA_SIZE,
                                  8,
                                  EGL_NONE};
        EGLConfig egl_config{};
        EGLint count{};
        if (single_buffer && (!eglChooseConfig(egl_display, attributes, &egl_config, 1, &count) || !count)) {
            single_buffer = false;
            attributes[3] = EGL_WINDOW_BIT;
        }
        if (!single_buffer && (!eglChooseConfig(egl_display, attributes, &egl_config, 1, &count) || !count))
            throw std::runtime_error("GLES3 window config unavailable");
        const EGLint context_attributes[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attributes);
        surface = eglCreateWindowSurface(egl_display, egl_config, display_.get(), nullptr);
        if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
            !eglMakeCurrent(egl_display, surface, surface, context) ||
            !eglSwapInterval(egl_display, 1))
            throw std::runtime_error("Cannot create renderer window/context");
        single_buffer = single_buffer && eglSurfaceAttrib(egl_display, surface, EGL_RENDER_BUFFER, EGL_SINGLE_BUFFER);
        if (single_buffer) {
            // Without this the compositor only shows the buffer when told to.
            (void)eglSurfaceAttrib(egl_display, surface, EGL_FRONT_BUFFER_AUTO_REFRESH_ANDROID, EGL_TRUE);
            (void)eglSwapBuffers(egl_display, surface); // The new mode starts at the next swap.
        }
        single_buffer_active_.store(single_buffer);
        // Resolve the API30 hint dynamically so the API29 client can still load.
        using SetFrameRate = int32_t (*)(ANativeWindow *, float, int8_t);
        const auto set_frame_rate =
            reinterpret_cast<SetFrameRate>(dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate"));
        if (set_frame_rate)
            (void)set_frame_rate(display_.get(), 60.0F,
                                 ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
        glGenTextures(1, &texture);
        if (ASurfaceTexture_attachToGLContext(texture_, texture) != 0)
            throw std::runtime_error("Cannot attach decoder texture to GLES");
        attached = true;
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        vertex = shader(GL_VERTEX_SHADER, vertex_shader);
        fragment = shader(GL_FRAGMENT_SHADER, fragment_shader);
        program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        GLint linked{};
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked)
            throw std::runtime_error("Renderer program link failed");
        glUseProgram(program);
        const auto rotation_uniform = glGetUniformLocation(program, "rotation");
        const auto projection_uniform = glGetUniformLocation(program, "projection");
        const auto render_projection_uniform = glGetUniformLocation(program, "renderProjection");
        const auto transform_uniform = glGetUniformLocation(program, "textureTransform");
        const auto vignette_uniform = glGetUniformLocation(program, "vignette");
        const auto eye_uniform = glGetUniformLocation(program, "eye");
        const auto edge_uniform = glGetUniformLocation(program, "edgeMode");
        const auto chromatic_uniform = glGetUniformLocation(program, "chromatic");
        const auto grid_uniform = glGetUniformLocation(program, "gridMode");
        const auto video_uniform = glGetUniformLocation(program, "videoReady");
        const auto stats_uniform = glGetUniformLocation(program, "statsMode");
        const auto hud_height_uniform = glGetUniformLocation(program, "hudHeight");
        glUniform1i(glGetUniformLocation(program, "video"), 0);
        glUniform1i(glGetUniformLocation(program, "statsBitmap"), 1);
        StatsBitmap bitmap;
        glGenTextures(1, &stats_texture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, stats_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, StatsBitmap::width, StatsBitmap::height, 0, GL_RED,
                     GL_UNSIGNED_BYTE, bitmap.pixels.data());
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vertices);
        glBindBuffer(GL_ARRAY_BUFFER, vertices);
        constexpr unsigned columns = 40, rows = 40;
        constexpr size_t eye_vertices = (columns + 1) * (rows + 1);
        std::array<MeshVertex, eye_vertices> mesh{};
        int mesh_view = 0;
        double mesh_size = 1;
        uint64_t last_menu = 0;
        glBufferData(GL_ARRAY_BUFFER, 2 * eye_vertices * sizeof(MeshVertex), nullptr,
                     GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(MeshVertex), nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                              reinterpret_cast<void *>(2 * sizeof(float)));
        std::array<uint16_t, columns * rows * 6> mesh_indices{};
        size_t index{};
        for (unsigned y = 0; y < rows; ++y)
            for (unsigned x = 0; x < columns; ++x) {
                const auto a = static_cast<uint16_t>(y * (columns + 1) + x);
                for (const auto i :
                     {a, static_cast<uint16_t>(a + 1), static_cast<uint16_t>(a + columns + 1),
                      static_cast<uint16_t>(a + 1), static_cast<uint16_t>(a + columns + 2),
                      static_cast<uint16_t>(a + columns + 1)})
                    mesh_indices[index++] = i;
            }
        glGenBuffers(1, &indices);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(mesh_indices), mesh_indices.data(),
                     GL_STATIC_DRAW);
        const auto presentation = reinterpret_cast<PFNEGLPRESENTATIONTIMEANDROIDPROC>(
            eglGetProcAddress("eglPresentationTimeANDROID"));
        auto *choreographer = AChoreographer_getInstance();
        if (!choreographer)
            throw std::runtime_error("Native display cadence unavailable");
        Cadence cadence;
        AChoreographer_postFrameCallback64(choreographer, Cadence::tick, &cadence);
        Config current;
        std::weak_ptr<Sensors> sensors;
        std::optional<SurfaceFrame> displayed;
        uint64_t last_vsync{}, last_timestamp{}, last_draw_id{};
        uint64_t sample_ns{}, sample_draws{}, sample_latched{};
        int last_stats = -1;
        bool last_calibrating = false;
        bool last_cooling = false;
        std::array<float, 16> transform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        running_.store(true);
        while (!token.stop_requested() && !stop_.load()) {
            (void)ALooper_pollOnce(5, nullptr, nullptr, nullptr);
            if (!cadence.ready)
                continue;
            cadence.ready = false;
            AChoreographer_postFrameCallback64(choreographer, Cadence::tick, &cadence);
            const auto started = monotonic_ns();
            const auto vsync =
                cadence.timestamp > 0 ? static_cast<uint64_t>(cadence.timestamp) : started;
            const auto period = static_cast<uint64_t>(std::llround(1e9 / display_hz_.load()));
            if (last_vsync && vsync > last_vsync + period * 3 / 2)
                missed_.fetch_add(std::max<uint64_t>(1, (vsync - last_vsync + period / 2) / period) - 1);
            last_vsync = vsync;
            if (cadence.timestamp > 0)
                vsync_ns_.store(vsync);
            Config next = current;
            if (config_mutex_.try_lock()) {
                next = config_;
                sensors = sensors_;
                config_mutex_.unlock();
            }
            const int view = view_mode_.load();
            const double size = view_size_.load();
            if (next.revision != current.revision || view != mesh_view || size != mesh_size) {
                current = next;
                mesh_view = view;
                mesh_size = size;
                // Mesh rebuilds occur only for a settings change, not per draw.
                for (size_t eye = 0; eye < 2 && current.revision; ++eye) {
                    const Lens flat{0, 0};
                    lens_mesh_into(view == 2 ? flat : current.lens,
                                   view == 0 ? current.geometry.screen[eye] : current.geometry.eyes[eye],
                                   mesh, columns, rows);
                    if (view != 2)
                        scale_mesh_about_lens(mesh, view == 0 ? current.geometry.screen[eye] : current.geometry.eyes[eye], size);
                    glBufferSubData(GL_ARRAY_BUFFER,
                                    static_cast<GLintptr>(eye * eye_vertices * sizeof(MeshVertex)),
                                    static_cast<GLsizeiptr>(mesh.size() * sizeof(MeshVertex)),
                                    mesh.data());
                }
            }
            if (ASurfaceTexture_updateTexImage(texture_) != 0)
                throw std::runtime_error("Cannot latch decoded texture");
            const auto timestamp = ASurfaceTexture_getTimestamp(texture_);
            const auto matched = frames_.match(timestamp > 0 ? static_cast<uint64_t>(timestamp) : 0,
                                               session_.load());
            if (matched) {
                displayed = matched;
                if (matched->timestamp_ns != last_timestamp) {
                    latched_.fetch_add(1);
                    last_timestamp = matched->timestamp_ns;
                    // Never make the draw loop wait for a reader.
                    if (matched->received_ns) {
                        std::unique_lock lock(latency_mutex_, std::try_to_lock);
                        if (lock.owns_lock() && latency_.size() < 120)
                            latency_.push_back({matched->header.frame_id, matched->received_ns, matched->queued_ns,
                                                matched->timestamp_ns, monotonic_ns()});
                    }
                }
                ASurfaceTexture_getTransformMatrix(texture_, transform.data());
            } else if (!displayed ||
                       displayed->timestamp_ns !=
                           static_cast<uint64_t>(std::max<int64_t>(0, timestamp)) ||
                       displayed->session != session_.load())
                displayed.reset();
            EGLint width{}, height{};
            if (!eglQuerySurface(egl_display, surface, EGL_WIDTH, &width) ||
                !eglQuerySurface(egl_display, surface, EGL_HEIGHT, &height) || width < 2 ||
                height < 1)
                throw std::runtime_error("Renderer window dimensions unavailable");
            glViewport(0, 0, width, height);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            const bool grid = grid_.load();
            const bool video_ready =
                displayed && displayed->header.projection_id == current.geometry.id;
            PoseSnapshot pose;
            bool calibrating = false;
            if (const auto source = sensors.lock()) {
                pose = source->pose();
                calibrating = source->calibrating();
            }
#ifdef POCKETVR_NATIVE_TESTS
            if (!pose.tracking)
                pose = test_pose_.read();
            calibrating = calibrating || test_calibrating_.load();
#endif
            const int override = stats_override_.load();
            const bool cooling = cooling_.load();
            std::vector<std::string> menu_lines;
            const auto menu_seen = menu_revision_.load();
            {
                std::unique_lock lock(menu_mutex_, std::try_to_lock);
                if (lock.owns_lock() && !menu_.empty())
                    menu_lines = menu_;
            }
            const int stats = !menu_lines.empty() ? 2 : (calibrating || cooling) ? 1 : override < 0 ? current.stats : override;
            stats_mode_.store(stats);
            if (!sample_ns || started - sample_ns >= 500'000'000 || stats != last_stats ||
                calibrating != last_calibrating || cooling != last_cooling || menu_seen != last_menu) {
                last_menu = menu_seen;
                const auto count = draws_.load(), latched = latched_.load();
                if (sample_ns && started > sample_ns) {
                    const auto seconds = static_cast<double>(started - sample_ns) / 1e9;
                    display_fps_.store(static_cast<double>(count - sample_draws) / seconds);
                    video_fps_.store(static_cast<double>(latched - sample_latched) / seconds);
                }
                sample_ns = started;
                sample_draws = count;
                sample_latched = latched;
                last_stats = stats;
                last_calibrating = calibrating;
                last_cooling = cooling;
                if (stats) {
                    bitmap.pixels.fill(0);
                    std::array<char, 128> line{};
                    std::snprintf(line.data(), line.size(), "DRAW FPS %.0f VIDEO FPS %.0f",
                                  display_fps_.load(), video_fps_.load());
                    bitmap.line(0, line.data());
                    std::snprintf(line.data(), line.size(), "CPU DRAW-SWAP %.1f MS",
                                  render_ms_.load());
                    bitmap.line(1, line.data());
                    std::snprintf(line.data(), line.size(), "REPEAT %llu MISS %llu",
                                  static_cast<unsigned long long>(repeated_.load()),
                                  static_cast<unsigned long long>(missed_.load()));
                    bitmap.line(2, line.data());
                    if (pose.tracking && pose.timestamp_ns && started >= pose.timestamp_ns) {
                        std::snprintf(line.data(), line.size(), "SENSOR AGE %.0f MS",
                                      static_cast<double>(started - pose.timestamp_ns) / 1e6);
                        bitmap.line(3, line.data());
                    } else
                        bitmap.line(3, "SENSOR AGE --");
                    if (cooling) {
                        bitmap.pixels.fill(0);
                        bitmap.line(0, "COOLING DOWN");
                        bitmap.line(1, "VIDEO PAUSED - TRACKING ACTIVE");
                    } else if (calibrating) {
                        bitmap.pixels.fill(0);
                        bitmap.line(0, "HOLD PHONE STILL");
                        bitmap.line(1, "GYRO CALIBRATION - 2 SECONDS");
                    }
                    if (!menu_lines.empty()) {
                        bitmap.pixels.fill(0);
                        for (size_t row = 0; row < menu_lines.size() && row < 4; ++row)
                            bitmap.line(static_cast<unsigned>(row), menu_lines[row]);
                    }
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D, stats_texture);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, StatsBitmap::width, StatsBitmap::height,
                                    GL_RED, GL_UNSIGNED_BYTE, bitmap.pixels.data());
                }
            }
            glUniform1i(stats_uniform, stats);
            glUniform1i(video_uniform, video_ready && !cooling);
            if (current.revision && (grid || video_ready || calibrating || cooling)) {
                const auto rotation =
                    !grid && video_ready
                        ? late_surface_rotation(*displayed, pose, started + period / 2,
                                                current.reprojection)
                        : Quat{};
                glUniform4f(rotation_uniform, static_cast<float>(rotation.x),
                            static_cast<float>(rotation.y), static_cast<float>(rotation.z),
                            static_cast<float>(rotation.w));
                const auto strength =
                    current.dynamic
                        ? std::min(1.0F, current.strength +
                                             static_cast<float>(pose.pose.angular_velocity.norm()) *
                                                 0.03F)
                        : current.strength;
                glUniform4f(vignette_uniform, view == 2 ? 0.0F : strength, current.size, current.softness, 0);
                glUniformMatrix4fv(transform_uniform, 1, GL_FALSE, transform.data());
                glUniform1i(edge_uniform, current.edge);
                glUniform1i(chromatic_uniform, current.chromatic);
                glUniform1i(grid_uniform, grid);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
                for (unsigned eye = 0; eye < (view == 2 ? 1U : 2U); ++eye) {
                    const auto &p = current.geometry.eyes[eye];
                    const auto eye_width = eye == 0 ? width / 2 : width - width / 2;
                    glViewport(view == 2 ? width / 4 : eye == 0 ? 0 : width / 2, 0, eye_width, height);
                    glUniform1f(eye_uniform, static_cast<float>(eye));
                    glUniform1f(hud_height_uniform,
                                0.1F * static_cast<float>(eye_width) / static_cast<float>(height));
                    glUniform4f(projection_uniform, static_cast<float>(p.left),
                                static_cast<float>(p.right), static_cast<float>(p.up),
                                static_cast<float>(p.down));
                    const auto render = guarded_projection(p);
                    glUniform4f(render_projection_uniform, static_cast<float>(render.left),
                                static_cast<float>(render.right), static_cast<float>(render.up),
                                static_cast<float>(render.down));
                    const auto offset = eye * eye_vertices * sizeof(MeshVertex);
                    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                                          reinterpret_cast<void *>(offset));
                    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                                          reinterpret_cast<void *>(offset + 2 * sizeof(float)));
                    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh_indices.size()),
                                   GL_UNSIGNED_SHORT, nullptr);
                }
                if (!grid && video_ready) {
                    frame_id_.store(displayed->header.frame_id);
                    if (last_draw_id == displayed->header.frame_id)
                        repeated_.fetch_add(1);
                    last_draw_id = displayed->header.frame_id;
                }
            }
            if (glGetError() != GL_NO_ERROR)
                throw std::runtime_error("GLES renderer reported an error");
            if (presentation && !single_buffer)
                (void)presentation(egl_display, surface,
                                   static_cast<EGLnsecsANDROID>(vsync + period));
            if (!eglSwapBuffers(egl_display, surface))
                throw std::runtime_error("Renderer swap failed");
            draws_.fetch_add(1);
            render_ms_.store(static_cast<double>(monotonic_ns() - started) / 1e6);
        }
    } catch (const std::exception &error) {
        std::lock_guard lock(error_mutex_);
        error_ = error.what();
    } catch (...) {
        std::lock_guard lock(error_mutex_);
        error_ = "Renderer failed";
    }
    running_.store(false);
    if (context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE) {
        if (attached)
            (void)ASurfaceTexture_detachFromGLContext(texture_);
        if (indices)
            glDeleteBuffers(1, &indices);
        if (stats_texture)
            glDeleteTextures(1, &stats_texture);
        if (vertices)
            glDeleteBuffers(1, &vertices);
        if (vao)
            glDeleteVertexArrays(1, &vao);
        if (program)
            glDeleteProgram(program);
        if (vertex)
            glDeleteShader(vertex);
        if (fragment)
            glDeleteShader(fragment);
    }
    if (egl_display != EGL_NO_DISPLAY) {
        (void)eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE)
            eglDestroySurface(egl_display, surface);
        if (context != EGL_NO_CONTEXT)
            eglDestroyContext(egl_display, context);
        release_display();
    }
    if (looper)
        ALooper_release(looper);
}
} // namespace pocketvr::android

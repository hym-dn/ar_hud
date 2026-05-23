# ═══════════════════════════════════════════════════════════════════════════
# ARHud 引擎 — Conan 2.x 依赖管理
# ═══════════════════════════════════════════════════════════════════════════
#
# 用法：
#   cd <project-root>
#   conan install . --build=missing -s build_type=Debug
#   cmake --preset conan-default
#   cmake --build build/Debug
#
# 架构参考：
#   当前阶段仅需轻量依赖：OpenGL 3.3 Core、数学库、字体、图片加载。
#   后续 Vulkan 路径可在此 recipe 中添加 vulkan-loader、shaderc 等。
#
# ═══════════════════════════════════════════════════════════════════════════

from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, cmake_layout

class ARHudConan(ConanFile):
    """ARHud 引擎 Conan 配方

    管理所有第三方依赖的版本和构建配置。
    使用 CMakeDeps + CMakeToolchain 生成器，与 CMakeLists.txt 无缝集成。
    """

    # ── 项目元数据 ────────────────────────────────────────────────────
    name = "arhud"
    version = "0.1.0"
    description = "AR Head-Up Display Rendering Engine"
    license = "MIT"
    author = "yameng.he"
    homepage = ""
    url = ""
    
    # ── 包类型 ────────────────────────────────────────────────────────
    # application：最终可执行文件
    # library：供其他 Conan 包消费（暂不需要，设为 application）
    package_type = "application"

    # ── 设置 ───────────────────────────────────────────────────────────
    # 从 Conan 继承标准设置：编译器、构建类型、架构等
    # 通过 `conan install -s ...` 传递
    settings = "os", "compiler", "build_type", "arch"

    # ── 选项（与 CMakeLists.txt 的 option 对应） ──────────────────────
    options = {
        # 图形 API 后端：当前只有 OpenGL，Vulkan 预留
        "api_opengl": [True, False],
        "api_vulkan": [True, False],
        # 构建测试
        "build_tests": [True, False],
        # 共享库
        "shared": [True, False],
        # fPIC（位置无关代码，静态库链接到共享库时需要）
        "fPIC": [True, False],
    }

    # ── 默认选项 ──────────────────────────────────────────────────────
    default_options = {
        "api_opengl": True,
        "api_vulkan": False,
        "build_tests": False,
        "shared": False,
        "fPIC": True,
    }

    # ── 生成器 ─────────────────────────────────────────────────────────
    #
    # CMakeDeps：为每个依赖生成 find_package 配置文件
    #   → CMakeLists.txt 中可直接 find_package(glm) 使用
    #
    # CMakeToolchain：生成 CMakePresets.json + toolchain 文件
    #   → 自动设置 CMAKE_BUILD_TYPE、CMAKE_CXX_STANDARD、编译器路径等
    #
    generators = "CMakeDeps"

    # ═══════════════════════════════════════════════════════════════════════
    # 依赖声明
    # ═══════════════════════════════════════════════════════════════════════

    def requirements(self):
        """声明 Conan 管理的第三方依赖

        每个 Conan 包会自动处理各自的传递依赖。
        使用 revisions（#...）锁定版本哈希，确保可复现构建。
        """

        # ── GLM（数学库，header-only） ────────────────────────────────
        # 轻量 vec2/vec3/mat4/rect 等，引擎所有模块依赖
        self.requires("glm/1.0.1")

        # ── GLAD（OpenGL 函数加载器） ─────────────────────────────────
        # 运行时加载 OpenGL 函数指针，无需链接 opengl32.lib
        # 生成 gl.h / gl.c，支持 OpenGL 3.3 Core + GLES 3.2
        self.requires("glad/2.0.8")

        # ── FreeType（字体渲染引擎） ──────────────────────────────────
        # 用于 SDF 字体图集生成（HUD 文字渲染核心依赖）
        self.requires("freetype/2.13.3")

        # ── STB（单头文件图片库） ────────────────────────────────────
        # stb_image.h — PNG/JPG 解码（图标、背景图加载）
        # stb_truetype.h — FreeType 的轻量替代（可选的 fallback）
        self.requires("stb/cci.20240531")

        # ── GLFW3（窗口管理，仅开发/测试） ───────────────────────────
        # 车规平台（QNX）使用 Screen API 替代 GLFW，因此设 visible=False
        # 让 consumer（主应用）决定是否链接
        # 当前 core 库不依赖 glfw，引擎层在创建窗口时才需要
        if self.options.build_tests:
            self.requires("glfw/3.4", visible=True)
        else:
            self.requires("glfw/3.4", visible=False)

    def configure(self):
        """根据选项调整依赖配置"""

        self.options["glad"].shared = self.options.shared
        self.options["glad"].gl_profile = "core"
        self.options["glad"].gl_version = "3.3"

    def validate(self):
        """验证选项组合的合法性"""
        # OpenGL 和 Vulkan 至少启用一个
        if not self.options.api_opengl and not self.options.api_vulkan:
            raise ConanInvalidConfiguration(
                "At least one graphics API must be enabled "
                "(api_opengl or api_vulkan)"
            )

    # ═══════════════════════════════════════════════════════════════════════
    # 构建系统集成
    # ═══════════════════════════════════════════════════════════════════════

    def layout(self):
        """定义项目布局，使 Conan 知道源码、构建、输出目录的位置

        cmake_layout() 是标准布局：
          - 源码：project-root/
          - 构建：build/<os>-<arch>-<compiler>-<build_type>/
          - 生成器文件位于构建目录内
        """
        cmake_layout(self)

    def generate(self):
        """生成阶段：将 Conan 选项传递给 CMake

        通过 CMakeToolchain 生成器，Conan 会自动将 settings 和 options
        映射为 CMake 缓存变量。这里注入额外的自定义选项。
        """
        tc = CMakeToolchain(self)
        tc.variables["ARHUD_API_OPENGL"] = self.options.api_opengl
        tc.variables["ARHUD_API_VULKAN"] = self.options.api_vulkan
        tc.variables["ARHUD_BUILD_TESTS"] = self.options.build_tests
        tc.variables["BUILD_SHARED_LIBS"] = self.options.shared
        tc.generate()

    # ═══════════════════════════════════════════════════════════════════════
    # 打包（预留，当前不需要将引擎发布为 Conan 包）
    # ═══════════════════════════════════════════════════════════════════════

    def package_info(self):
        """设置从 Conan 消费本包时的 CMake 信息

        当前 package_type = "application" 意味着不被消费，
        此方法主要用于 arhud::arhud CMake target 的定义。
        """
        self.cpp_info.libs = ["arhud_core", "arhud_engine"]

add_rules("mode.debug", "mode.release")
set_policy("package.requires_lock", true)

add_requires("entt v3.16.0")

target("BedrockReplay")
    set_kind("shared")
    set_languages("c++20")
    set_strip("all")
    add_defines("ENTT_PACKED_PAGE=128", "ENTT_SPARSE_PAGE=2048", {public = true})

    add_files(
        "src/main.cpp",
        "src/core/GameHooks.cpp",
        "src/core/Runtime.cpp",
        "src/core/events/*.cpp",
        "src/core/memory/*.cpp",
        "src/replay/**.cpp",
        "src/runtime/*.cpp",
        "third_party/imgui/imgui.cpp",
        "third_party/imgui/imgui_draw.cpp",
        "third_party/imgui/imgui_tables.cpp",
        "third_party/imgui/imgui_widgets.cpp",
        "third_party/imgui/backends/imgui_impl_opengl3.cpp"
    )

    add_includedirs("include", {public = true})
    add_includedirs("src", "third_party", "third_party/imgui", "third_party/imgui/backends")
    add_packages("entt")

    if is_plat("android") then
        add_cxflags("-O2", "-fPIC", "-fexceptions", "-fvisibility=hidden")
        add_cxxflags("-fno-rtti", "-fvisibility-inlines-hidden")
        add_shflags(
            "-Wl,--exclude-libs,ALL",
            "-Wl,-z,max-page-size=16384",
            "-Wl,--version-script=" .. path.join(os.projectdir(), "exports.map"),
            {force = true}
        )
        add_linkdirs("lib")
        add_links("GlossHook", "android", "EGL", "GLESv3", "GLESv2", "mediandk", "dl", "z", "log")
    end

    after_build(function (target)
        if not target:is_plat("android") then return end
        import("lib.detect.find_tool")
        local python = find_tool("python3") or find_tool("python")
        local args = {}
        if not python then
            python = find_tool("py")
            if python then table.insert(args, "-3") end
        end
        assert(python, "Python 3 is required to package BedrockReplay.levipack")
        table.insert(args, path.join(os.projectdir(), "scripts", "package_levipack.py"))
        table.insert(args, "--library")
        table.insert(args, target:targetfile())
        table.insert(args, "--icon")
        local icon = path.join(os.projectdir(), "assets", "bedrockreplay.png")
        if not os.isfile(icon) then
            icon = path.join(os.projectdir(), "resources", "minecraft_resource_packs", "replay", "pack_icon.png")
        end
        table.insert(args, icon)
        table.insert(args, "--font")
        table.insert(args, path.join(os.projectdir(), "resources", "minecraft.ttf"))
        table.insert(args, "--resource-packs")
        table.insert(args, path.join(os.projectdir(), "resources", "minecraft_resource_packs"))
        table.insert(args, "--behavior-packs")
        table.insert(args, path.join(os.projectdir(), "resources", "minecraft_behavior_packs"))
        table.insert(args, "--version-header")
        table.insert(args, path.join(os.projectdir(), "include", "bedrocktools", "Version.hpp"))
        table.insert(args, "--output")
        table.insert(args, path.join(target:targetdir(), "BedrockReplay.levipack"))
        os.vrunv(python.program, args)
    end)

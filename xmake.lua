-- set minimum xmake version
set_xmakever("2.8.2")

-- includes
includes("lib/commonlibsse-ng")

-- set project
set_project("commonlibsse-ng-template")
set_version("0.0.0")
set_license("GPL-3.0")

-- set defaults
set_languages("c++23")
set_warnings("allextra")

-- set policies
set_policy("package.requires_lock", true)

-- add rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- main target (kept for metadata)
target("commonlibsse-ng-template")
    -- build as a shared library (DLL)
    set_kind("shared")
    set_basename("Interactive_Water_VR")

    -- add dependencies to target
    add_deps("commonlibsse-ng")

    -- add commonlibsse-ng plugin rule metadata (keeps manifest info)
    add_rules("commonlibsse-ng.plugin", {
        name = "commonlibsse-ng-template",
        author = "qudix",
        description = "SKSE64 plugin template using CommonLibSSE-NG"
    })

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

-- Explicit plugin target to guarantee a DLL is produced
target("skse_plugin")
    set_kind("shared")
    set_basename("Interactive_Water_VR")

    -- compile plugin sources
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    -- depend on commonlibsse-ng so its includes are provided
    add_deps("commonlibsse-ng")
    add_includedirs("lib/commonlibsse-ng/include")

    -- Point to local SKSE VR SDK (user-provided path)
    local skse_sdk = "C:/Users/user/Desktop/Gaming apps/skyrim vr mod tools folder/MODS/SKSE MOD DEV/sksevr_2_00_12/sksevr_2_00_12/src"
    if os.isdir(skse_sdk) then
        add_includedirs(skse_sdk)
        add_includedirs(path.join(skse_sdk, "sksevr"))
        add_includedirs(path.join(skse_sdk, "sksevr", "skse64"))

        -- Link against prebuilt libs inside the SDK x64 Release folder
        local libdir = path.join(skse_sdk, "sksevr", "x64", "Release")
        if os.isdir(libdir) then
            add_linkdirs(libdir)
            add_links("skse64_common", "skse64_loader_common")
        end

        -- Optionally link the compatibility lib if present
        local libdir2 = path.join(skse_sdk, "sksevr", "x64_v143", "Release")
        if os.isdir(libdir2) then
            add_linkdirs(libdir2)
            add_links("common_vc14")
        end
    end

    -- ensure MSVC uses C++23
    set_languages("c++23")
    add_defines{"_CRT_SECURE_NO_WARNINGS"}
    if is_mode("release") then
        set_symbols("debug")
    end

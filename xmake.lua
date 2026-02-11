add_rules("mode.debug", "mode.release")

-- External packages
add_requires("qoi")
add_requires("libpng")
add_requires("libsdl2")

-- CMT: Metal C bindings (custom package from git)
package("cmt")
    set_homepage("https://github.com/recp/cmt")
    set_description("C Bindings/Wrappers for Apple's Metal Graphics API")
    set_license("MIT")

    set_urls("https://github.com/recp/cmt.git")
    add_versions("latest", "master")

    on_install("macosx", function (package)
        os.cp("include/*", package:installdir("include"))
        os.cp("src/*", package:installdir("src"))
    end)

    on_test(function (package)
        assert(os.isfile(path.join(package:installdir("include"), "cmt", "cmt.h")))
    end)
package_end()

add_requires("cmt latest")

target("mandelbrotplay_c")
    set_kind("binary")
    set_languages("c17", "c++17")

    -- Core C sources
    add_files("src/*.c")
    add_files("src/mandelbrot/*.c")
    add_files("src/render/*.c")
    add_files("src/stream/*.c")
    add_files("src/viewer/*.c")
    add_files("src/gpu/*.c")
    add_files("src/gpu/*.m")

    -- CMT Objective-C sources (compiled from package install dir)
    -- Disable ARC for all Objective-C files (CMT requires manual memory management)
    on_load(function (target)
        local cmt = target:pkg("cmt")
        if cmt then
            local cmt_src = path.join(cmt:installdir(), "src")
            target:add("files", path.join(cmt_src, "*.m"))
            target:add("includedirs", path.join(cmt:installdir(), "include"))
        end
    end)

    -- Disable ARC for Objective-C files (CMT library compatibility)
    add_mxflags("-fno-objc-arc", {force = true})

    -- Frameworks (macOS Metal)
    if is_plat("macosx") then
        add_frameworks("Metal", "Foundation", "QuartzCore")
    end

    -- Packages
    add_packages("qoi", "libpng", "libsdl2", "cmt")
    add_includedirs("src")

    -- Optimization
    set_optimize("fastest")
    add_cxflags("-march=native", "-flto", {force = true})
    add_ldflags("-flto", {force = true})

-- Interactive viewer target (native macOS, no SDL2)
target("mandelbrot_interactive")
    set_kind("binary")
    set_languages("c17", "c++17")

    -- Main entry point
    add_files("src/viewer_native/main_interactive.c")

    -- Native viewer (Objective-C)
    add_files("src/viewer_native/*.m")

    -- Core compute modules
    add_files("src/mandelbrot/*.c")
    add_files("src/gpu/*.c")
    add_files("src/gpu/*.m")
    add_files("src/tile_cache/*.c")
    add_files("src/tile_map/*.c")
    add_files("src/compute/*.c")
    add_files("src/perturbation/*.c")

    -- CMT Objective-C sources (compiled from package install dir)
    on_load(function (target)
        local cmt = target:pkg("cmt")
        if cmt then
            local cmt_src = path.join(cmt:installdir(), "src")
            target:add("files", path.join(cmt_src, "*.m"))
            target:add("includedirs", path.join(cmt:installdir(), "include"))
        end
    end)

    -- Disable ARC for Objective-C files (CMT library compatibility)
    add_mxflags("-fno-objc-arc", {force = true})

    -- Frameworks (macOS only)
    if is_plat("macosx") then
        add_frameworks("Metal", "Foundation", "QuartzCore", "AppKit", "CoreVideo", "CoreText", "CoreGraphics")
    end

    -- Packages (no SDL2!)
    add_packages("cmt", "qoi")
    add_includedirs("src")

    -- Optimization
    set_optimize("fastest")
    add_cxflags("-march=native", "-flto", {force = true})
    add_ldflags("-flto", {force = true})

add_rules("mode.debug", "mode.release")

-- =============================================================================
-- Build Options
-- =============================================================================

option("strict")
    set_default(false)
    set_showmenu(true)
    set_description("Enable strict warnings (-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2)")
option_end()

option("werror")
    set_default(false)
    set_showmenu(true)
    set_description("Treat warnings as errors (-Werror)")
option_end()

option("asan")
    set_default(false)
    set_showmenu(true)
    set_description("Enable Address Sanitizer and Undefined Behavior Sanitizer")
option_end()

option("tsan")
    set_default(false)
    set_showmenu(true)
    set_description("Enable Thread Sanitizer")
option_end()

option("native")
    set_default(true)
    set_showmenu(true)
    set_description("Enable -march=native optimization")
option_end()

option("lto")
    set_default(true)
    set_showmenu(true)
    set_description("Enable Link-Time Optimization (release only)")
option_end()

-- =============================================================================
-- Metal Shader to C Header Rule
-- =============================================================================

rule("metal2header")
    set_extensions(".metal")
    before_build(function (target)
        local metal_file = path.join(os.projectdir(), "src/gpu/mandelbrot.metal")
        local header_file = path.join(os.projectdir(), "src/gpu/mandelbrot_shader.h")
        local script_file = path.join(os.projectdir(), "scripts/metal2header.sh")

        -- Check if header needs regeneration
        local need_regen = false
        if not os.isfile(header_file) then
            need_regen = true
        else
            local metal_mtime = os.mtime(metal_file)
            local header_mtime = os.mtime(header_file)
            if metal_mtime and header_mtime and metal_mtime > header_mtime then
                need_regen = true
            end
        end

        if need_regen then
            print("Regenerating Metal shader header...")
            os.execv("bash", {script_file, metal_file, header_file})
        end
    end)
rule_end()

-- =============================================================================
-- Build Flags Rule (applies options to all targets)
-- =============================================================================

rule("build_flags")
    on_load(function (target)
        local use_asan = get_config("asan")
        local use_tsan = get_config("tsan")
        local use_strict = get_config("strict")
        local use_werror = get_config("werror")
        local use_native = get_config("native")
        local use_lto = get_config("lto")
        local is_release = is_mode("release")

        -- Strict warnings
        if use_strict then
            target:add("cxflags", "-Wall", "-Wextra", "-Wpedantic", "-Wconversion", "-Wshadow", "-Wformat=2", {force = true})
        end

        -- Warnings as errors
        if use_werror then
            target:add("cxflags", "-Werror", {force = true})
        end

        -- Sanitizers (mutually exclusive with native/lto)
        if use_asan then
            target:add("cxflags", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", {force = true})
            target:add("ldflags", "-fsanitize=address,undefined", {force = true})
        elseif use_tsan then
            target:add("cxflags", "-fsanitize=thread", {force = true})
            target:add("ldflags", "-fsanitize=thread", {force = true})
        else
            -- Only apply native/lto when sanitizers are NOT active
            if use_native then
                target:add("cxflags", "-march=native", {force = true})
            end
            if use_lto and is_release then
                target:add("cxflags", "-flto", {force = true})
                target:add("ldflags", "-flto", {force = true})
            end
        end
    end)
rule_end()

-- =============================================================================
-- External Packages
-- =============================================================================

add_requires("qoi")
add_requires("libpng")
add_requires("libsdl2")
add_requires("gmp")
add_requires("mpfr")

-- CMT: Metal C bindings (custom package from git)
package("cmt")
    set_homepage("https://github.com/recp/cmt")
    set_description("C Bindings/Wrappers for Apple's Metal Graphics API")
    set_license("MIT")

    set_urls("https://github.com/recp/cmt.git")
    add_versions("2024.01", "fa15edabf8fa78798647f3018b7efc236390b688")

    on_install("macosx", function (package)
        os.cp("include/*", package:installdir("include"))
        os.cp("src/*", package:installdir("src"))
    end)

    on_test(function (package)
        assert(os.isfile(path.join(package:installdir("include"), "cmt", "cmt.h")))
    end)
package_end()

add_requires("cmt 2024.01")

-- =============================================================================
-- CLI Target
-- =============================================================================

target("mandelbrotplay_c")
    set_kind("binary")
    set_languages("c17", "c++17")
    add_rules("metal2header", "build_flags")
    -- Not a default target so bare `xmake run` opens only the viewer;
    -- still built by default because mandelbrot_interactive depends on it.
    set_default(false)

    -- Core C sources
    add_files("src/*.c")
    add_files("src/mandelbrot/*.c")
    add_files("src/stream/*.c")
    add_files("src/viewer/*.c")
    add_files("src/gpu/*.c")
    add_files("src/gpu/*.m")
    add_files("src/perturbation/perturb_cpu.c")

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
    add_packages("qoi", "libpng", "libsdl2", "cmt", "gmp", "mpfr")
    add_includedirs("src")

    -- Optimization
    set_optimize("fastest")

-- =============================================================================
-- Unit Tests
-- =============================================================================

target("mandelbrot_tests")
    set_kind("binary")
    set_languages("c17")
    add_rules("build_flags")
    set_default(false)

    add_files("tests/test_main.c")
    add_files("src/mandelbrot/mandelbrot.c")
    add_files("src/mandelbrot/mandelbrot_simd.c")
    add_files("src/perturbation/*.c")
    add_files("src/precision/*.c")
    add_files("src/cinematic/*.c")

    add_packages("gmp", "mpfr")
    add_includedirs("src")
    set_optimize("fastest")

-- =============================================================================
-- Interactive Viewer Target (native macOS, no SDL2)
-- =============================================================================

target("mandelbrot_interactive")
    set_kind("binary")
    set_languages("c17", "c++17")
    add_rules("metal2header", "build_flags")
    -- Build-order dependency only (binaries don't link each other): keeps
    -- the CLI building on plain `xmake` while `xmake run` targets just this.
    add_deps("mandelbrotplay_c")

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
    add_files("src/precision/*.c")
    add_files("src/cinematic/*.c")

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
    add_packages("cmt", "qoi", "gmp", "mpfr")
    add_includedirs("src")

    -- Optimization
    set_optimize("fastest")

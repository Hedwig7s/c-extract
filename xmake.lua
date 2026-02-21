add_rules("mode.debug", "mode.release")

add_requires("libzip", "yyjson", "glib")

target("c-extract")
do
    set_kind("binary")
    add_files("src/*.c")
    add_packages("libzip", "yyjson", "glib")
    --  add_includedirs("include")

    if is_mode("release") then
        set_policy("check.auto_ignore_flags", false)
        set_optimize("fastest")
        set_strip("all")
        set_symbols("hidden")
        add_ldflags("-flto")
        add_cxflags("-flto", "-Ofast")
    end
end

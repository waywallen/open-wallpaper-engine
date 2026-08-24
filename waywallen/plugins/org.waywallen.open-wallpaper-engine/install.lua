local bundle_layout = lito.env("OWE_WAYWALLEN_PLUGIN_BUNDLE_LAYOUT") == "ON"
local plugin = "share/waywallen/plugins/org.waywallen.open-wallpaper-engine"
local scene_bin = "../../../../bin/waywallen-wescene-renderer"
local web_bin = "../../../../lib/weweb/waywallen-weweb-renderer"
if bundle_layout then
    plugin = ""
    scene_bin = "bin/waywallen-wescene-renderer"
    web_bin = "lib/weweb/waywallen-weweb-renderer"
end

local function plugin_path(path)
    if plugin == "" then
        return path
    end
    return plugin .. "/" .. path
end

local web_block = lito.render_template({
    input = "weweb-renderer.toml.in",
    values = {
        OWE_WEWEB_BIN = web_bin,
    },
})

lito.install({
    files = {
        { source = "main.lua", destination = plugin_path("main.lua") },
        { source = "i18n/ru.po", destination = plugin_path("i18n/ru.po") },
        { source = "i18n/zh-CN.po", destination = plugin_path("i18n/zh-CN.po") },
        { source = "wallpaper_engine/api.lua", destination = plugin_path("wallpaper_engine/api.lua") },
        { source = "wallpaper_engine/auth.lua", destination = plugin_path("wallpaper_engine/auth.lua") },
        { source = "wallpaper_engine/discover.lua", destination = plugin_path("wallpaper_engine/discover.lua") },
        { source = "wallpaper_engine/map.lua", destination = plugin_path("wallpaper_engine/map.lua") },
        { source = "wallpaper_engine/profile.lua", destination = plugin_path("wallpaper_engine/profile.lua") },
        { source = "wallpaper_engine/project.lua", destination = plugin_path("wallpaper_engine/project.lua") },
        { source = "wallpaper_engine/session.lua", destination = plugin_path("wallpaper_engine/session.lua") },
        { source = "wallpaper_engine/source.lua", destination = plugin_path("wallpaper_engine/source.lua") },
        { source = "wallpaper_engine/subscription.lua", destination = plugin_path("wallpaper_engine/subscription.lua") },
        { source = "wallpaper_engine/wallpaper.lua", destination = plugin_path("wallpaper_engine/wallpaper.lua") },
        { source = "wallpaper_engine/workshop.lua", destination = plugin_path("wallpaper_engine/workshop.lua") },
    },
    templates = {
        {
            input = "plugin.toml.in",
            destination = plugin_path("plugin.toml"),
            values = {
                OWE_WAYWALLEN_PLUGIN_ID = "org.waywallen.open-wallpaper-engine",
                OWE_PLUGIN_VERSION = lito.package_version,
                OWE_WAYWALLEN_PLUGIN_UPDATE_URL = "https://github.com/waywallen/open-wallpaper-engine/raw/refs/heads/main/update.json",
                OWE_WESCENE_BIN = scene_bin,
                OWE_WEWEB_BLOCK = web_block,
            },
        },
    },
    inventories = {
        {
            destination = plugin_path("files.txt"),
            relative_to = plugin,
        },
    },
})

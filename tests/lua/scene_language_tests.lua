local source = assert(arg[1], "plugin directory required")

function tr(value) return value end

local modules = {}
function import(name)
    if not modules[name] then
        local file = source .. "/" .. (name:gsub("%.", "/")) .. ".lua"
        modules[name] = assert(loadfile(file))()
    end
    return modules[name]
end

local language = import("wallpaper_engine.language")

local env = {}
local settings = {}
local project = { general = { properties = {} } }
local project_exists = false
ctx = {
    env = function(name) return env[name] end,
    config = { get = function(name) return settings[name] end },
    fs = {
        exists = function(path) return project_exists and path:find("project.json", 1, true) ~= nil end,
        read = function(path) return "{}" end,
    },
    json = { parse = function() return project end },
}

local ENTRY = { wp_type = "scene", resource = "/workshop/431960/1/scene.pkg" }

local function keys(props)
    local out = {}
    for key in pairs(props) do out[#out + 1] = key end
    table.sort(out)
    return table.concat(out, ",")
end

-- The selector plus one variant per language, the shape workshop scenes ship.
local function scene()
    return {
        language = { text = "Language", type = "combo", value = "1", options = {
            { label = "English", value = "1" },
            { label = "Spanish", value = "4" },
            { label = "French", value = "5" },
        } },
        barcolor = { text = "Bar Color", type = "color", value = "1 1 1", condition = "language.value == 1" },
        barcolor3 = { text = "Color de la barra", type = "color", value = "0.2 0.2 0.2", condition = "language.value == 4" },
        barcolor4 = { text = "Couleur de la barre", type = "color", value = "0 0 0", condition = "language.value == 5" },
        hdr = { text = "HDR", type = "bool", value = true, condition = "" },
    }
end

-- Spanish host: only the Spanish variant survives and the selector shows it.
env = { LANG = "es_CL.UTF-8" }
local props = scene()
local filtered = language.filter(props, ctx)
assert(keys(filtered) == "barcolor3,hdr,language", keys(filtered))
assert(filtered.language.value == "4")
assert(filtered.barcolor3.value == "0.2 0.2 0.2")
assert(props.language.value == "1", "authored table must not be modified")
assert(props.barcolor ~= nil, "authored table must not be modified")

-- French host: the French variant is selected, not just the first one.
env = { LANG = "fr_FR.UTF-8" }
filtered = language.filter(scene(), ctx)
assert(keys(filtered) == "barcolor4,hdr,language", keys(filtered))
assert(filtered.language.value == "5")

-- A locale the scene does not offer leaves the authored defaults alone.
env = { LANG = "de_DE.UTF-8" }
props = scene()
assert(language.filter(props, ctx) == props)

-- An explicit wallpaper_engine_locale wins over the environment.
settings.wallpaper_engine_locale = "es-cl"
env = { LANG = "en_US.UTF-8" }
assert(language.filter(scene(), ctx).language.value == "4")
settings.wallpaper_engine_locale = nil

-- Labels are authored text: native spellings and suffixes both resolve.
env = { LANG = "es_CL.UTF-8" }
props = scene()
props.language.options[2].label = "Spanish (Latin America)"
assert(language.filter(props, ctx).language.value == "4")
props = scene()
props.language.options[2].label = "Español"
assert(language.filter(props, ctx).language.value == "4")

-- A plain label wins over a suffixed one offering the same language.
props = scene()
props.language.options[2].label = "Spanish (Latin America)"
props.language.options[3] = { label = "Spanish", value = "7" }
assert(language.filter(props, ctx).language.value == "7")

-- Whitespace around the operator is optional.
props = scene()
props.barcolor3.condition = "language.value==4"
assert(language.filter(props, ctx).barcolor3 ~= nil)

-- Numeric option values compare against the literal in the condition.
props = scene()
props.language.options[2].value = 4
filtered = language.filter(props, ctx)
assert(filtered.language.value == 4)
assert(filtered.barcolor3 ~= nil)

-- Gates on properties the plugin does not pin stay visible: the list comes from
-- authored defaults, so the panel cannot know the current value of a toggle.
props = scene()
props.coloring1 = { text = "Coloring", type = "combo", value = "1", options = {
    { label = "Solid", value = "1" },
    { label = "RGB", value = "3" },
} }
props.color1 = { text = "Color", type = "color", value = "1 1 1", condition = "coloring1.value == 3" }
filtered = language.filter(props, ctx)
assert(filtered.color1 ~= nil, "a condition on another property must stay visible")

-- An undecidable clause keeps the whole condition undecidable.
props = scene()
props.extra = { text = "Extra", type = "bool", value = true, condition = "language.value == 4 && other.value == 1" }
filtered = language.filter(props, ctx)
assert(filtered.extra ~= nil, "a compound condition with an unknown property must stay visible")
props = scene()
props.extra = { text = "Extra", type = "bool", value = true, condition = "(language.value == 4)" }
assert(language.filter(props, ctx).extra ~= nil, "parenthesised conditions are not decoded")

-- Relational and inequality gates are decoded for the pinned language.
props = scene()
props.onlynotspanish = { text = "Not Spanish", type = "bool", value = true, condition = "language.value != 4" }
assert(language.filter(props, ctx).onlynotspanish == nil)
props = scene()
props.fromfour = { text = "Four", type = "bool", value = true, condition = "language.value >= 4" }
assert(language.filter(props, ctx).fromfour ~= nil)

-- Scenes without a selector keep every property, even language-gated ones.
props = { color = { text = "Color", type = "color", value = "1 1 1", condition = "language.value == 1" } }
assert(language.filter(props, ctx) == props)

-- Integration: properties() filters the list the daemon asks for.
env = { LANG = "es_CL.UTF-8" }
project = { general = { properties = scene() } }
project_exists = true
local wallpaper = import("wallpaper_engine.wallpaper")
local wired = wallpaper.properties(ENTRY, ctx)
assert(wired.barcolor3 ~= nil and wired.barcolor == nil)
assert(wired.language.value == "4")
assert(wired.hdr ~= nil)
assert(wired["waywallen.enable_audio"] ~= nil)
assert(wired["waywallen.playback_speed"] ~= nil)
assert(wired["waywallen.mouse_parallax"] ~= nil)

-- apply() pins the same value, so the rendered wallpaper matches the labels.
env = { LANG = "es_CL.UTF-8" }
local applied = wallpaper.apply(ENTRY, ctx)
assert(applied.default_user_properties.language == "4")

env = { LANG = "fr_FR.UTF-8" }
applied = wallpaper.apply(ENTRY, ctx)
assert(applied.default_user_properties.language == "5")

env = { LANG = "de_DE.UTF-8" }
applied = wallpaper.apply(ENTRY, ctx)
assert(applied.default_user_properties.language == nil)
assert(wallpaper.properties(ENTRY, ctx).language.value == "1")

print("scene language tests passed")

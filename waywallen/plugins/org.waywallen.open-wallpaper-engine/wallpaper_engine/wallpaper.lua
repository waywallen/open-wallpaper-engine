local project_util = import("wallpaper_engine.project")

local M = {}

local _locale_cache = {}
local _tag_cache = nil

local LOCALE_DIR = "/steamapps/common/wallpaper_engine/locale/"
local FALLBACK_TAG = "en-us"
local PROPERTY_TITLE_PREFIX = "<style>\n  img { max-width: 100%; }\n  </style>\n"

-- Wallpaper Engine names its locale files after its own language list, not
-- after the POSIX locale, so the tag cannot always be derived from `$LANG`.
-- Only the cases `<lang>-<region>` and `<lang>-<lang>` miss are listed here.
local LOCALE_ALIASES = {
    ["zh-cn"] = "zh-chs", ["zh-sg"] = "zh-chs", ["zh-hans"] = "zh-chs",
    ["zh-tw"] = "zh-cht", ["zh-hk"] = "zh-cht", ["zh-mo"] = "zh-cht",
    ["zh-hant"] = "zh-cht",
    ar = "ar-sa", be = "be-by", cs = "cs-cz", da = "da-dk", el = "el-gr",
    en = "en-us", eu = "eu-es", fa = "fa-ir", he = "he-il", ja = "ja-jp",
    ko = "ko-kr", nb = "nb-no", no = "nb-no", pt = "pt-pt", sl = "sl-si",
    sv = "sv-se", uk = "uk-ua", vi = "vi-vn", zh = "zh-chs",
}

-- "ru_RU.UTF-8@euro" / "ru-RU" -> "ru", "ru". Region may come back empty.
local function split_posix_locale(value)
    local body = string.match(value, "^([^.@]+)") or value
    body = string.gsub(body, "-", "_")
    local lang, region = string.match(body, "^(%a+)_(%w+)$")
    if not lang then
        lang = string.match(body, "^(%a+)$")
    end
    if not lang then return nil, nil end
    return string.lower(lang), region and string.lower(region) or nil
end

-- The language the user reads, as a Wallpaper Engine locale tag. An explicit
-- `wallpaper_engine_locale` setting wins; otherwise the daemon's environment
-- decides, in POSIX precedence. Every candidate is probed against the actual
-- locale directory, so an unknown tag falls through instead of blanking the
-- property titles.
local function locale_tags(ctx, dir)
    if _tag_cache then return _tag_cache end

    local candidates = {}
    local function add(tag)
        if tag and tag ~= "" then table.insert(candidates, tag) end
    end

    local configured = ctx.config.get("wallpaper_engine_locale")
    if configured and configured ~= "" then
        add(string.lower(configured))
    end

    local env = nil
    for _, name in ipairs({ "LC_ALL", "LC_MESSAGES", "LANG" }) do
        local v = ctx.env(name)
        if v and v ~= "" and v ~= "C" and v ~= "POSIX" then
            env = v
            break
        end
    end
    if env then
        local lang, region = split_posix_locale(env)
        if lang then
            if region then
                add(lang .. "-" .. region)
                add(LOCALE_ALIASES[lang .. "-" .. region])
            end
            add(lang .. "-" .. lang)
            add(LOCALE_ALIASES[lang])
        end
    end

    local tag = nil
    for _, candidate in ipairs(candidates) do
        if ctx.fs.exists(dir .. "ui_" .. candidate .. ".json") then
            tag = candidate
            break
        end
    end

    _tag_cache = { tag = tag, fallback = FALLBACK_TAG }
    return _tag_cache
end

local function read_locale_file(ctx, path)
    if not ctx.fs.exists(path) then return nil end
    local content = ctx.fs.read(path)
    if not content then return nil end
    local parsed = ctx.json.parse(content)
    if type(parsed) ~= "table" then return nil end
    return parsed
end

-- Returns the translated table and the English one. Wallpaper Engine's
-- translated files trail en-us by a few dozen keys; without the English
-- table behind them those properties would show their raw locale key.
local function load_locale(ctx, library_root)
    if library_root == nil or library_root == "" then return nil, nil end
    local cached = _locale_cache[library_root]
    if cached ~= nil then
        if cached == false then return nil, nil end
        return cached.localized, cached.fallback
    end

    local dir = library_root .. LOCALE_DIR
    local tags = locale_tags(ctx, dir)
    local fallback = read_locale_file(ctx, dir .. "ui_" .. tags.fallback .. ".json")
    local localized = nil
    if tags.tag and tags.tag ~= tags.fallback then
        localized = read_locale_file(ctx, dir .. "ui_" .. tags.tag .. ".json")
    end

    if not localized and not fallback then
        _locale_cache[library_root] = false
        return nil, nil
    end
    _locale_cache[library_root] = { localized = localized, fallback = fallback }
    return localized, fallback
end

local PROPERTY_KEY_MAP = {
    schemecolor = "waywallen.scheme_color"
}

local ENABLE_AUDIO_PROPERTY = {
    text = tr("Enable audio"),
    type = "bool",
    value = true,
}

local PLAYBACK_SPEED_PROPERTY = {
    text = tr("Playback speed"),
    type = "slider",
    min = 10,
    max = 400,
    step = 10,
    suffix = "%",
    value = 100,
}

local function load_project_properties(entry, ctx)
    local dir = project_util.project_dir_of(entry)
    if not dir then return nil end
    local proj = dir .. "/project.json"
    if not ctx.fs.exists(proj) then return nil end
    local content = ctx.fs.read(proj)
    if not content then return nil end
    local parsed = ctx.json.parse(content)
    if not parsed or type(parsed) ~= "table" then return nil end
    local props = parsed.general and parsed.general.properties or {}
    if type(props) ~= "table" then return {} end
    return props
end

local function map_property_keys(props)
    for from, to in pairs(PROPERTY_KEY_MAP) do
        local v = props[from]
        if v ~= nil then
            if props[to] == nil then props[to] = v end
            props[from] = nil
        end
    end
end

local function color_wire_value(value)
    if type(value) == "string" then return value end
    if type(value) ~= "table" then return nil end
    local components = {}
    for index = 1, #value do
        if type(value[index]) ~= "number" then return nil end
        components[index] = tostring(value[index])
    end
    if #components < 3 or #components > 4 then return nil end
    return table.concat(components, " ")
end

local function prefix_property_titles(props)
    for _, v in pairs(props) do
        if type(v) == "table" and type(v.text) == "string" then
            v.text = PROPERTY_TITLE_PREFIX .. v.text
        end
    end
end

local function add_predefined_properties(entry, props)
    if entry.wp_type == "web" then return end
    if props["waywallen.enable_audio"] == nil then
        props["waywallen.enable_audio"] = ENABLE_AUDIO_PROPERTY
    end
    if (entry.wp_type == "scene" or entry.wp_type == "video") and
        props["waywallen.playback_speed"] == nil then
        props["waywallen.playback_speed"] = PLAYBACK_SPEED_PROPERTY
    end
end

function M.properties(entry, ctx)
    local props = load_project_properties(entry, ctx)
    if not props then return nil end
    map_property_keys(props)

    local locale, locale_fallback = load_locale(ctx, entry.library_root)
    if locale or locale_fallback then
        for _, v in pairs(props) do
            if type(v) == "table" and type(v.text) == "string" then
                local mapped = locale and locale[v.text] or nil
                if type(mapped) ~= "string" or mapped == "" then
                    mapped = locale_fallback and locale_fallback[v.text] or nil
                end
                if type(mapped) == "string" and mapped ~= "" then
                    v.text = mapped
                end
            end
        end
    end

    prefix_property_titles(props)
    add_predefined_properties(entry, props)

    return ctx.json.encode(props)
end

local function we_assets(ctx)
    local configured = ctx.config.get("wallpaper_engine_assets")
    if configured and configured ~= "" and ctx.fs.exists(configured) then
        return configured
    end
    local home = ctx.env("HOME") or ""
    local roots = {
        home .. "/.local/share/Steam",
        home .. "/.steam/steam",
        home .. "/.steam/root",
        home .. "/.var/app/com.valvesoftware.Steam/data/Steam",
    }
    for _, root in ipairs(roots) do
        local p = root .. project_util.ASSETS_REL
        if ctx.fs.exists(p) then
            return p
        end
    end
    return nil
end

function M.apply(entry, ctx)
    local extras = { path = entry.resource }
    local default_user_properties = {}
    if entry.wp_type == "video" then
        local props = load_project_properties(entry, ctx)
        if props then
            map_property_keys(props)
            local scheme = props["waywallen.scheme_color"]
            if type(scheme) == "table" then
                local value = color_wire_value(scheme.value)
                if value then
                    default_user_properties["waywallen.scheme_color"] = value
                end
            end
        end
    end
    if entry.wp_type == "scene" then
        local assets
        if entry.library_root and entry.library_root ~= "" then
            local candidate = entry.library_root .. project_util.ASSETS_REL
            if ctx.fs.exists(candidate) then
                assets = candidate
            end
        end
        assets = assets or we_assets(ctx)
        if assets then
            extras.assets = assets
        end
    end
    if entry.external_id and entry.external_id ~= "" then
        extras.workshop_id = entry.external_id
    end
    return {
        extras = extras,
        default_user_properties = default_user_properties,
    }
end

return M

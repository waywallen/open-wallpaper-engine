local M = {}

M.WE_APPID = "431960"
M.WORKSHOP = "/steamapps/workshop/content/" .. M.WE_APPID
M.ASSETS_REL = "/steamapps/common/wallpaper_engine/assets"
M.PROJECTS_REL = "/steamapps/common/wallpaper_engine/projects"
M.LOCAL_DIRS = { "defaultprojects", "myprojects" }

-- Users are asked to register the directory that *contains* steamapps, but they
-- routinely pick steamapps itself, or the workshop content directory they can
-- see the wallpapers in. Fold those back to the Steam root, so the paths built
-- from WORKSHOP/PROJECTS_REL still resolve instead of silently missing.
function M.normalize_root(path)
    if type(path) ~= "string" then return path end
    local root = path:gsub("/+$", "")
    root = root:gsub("/steamapps/workshop/content/" .. M.WE_APPID .. "$", "")
    root = root:gsub("/steamapps/workshop/content$", "")
    root = root:gsub("/steamapps/workshop$", "")
    root = root:gsub("/steamapps$", "")
    return root
end

local VIDEO_EXTS = { mp4 = true, webm = true, mkv = true, avi = true, mov = true }

local SCENE_BASE_BY_FILE = {
    ["assets.json"] = "assets",
    ["audiophile.json"] = "audiophile",
    ["fantasticcar.json"] = "fantasticcar",
    ["gifscene.json"] = "gifscene",
    ["ricepod.json"] = "ricepod",
    ["scene.json"] = "scene",
    ["techno.json"] = "techno",
}

function M.pick_preview(ctx, dir, project)
    if project and project.preview then
        local p = dir .. "/" .. project.preview
        if ctx.fs.exists(p) then return p end
    end
    for _, p in ipairs({ dir .. "/preview.jpg", dir .. "/preview.png", dir .. "/preview.gif" }) do
        if ctx.fs.exists(p) then return p end
    end
    return nil
end

local function classify(ctx, dir, project)
    local project_type = project and type(project.type) == "string" and string.lower(project.type)
    if project_type == "web" then
        if ctx.fs.exists(dir .. "/project.json") then
            return "web", dir
        end
    elseif project_type == "video" then
        local file = project and project.file
        if file and ctx.fs.exists(dir .. "/" .. file) then
            return "video", dir .. "/" .. file
        end
        for _, path in ipairs(ctx.fs.glob(dir .. "/*.*")) do
            local ext = ctx.fs.extension(path)
            if ext and VIDEO_EXTS[string.lower(ext)] then
                return "video", path
            end
        end
    else
        local file = project and project.file
        local base = SCENE_BASE_BY_FILE[file] or "scene"
        if ctx.fs.exists(dir .. "/" .. base .. ".pkg") then
            return "scene", dir .. "/" .. base .. ".pkg"
        elseif ctx.fs.exists(dir .. "/" .. base .. ".json") then
            return "scene", dir .. "/" .. base .. ".json"
        end
    end
    return nil, nil
end

-- Strip the item-directory prefix so resource/preview come back relative to it,
-- the form the daemon's resolve upsert expects.
function M.relpath(dir, abs)
    if not abs then
        return nil
    end
    local d = dir:gsub("/+$", "")
    if abs == d then
        return ""
    end
    local prefix = d .. "/"
    if abs:sub(1, #prefix) == prefix then
        return abs:sub(#prefix + 1)
    end
    return abs
end

function M.resolver(ctx)
    local roots = ctx.libraries()
    table.sort(roots)
    local function read(dir)
        local content = ctx.fs.read(dir .. "/project.json")
        local parsed = content and ctx.json.parse(content)
        if type(parsed) == "table" then return parsed end
    end

    local function base_at(root, dependency)
        local dir = root .. M.WORKSHOP .. "/" .. dependency
        local manifest = read(dir)
        if not manifest or manifest.preset ~= nil then return nil end
        local wp_type, resource = classify(ctx, dir, manifest)
        if wp_type then
            return {
                wp_type = wp_type, resource = resource, manifest = manifest, library_root = root,
            }
        end
    end

    local function resolve_preset(dir, manifest, root)
        local dependency = manifest and manifest.dependency
        if type(dependency) ~= "string" or not dependency:match("^[1-9]%d*$")
            or type(manifest.preset) ~= "table" then
            return nil, "invalid project.json dependency or preset"
        end
        if ctx.fs.basename(dir) == dependency then
            return nil, "preset depends on itself"
        end
        local base = base_at(root, dependency)
        if not base then
            for _, library in ipairs(roots) do
                local candidate = M.normalize_root(library)
                if candidate ~= root then
                    base = base_at(candidate, dependency)
                    if base then break end
                end
            end
        end
        if not base then
            return nil, "Workshop dependency " .. dependency ..
                " is not installed as a playable original in a registered Steam library"
        end
        base.preset = manifest.preset
        base.preset_dir = dir
        return base
    end

    return {
        scan = function(dir, root, is_workshop)
            local manifest = read(dir)
            if is_workshop and manifest and manifest.preset ~= nil then
                local base, err = resolve_preset(dir, manifest, root)
                if not base then return nil, err end
                return {
                    wp_type = base.wp_type, resource = dir .. "/project.json", manifest = manifest,
                }
            end
            local wp_type, resource = classify(ctx, dir, manifest)
            if wp_type then
                return { wp_type = wp_type, resource = resource, manifest = manifest }
            end
        end,
        entry = function(entry)
            local root = M.normalize_root(entry.library_root or "")
            local dir
            if ctx.fs.basename(entry.resource) == "project.json" then
                dir = ctx.fs.parent(entry.resource)
                local base, err = resolve_preset(dir, read(dir), root)
                if base and base.wp_type ~= entry.wp_type then
                    return nil, "Workshop dependency type changed; rescan the library"
                end
                return base, err
            end
            dir = entry.wp_type == "web" and entry.resource or ctx.fs.parent(entry.resource)
            return { resource = entry.resource, manifest = dir and read(dir), library_root = root }
        end,
    }
end

return M

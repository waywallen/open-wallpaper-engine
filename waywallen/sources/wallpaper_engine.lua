local M = {}

function M.info()
    return {
        name = "wallpaper_engine",
        types = {"scene", "video"},
        version = "0.2.0",
    }
end

function M.scan(ctx)
    local entries = {}

    local workshop_dir = ctx.config("workshop_dir")
        or ctx.env("WAYWALLEN_WORKSHOP_DIR")

    if not workshop_dir then
        local home = ctx.env("HOME") or ""
        -- Try common Steam library paths
        local candidates = {
            home .. "/.steam/steam/steamapps/workshop/content/431960",
            home .. "/.local/share/Steam/steamapps/workshop/content/431960",
            home .. "/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/workshop/content/431960",
        }
        for _, path in ipairs(candidates) do
            if ctx.file_exists(path) then
                workshop_dir = path
                break
            end
        end
    end

    if not workshop_dir or not ctx.file_exists(workshop_dir) then
        ctx.log("wallpaper_engine: workshop dir not found")
        return entries
    end

    -- Derive WE installation assets dir from workshop path.
    -- workshop_dir = .../steamapps/workshop/content/431960
    -- we_assets    = .../steamapps/common/wallpaper_engine/assets
    local steamapps = workshop_dir:match("(.*/steamapps)/workshop/content/%d+$")
    local we_assets = steamapps and (steamapps .. "/common/wallpaper_engine/assets") or ""
    if we_assets == "" or not ctx.file_exists(we_assets) then
        ctx.log("wallpaper_engine: WE assets dir not found, shaders may be missing")
    end

    local video_exts = {mp4 = true, webm = true, mkv = true, avi = true, mov = true}

    local dirs = ctx.list_dirs(workshop_dir)
    for _, dir in ipairs(dirs) do
        local workshop_id = ctx.basename(dir) or dir
        local name = "Workshop " .. workshop_id

        -- Parse project.json first to determine wallpaper type.
        local project = nil
        local project_path = dir .. "/project.json"
        if ctx.file_exists(project_path) then
            local content = ctx.read_file(project_path)
            if content then
                project = ctx.json_parse(content)
            end
        end

        local project_type = project and project.type and string.lower(project.type) or nil
        if project and project.title then
            name = project.title
        end

        local wp_type = nil
        local resource = nil

        if project_type == "video" then
            -- Resolve the video file referenced by project.json.
            local file = project and project.file
            if file and ctx.file_exists(dir .. "/" .. file) then
                wp_type = "video"
                resource = dir .. "/" .. file
            else
                -- Fallback: first video file in the directory.
                local pkg_dir_files = ctx.glob(dir .. "/*.*")
                for _, path in ipairs(pkg_dir_files) do
                    local ext = ctx.extension(path)
                    if ext and video_exts[string.lower(ext)] then
                        wp_type = "video"
                        resource = path
                        break
                    end
                end
            end
        else
            -- Scene wallpaper (default). Prefer scene.pkg, fall back to scene.json.
            local pkg_path = dir .. "/scene.pkg"
            local json_path = dir .. "/scene.json"
            if ctx.file_exists(pkg_path) then
                wp_type = "scene"
                resource = pkg_path
            elseif ctx.file_exists(json_path) then
                wp_type = "scene"
                resource = json_path
            end
        end

        if wp_type and resource then
            -- Look for preview image
            local preview = nil
            if project and project.preview then
                local p = dir .. "/" .. project.preview
                if ctx.file_exists(p) then
                    preview = p
                end
            end
            if not preview then
                local preview_candidates = {
                    dir .. "/preview.jpg",
                    dir .. "/preview.png",
                    dir .. "/preview.gif",
                }
                for _, p in ipairs(preview_candidates) do
                    if ctx.file_exists(p) then
                        preview = p
                        break
                    end
                end
            end

            local metadata = {
                workshop_id = workshop_id,
            }
            if wp_type == "scene" then
                metadata.scene = resource
                metadata.assets = we_assets
            else
                -- waywallen-mpv reads `video` / `path` from metadata.
                metadata.video = resource
                metadata.path = resource
            end

            table.insert(entries, {
                id = workshop_id,
                name = name,
                wp_type = wp_type,
                resource = resource,
                preview = preview,
                metadata = metadata,
            })
        end
    end

    ctx.log("wallpaper_engine: found " .. #entries .. " wallpapers in " .. workshop_dir)
    return entries
end

return M

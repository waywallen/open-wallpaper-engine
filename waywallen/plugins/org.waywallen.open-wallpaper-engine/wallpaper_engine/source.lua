local project_util = import("wallpaper_engine.project")
local workshop = import("wallpaper_engine.workshop")

local M = {}

function M.auto_detect(ctx)
    local home = ctx.env("HOME") or ""
    if home == "" then return {} end
    local candidates = {
        home .. "/.steam/steam",
        home .. "/.local/share/Steam",
        home .. "/.var/app/com.valvesoftware.Steam/data/Steam",
    }
    local found, seen = {}, {}
    for _, root in ipairs(candidates) do
        if not seen[root] and (ctx.fs.exists(root .. project_util.WORKSHOP)
            or ctx.fs.exists(root .. project_util.PROJECTS_REL)) then
            seen[root] = true
            table.insert(found, root)
        end
    end
    return found
end

local function scan_container(ctx, steam_root, container, name_prefix, is_workshop, entries)
    if not ctx.fs.exists(container) then return end
    for _, dir in ipairs(ctx.fs.list_dirs(container)) do
        local id = ctx.fs.basename(dir) or dir
        local project = nil
        local project_path = dir .. "/project.json"
        if ctx.fs.exists(project_path) then
            local content = ctx.fs.read(project_path)
            if content then project = ctx.json.parse(content) end
        end
        local project_type = project and project.type and string.lower(project.type) or nil

        local wp_type, resource = project_util.classify(ctx, dir, project, project_type)
        if wp_type and resource then
            local external_id = is_workshop and workshop.subscription_id(id) or nil
            table.insert(entries, {
                name = (project and project.title) or (name_prefix .. id),
                wp_type = wp_type,
                resource = resource,
                preview = project_util.pick_preview(ctx, dir, project),
                library_root = steam_root,
                description = project and project.description or nil,
                tags = (project and project.tags) or {},
                content_rating = project and project.contentrating or nil,
                external_id = external_id,
                update_available = is_workshop and workshop.update_available(id, steam_root) or false,
                web_url = external_id and
                    ("https://steamcommunity.com/sharedfiles/filedetails/?id=" .. external_id) or nil,
                -- Steam measured the item when it unpacked it; without that the
                -- daemon stats `resource` alone and calls a web wallpaper's
                -- directory inode the size of the wallpaper.
                size = is_workshop and workshop.size(id) or nil,
                metadata = {},
            })
        end
    end
end

function M.scan(ctx)
    local entries = {}

    local roots = {}
    for _, library in ipairs(ctx.libraries()) do
        table.insert(roots, project_util.normalize_root(library))
    end
    -- Steam's own record decides which Workshop directories are subscriptions;
    -- read it once per scan, before any entry asks about its id.
    workshop.refresh(ctx, roots)

    for _, steam_root in ipairs(roots) do
        local content = steam_root .. project_util.WORKSHOP
        if not ctx.fs.exists(content) then
            ctx.log("wallpaper_engine: no WE workshop under " .. steam_root)
        else
            scan_container(ctx, steam_root, content, "Workshop ", true, entries)
        end
        for _, name in ipairs(project_util.LOCAL_DIRS) do
            scan_container(
                ctx,
                steam_root,
                steam_root .. project_util.PROJECTS_REL .. "/" .. name,
                "",
                false,
                entries
            )
        end
    end

    ctx.log("wallpaper_engine: " .. #entries .. " wallpapers")
    return entries
end

return M

local api = import("wallpaper_engine.api")
local map = import("wallpaper_engine.map")
local profile = import("wallpaper_engine.profile")

local M = {}
local cached_details = {}

function M.search(ctx, params)
    local result = api.search(ctx, params)
    local creators = {}
    for _, item in ipairs(result.items) do
        creators[#creators + 1] = tostring(item.creator or "")
    end
    local authors = profile.names(ctx, creators)
    local items = {}
    for _, item in ipairs(result.items) do
        cached_details[tostring(item.publishedfileid or "")] = item
        table.insert(items, map.search_item(item, authors))
    end
    local page = params.page or 1
    return {
        items = items,
        has_more = page * result.numperpage < result.total,
    }
end

function M.details(ctx, id)
    local key = tostring(id)
    local item = cached_details[key]
    if not item then
        item = api.details(ctx, key)
        cached_details[key] = item
    end
    return map.details(item)
end

return M

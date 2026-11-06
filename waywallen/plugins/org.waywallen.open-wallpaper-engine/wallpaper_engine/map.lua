local api = import("wallpaper_engine.api")

local M = {}

local function tag_names(item)
    local out = {}
    for _, t in ipairs(item.tags or {}) do
        local name = type(t) == "table" and t.tag or t
        if name and name ~= "" then
            table.insert(out, name)
        end
    end
    return out
end

local function wp_type(item)
    for _, name in ipairs(tag_names(item)) do
        if api.TYPE_WP[name] then
            return api.TYPE_WP[name]
        end
    end
    return "scene"
end

local function preview_url(item)
    if item.preview_url and item.preview_url ~= "" then
        return item.preview_url
    end
    local previews = item.previews or {}
    local first = previews[1] or {}
    return first.url or ""
end

-- `authors` maps a creator's steamid64 to a persona name. A creator missing
-- from it keeps the empty author the Workshop grid has always shown.
function M.search_item(item, authors)
    local creator = tostring(item.creator or "")
    return {
        id = tostring(item.publishedfileid or ""),
        title = item.title or "",
        preview_url = preview_url(item),
        author = authors and authors[creator] or "",
        wp_type = wp_type(item),
    }
end

local function fields(item)
    local out = {}
    local function field(key, label, value)
        if value ~= nil then
            table.insert(out, { key = key, label = label, value = tostring(value) })
        end
    end
    field("subscribers", tr("Subscribers"), item.subscriptions or item.lifetime_subscriptions)
    field("favorites", tr("Favorites"), item.favorited)
    field("votes_up", tr("Upvotes"), (item.vote_data or {}).votes_up)
    return out
end

function M.details(item)
    return {
        description = item.description or item.short_description or item.file_description or "",
        size = tostring(item.file_size or ""),
        tags = tag_names(item),
        fields = fields(item),
        web_url = "https://steamcommunity.com/sharedfiles/filedetails/?id=" ..
            tostring(item.publishedfileid or ""),
    }
end

return M

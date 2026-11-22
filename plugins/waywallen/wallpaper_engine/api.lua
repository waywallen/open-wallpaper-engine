local M = {}
local session = import("wallpaper_engine.session")

M.APPID = "431960"
local QUERYFILES = "https://api.steampowered.com/IPublishedFileService/QueryFiles/v1/"
local FILEDETAILS = "https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/"
local NUMPERPAGE = 30

-- sort key -> EPublishedFileQueryType (+ trend window in days).
-- 0 RankedByVote, 1 RankedByPublicationDate, 3 RankedByTrend, 9 RankedByTotalUniqueSubscriptions.
local SORTS = {
    trend_day = { query_type = 3, days = 1 },
    trend_week = { query_type = 3, days = 7 },
    trend_month = { query_type = 3, days = 30 },
    trend_3months = { query_type = 3, days = 90 },
    trend_6months = { query_type = 3, days = 180 },
    trend_year = { query_type = 3, days = 365 },
    recent = { query_type = 1 },
    most_subscribed = { query_type = 9 },
    top_rated = { query_type = 0 },
}

local ARTIST_SORTS = { recent = "creationorder" }
M.artist_sorts = {
    { key = "recent", label = tr("Most recent") },
}

-- Workshop type tags -> waywallen wp_type. Application is omitted (unrenderable, unless wine..?).
M.TYPE_WP = {
    Scene = "scene",
    Video = "video",
    Web = "web",
}

local RATING_TAGS = { Everyone = true, Questionable = true, Mature = true }

M.filters = {
    {
        id = "type",
        title = tr("Type"),
        type = "multi_select",
        options = {
            { value = "Scene", label = tr("Scene") },
            { value = "Video", label = tr("Video") },
            { value = "Web", label = tr("Web") },
        },
    },
    {
        id = "content_rating",
        title = tr("Content rating"),
        type = "multi_select",
        options = {
            { value = "Everyone", label = tr("Everyone") },
            { value = "Questionable", label = tr("Questionable") },
            { value = "Mature", label = tr("Mature") },
        },
        description = tr("Defaults to Everyone when nothing is selected. Mature includes NSFW wallpapers."),
    },
    {
        id = "miscellaneous",
        title = tr("Miscellaneous"),
        type = "multi_select",
        supports_exclusion = true,
        options = {
            { value = "Approved", label = tr("Approved") },
            { value = "Audio responsive", label = tr("Audio responsive") },
            { value = "3D", label = tr("3D") },
            { value = "Customizable", label = tr("Customizable") },
            { value = "Puppet Warp", label = tr("Puppet Warp") },
            { value = "HDR", label = tr("HDR") },
            { value = "Media Integration", label = tr("Media Integration") },
            { value = "User Shortcut", label = tr("User Shortcut") },
            { value = "Video Texture", label = tr("Video Texture") },
        },
    },
    {
        id = "genre",
        title = tr("Genre"),
        type = "multi_select",
        supports_exclusion = true,
        options = {
            { value = "Abstract", label = tr("Abstract") },
            { value = "Animal", label = tr("Animal") },
            { value = "Anime", label = tr("Anime") },
            { value = "Cartoon", label = tr("Cartoon") },
            { value = "CGI", label = tr("CGI") },
            { value = "Cyberpunk", label = tr("Cyberpunk") },
            { value = "Fantasy", label = tr("Fantasy") },
            { value = "Game", label = tr("Game") },
            { value = "Girls", label = tr("Girls") },
            { value = "Guys", label = tr("Guys") },
            { value = "Landscape", label = tr("Landscape") },
            { value = "Medieval", label = tr("Medieval") },
            { value = "Memes", label = tr("Memes") },
            { value = "MMD", label = tr("MMD") },
            { value = "Music", label = tr("Music") },
            { value = "Nature", label = tr("Nature") },
            { value = "Pixel art", label = tr("Pixel art") },
            { value = "Relaxing", label = tr("Relaxing") },
            { value = "Retro", label = tr("Retro") },
            { value = "Sci-Fi", label = tr("Sci-Fi") },
            { value = "Sports", label = tr("Sports") },
            { value = "Technology", label = tr("Technology") },
            { value = "Television", label = tr("Television") },
            { value = "Vehicle", label = tr("Vehicle") },
            { value = "Unspecified", label = tr("Unspecified") },
        },
    },
    {
        id = "resolution",
        title = tr("Resolution"),
        type = "select",
        options = {
            { value = "Standard Definition", label = tr("Standard Definition") },
            { value = "1280 x 720", label = tr("1280 x 720") },
            { value = "1366 x 768", label = tr("1366 x 768") },
            { value = "1920 x 1080", label = tr("1920 x 1080") },
            { value = "2560 x 1440", label = tr("2560 x 1440") },
            { value = "3840 x 2160", label = tr("3840 x 2160") },
            { value = "Ultrawide Standard Definition", label = tr("Ultrawide Standard Definition") },
            { value = "Ultrawide 2560 x 1080", label = tr("Ultrawide 2560 x 1080") },
            { value = "Ultrawide 3440 x 1440", label = tr("Ultrawide 3440 x 1440") },
            { value = "Dual Standard Definition", label = tr("Dual Standard Definition") },
            { value = "Dual 3840 x 1080", label = tr("Dual 3840 x 1080") },
            { value = "Dual 5120 x 1440", label = tr("Dual 5120 x 1440") },
            { value = "Dual 7680 x 2160", label = tr("Dual 7680 x 2160") },
            { value = "Triple Standard Definition", label = tr("Triple Standard Definition") },
            { value = "Triple 4096 x 768", label = tr("Triple 4096 x 768") },
            { value = "Triple 5760 x 1080", label = tr("Triple 5760 x 1080") },
            { value = "Triple 7680 x 1440", label = tr("Triple 7680 x 1440") },
            { value = "Triple 11520 x 2160", label = tr("Triple 11520 x 2160") },
            { value = "Portrait Standard Definition", label = tr("Portrait Standard Definition") },
            { value = "Portrait 720 x 1280", label = tr("Portrait 720 x 1280") },
            { value = "Portrait 1080 x 1920", label = tr("Portrait 1080 x 1920") },
            { value = "Portrait 1440 x 2560", label = tr("Portrait 1440 x 2560") },
            { value = "Portrait 2160 x 3840", label = tr("Portrait 2160 x 3840") },
            { value = "Other resolution", label = tr("Other resolution") },
            { value = "Dynamic resolution", label = tr("Dynamic resolution") },
        },
    },
}

function M.artist_id(value)
    local id = tostring(value or "")
    if #id < 17 or #id > 20 or not id:match("^%d+$") then return "" end
    return id
end

local function file_details(ctx, ids)
    local form = { itemcount = tostring(#ids) }
    for index, id in ipairs(ids) do form["publishedfileids[" .. index - 1 .. "]"] = id end
    local response = ctx.http:post(FILEDETAILS):form(form):timeout(20):send()
    if not response:ok() then error("Steam Workshop details http " .. tostring(response:status())) end
    local body = response:json() or {}
    return (body.response or {}).publishedfiledetails or {}
end

local function artist_files(ctx, artist, query)
    local http = session.ensure_http(ctx)
    local response = http:get("https://steamcommunity.com/profiles/" .. artist .. "/myworkshopfiles/")
        :query(query):timeout(20):send()
    if not response:ok() then error("Steam Workshop artist http " .. tostring(response:status())) end
    local ids, seen, total, empty = {}, {}, nil, false
    local elements = ctx.html.query(response:text() or "",
        ".workshopItemPreviewHolder[id], .workshopBrowsePagingInfo, #no_items")
    for _, element in ipairs(elements) do
        local id = (element.attrs.id or ""):match("^sharedfile_(%d+)$")
        if id then
            if seen[id] or #ids >= NUMPERPAGE then error("Invalid Steam Workshop artist listing") end
            seen[id] = true
            ids[#ids + 1] = id
        elseif element.attrs.id == "no_items" then
            empty = true
        else
            local count = element.text:match("of%s+([%d,]+)%s+entr")
            if count then total = tonumber((count:gsub(",", ""))) end
        end
    end
    if not total then
        if empty and #ids == 0 then return { items = {}, total = 0, numperpage = NUMPERPAGE } end
        error("Steam Workshop artist listing is unavailable")
    end
    if (#ids == 0 and total > 0) or total < #ids then error("Invalid Steam Workshop artist pagination") end
    local by_id, items = {}, {}
    if #ids > 0 then
        for _, item in ipairs(file_details(ctx, ids)) do
            local id = tostring(item.publishedfileid or "")
            if not seen[id] then error("Steam returned an unexpected Workshop item") end
            if not item.result or tonumber(item.result) == 1 then
                if M.artist_id(item.creator) ~= artist
                    or (item.consumer_app_id and tostring(item.consumer_app_id) ~= M.APPID) then
                    error("Steam returned an item outside the requested artist scope")
                end
                by_id[id] = item
            end
        end
    end
    for _, id in ipairs(ids) do if by_id[id] then items[#items + 1] = by_id[id] end end
    return { items = items, total = total, numperpage = NUMPERPAGE }
end

function M.search(ctx, params)
    local sort = SORTS[params.sort] or SORTS.trend_week
    local artist_sort
    if params.artist_id then
        if M.artist_id(params.artist_id) == "" then
            error("Invalid Steam artist id")
        end
        artist_sort = ARTIST_SORTS[params.sort and params.sort ~= "" and params.sort or "recent"]
        if not artist_sort then error("Unsupported Workshop artist sort") end
        if (params.query or "") ~= "" then error("Workshop artist browsing does not support text search") end
    end

    -- Only wallpapers: never return applications, presets or asset packs.
    -- Content rating defaults to Everyone; explicit selections replace that default.
    local excluded = {
        Application = true, Preset = true, Asset = true, ["Asset Pack"] = true,
    }
    for _, tag in ipairs(params.excluded_tags or {}) do
        excluded[tag] = true
    end
    local required = {}
    local selected_types = {}
    local selected_ratings = {}
    local selected_rating_count = 0
    for _, tag in ipairs(params.tags or {}) do
        if M.TYPE_WP[tag] then
            selected_types[tag] = true
        elseif RATING_TAGS[tag] then
            if not selected_ratings[tag] then
                selected_ratings[tag] = true
                selected_rating_count = selected_rating_count + 1
            end
        else
            table.insert(required, tag)
        end
    end
    if next(selected_types) then
        for tag in pairs(M.TYPE_WP) do
            if not selected_types[tag] then excluded[tag] = true end
        end
    end
    if selected_rating_count == 0 then
        selected_ratings.Everyone = true
        selected_rating_count = 1
    end
    for tag in pairs(RATING_TAGS) do
        if not selected_ratings[tag] then excluded[tag] = true end
    end
    if selected_rating_count == 1 then
        for tag in pairs(selected_ratings) do table.insert(required, tag) end
    end

    local query = {
        appid = M.APPID,
        numperpage = tostring(NUMPERPAGE),
    }
    if params.artist_id then
        query.p = tostring(params.page or 1)
        query.browsefilter = "myfiles"
        query.section = "items"
        query.sortmethod = artist_sort
        query.l = "english"
    else
        query.page = tostring(params.page or 1)
        query.query_type = tostring(sort.query_type)
        query.search_text = params.query or ""
        query.return_tags = "true"
        query.return_previews = "true"
        query.return_vote_data = "true"
        query.return_short_description = "true"
        query.format = "json"
        if sort.days then query.days = tostring(sort.days) end
    end
    for i, tag in ipairs(required) do
        query["requiredtags[" .. (i - 1) .. "]"] = tag
    end
    local ei = 0
    for tag in pairs(excluded) do
        query["excludedtags[" .. ei .. "]"] = tag
        ei = ei + 1
    end

    if params.artist_id then return artist_files(ctx, params.artist_id, query) end

    local rsp = session.authorized(ctx, function(access_token, http)
        query.access_token = access_token
        return http:get(QUERYFILES):query(query):timeout(20):send()
    end)
    if not rsp:ok() then
        error("steam workshop http " .. tostring(rsp:status()))
    end
    local body = rsp:json() or {}
    local response = body.response or {}
    if response.result and tonumber(response.result) ~= 1 then
        error("Steam Workshop query failed with result " .. tostring(response.result))
    end
    return {
        items = response.publishedfiledetails or {},
        total = response.total or 0,
        numperpage = NUMPERPAGE,
    }
end

function M.details(ctx, id)
    local list = file_details(ctx, { tostring(id) })
    return list[1] or {}
end

return M

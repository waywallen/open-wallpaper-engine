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

function M.search(ctx, params)
    local sort = SORTS[params.sort] or SORTS.trend_week

    -- Only wallpapers: never return applications, presets or asset packs.
    -- Content rating defaults to Everyone; explicit selections replace that default.
    local excluded = {
        Application = true, Preset = true, Asset = true, ["Asset Pack"] = true,
    }
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
        query_type = tostring(sort.query_type),
        page = tostring(params.page or 1),
        numperpage = tostring(NUMPERPAGE),
        search_text = params.query or "",
        return_tags = "true",
        return_previews = "true",
        return_vote_data = "true",
        return_short_description = "true",
        format = "json",
    }
    if sort.days then
        query.days = tostring(sort.days)
    end
    for i, tag in ipairs(required) do
        query["requiredtags[" .. (i - 1) .. "]"] = tag
    end
    local ei = 0
    for tag in pairs(excluded) do
        query["excludedtags[" .. ei .. "]"] = tag
        ei = ei + 1
    end

    local rsp = session.authorized(ctx, function(access_token, http)
        query.access_token = access_token
        return http:get(QUERYFILES):query(query):timeout(20):send()
    end)
    if not rsp:ok() then
        error("steam workshop http " .. tostring(rsp:status()))
    end
    local body = rsp:json() or {}
    local response = body.response or {}
    return {
        items = response.publishedfiledetails or {},
        total = response.total or 0,
        numperpage = NUMPERPAGE,
    }
end

function M.details(ctx, id)
    local rsp = ctx.http
        :post(FILEDETAILS)
        :form({ itemcount = "1", ["publishedfileids[0]"] = tostring(id) })
        :timeout(20)
        :send()
    if not rsp:ok() then
        error("steam workshop http " .. tostring(rsp:status()))
    end
    local body = rsp:json() or {}
    local list = (body.response or {}).publishedfiledetails or {}
    return list[1] or {}
end

return M

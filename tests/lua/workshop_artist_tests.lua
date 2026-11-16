local source = assert(arg[1], "plugin directory required")
local modules, requests, responses = {}, {}, {}
local id = "76561198000000001"
function tr(value) return value end
modules["wallpaper_engine.session"] = {
    authorized = function(ctx, callback) return callback("test-token", ctx.http) end,
    ensure_http = function(ctx) return ctx.http end,
}
local profile_calls = 0
modules["wallpaper_engine.profile"] = {
    get = function(_, ids)
        profile_calls = profile_calls + 1
        assert(ids[1] == id)
        return { [id] = { name = "Artist", avatar_url = "https://example.com/avatar.jpg", url = "https://steamcommunity.com/profiles/" .. id .. "/" } }
    end,
}
function import(name)
    if not modules[name] then
        local file = name:gsub("%.", "/") .. ".lua"
        modules[name] = assert(loadfile(source .. "/" .. file))()
    end
    return modules[name]
end
local api = import("wallpaper_engine.api")
local map = import("wallpaper_engine.map")
local discover = import("wallpaper_engine.discover")
local elements
local ctx = { http = {}, html = { query = function(html, selector)
    assert(html == "author" and selector:find(".workshopItemPreviewHolder[id]", 1, true))
    return elements
end } }
local function request(url)
    local value = { url = url }
    function value:query(query) self.query_value = query; return self end
    function value:form(form) self.form_value = form; return self end
    function value:timeout(_) return self end
    function value:send()
        requests[#requests + 1] = self
        local response = assert(table.remove(responses, 1), "response required")
        elements = response.elements
        return {
            ok = function() return true end,
            status = function() return 200 end,
            json = function() return { response = response } end,
            text = function() return response.html or "" end,
        }
    end
    return value
end
function ctx.http:get(url) return request(url) end
function ctx.http:post(url) return request(url) end
local function reply(items, total)
    responses[#responses + 1] = { result = 1, publishedfiledetails = items or {}, total = total or 0 }
end
local function page(ids, total)
    local nodes = {}
    if total then
        nodes[#nodes + 1] = { attrs = {}, text = "Showing 1-30 of " .. total .. " entries" }
    else
        nodes[#nodes + 1] = { attrs = { id = "no_items" }, text = "No matching files were found" }
    end
    for _, id in ipairs(ids) do nodes[#nodes + 1] = { attrs = { id = "sharedfile_" .. id }, text = "" } end
    responses[#responses + 1] = { html = "author", elements = nodes }
end
local item = { publishedfileid = "1", creator = id, title = "Wallpaper", tags = { { tag = "Scene" } } }
local other = { publishedfileid = "2", creator = id, title = "Other", tags = { { tag = "Scene" } } }
page({"1", "2"}, 31)
reply({ other, item })
local result = discover.search(ctx, { artist_id = id, sort = "recent", page = 1 })
local query = requests[1].query_value
assert(requests[1].url == "https://steamcommunity.com/profiles/" .. id .. "/myworkshopfiles/")
assert(query.appid == "431960" and query.p == "1" and query.numperpage == "30" and query.l == "english")
assert(query.browsefilter == "myfiles" and query.section == "items" and query.sortmethod == "creationorder")
assert(query.query_type == nil and query.search_text == nil and query.days == nil)
assert(query.access_token == nil and result.has_more)
assert(requests[2].form_value.itemcount == "2" and requests[2].form_value["publishedfileids[0]"] == "1")
assert(result.items[1].id == "1" and result.items[2].id == "2")
assert(query["requiredtags[0]"] == "Everyone")
local excluded = {}
for key, value in pairs(query) do if key:find("excludedtags", 1, true) then excluded[value] = true end end
assert(excluded.Mature and excluded.Questionable and excluded.Application and excluded.Asset)
assert(result.items[1].author == "Artist" and result.items[1].author_avatar_url == "https://example.com/avatar.jpg")
local detail = discover.details(ctx, "1")
assert(#requests == 2 and detail.author_id == id and detail.author == "Artist")
assert(detail.author_avatar_url == result.items[1].author_avatar_url and detail.author_url:find(id, 1, true))

page({"1"}, 31)
reply({ item })
result = discover.search(ctx, { artist_id = id, sort = "recent", page = 2, tags = { "Scene", "Mature" }, excluded_tags = { "Anime" } })
query = requests[3].query_value
assert(query.p == "2" and query.sortmethod == "creationorder" and not result.has_more)
excluded = {}
for key, value in pairs(query) do if key:find("excludedtags", 1, true) then excluded[value] = true end end
assert(excluded.Everyone and excluded.Questionable and excluded.Video and excluded.Web and excluded.Anime and not excluded.Mature)
for _, params in ipairs({
    {artist_id="bad"}, {artist_id=id,query="ignored"}, {artist_id=id,sort="trend_week"},
}) do assert(not pcall(api.search, ctx, params)) end
assert(#requests == 4)

reply({ item }, 1)
api.search(ctx, { sort = "trend_week", query = "cloud" })
query = requests[5].query_value
assert(requests[5].url:find("QueryFiles", 1, true))
assert(query.query_type == "3" and query.days == "7" and query.search_text == "cloud" and query.steamid == nil)
local fallback = map.details(item)
assert(fallback.author_id == id and fallback.author == id and fallback.author_avatar_url == "")
responses[#responses + 1] = { result = 15 }
assert(not pcall(api.search, ctx, {}))
page({}, nil)
assert(#api.search(ctx, {artist_id=id}).items == 0)
page({"1"}, 1)
reply({{publishedfileid="1",creator="76561198000000002"}})
assert(not pcall(api.search, ctx, {artist_id=id}))
responses[#responses + 1] = {html="author",elements={}}
assert(not pcall(api.search, ctx, {artist_id=id}))
assert(profile_calls == 3)
print("workshop artist tests passed")

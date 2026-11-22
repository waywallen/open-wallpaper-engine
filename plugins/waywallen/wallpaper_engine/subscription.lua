local session = import("wallpaper_engine.session")
local workshop = import("wallpaper_engine.workshop")

local M = {}

local SUBSCRIBE = "https://api.steampowered.com/IPublishedFileService/Subscribe/v1/"
local UNSUBSCRIBE = "https://api.steampowered.com/IPublishedFileService/Unsubscribe/v1/"
local DETAILS = "https://steamcommunity.com/sharedfiles/filedetails/?id="
local DETAILS_HEADERS = {
    Accept = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
    ["Accept-Language"] = "zh",
    DNT = "1",
    Priority = "u=0, i",
    ["Sec-Fetch-Dest"] = "document",
    ["Sec-Fetch-Mode"] = "navigate",
    ["Sec-Fetch-Site"] = "none",
    ["Sec-Fetch-User"] = "?1",
    ["Upgrade-Insecure-Requests"] = "1",
}
local STATUS_TTL_MS = 60000
local UNKNOWN_TTL_MS = 15000
local MUTATION_GRACE_MS = 30000

local cache = {}
local cache_generation = -1

local function sync_cache()
    local current = session.generation()
    if current ~= cache_generation then
        cache = {}
        cache_generation = current
    end
end

function M.clear()
    cache = {}
    cache_generation = session.generation()
    workshop.forget()
end

local function parse_page_state(ctx, html)
    local signed_in = false
    local state
    local elements = ctx.html.query(
        html,
        "#account_pulldown, #SubscribeItemOptionSubscribed.selected, "
            .. "#SubscribeItemOptionAdd.selected"
    )
    for _, element in ipairs(elements) do
        local attrs = element.attrs or {}
        local id = attrs.id
        if id == "account_pulldown" then
            signed_in = true
        elseif id == "SubscribeItemOptionSubscribed" then
            if state then return "unknown" end
            state = "subscribed"
        elseif id == "SubscribeItemOptionAdd" then
            if state then return "unknown" end
            state = "unsubscribed"
        end
    end
    if not signed_in then return nil, "signed_out" end
    if not state then
        ctx.log("wallpaper_engine: Steam detail page has no selected subscription option")
    end
    return state or "unknown"
end

local function request_page(ctx, id, retry_login)
    local http = session.ensure_http(ctx)
    local response = http:get(DETAILS .. ctx.url.encode_component(tostring(id)))
        :headers(DETAILS_HEADERS)
        :timeout(20)
        :send()
    if response:status() == 401 or response:status() == 403 then
        if retry_login then
            session.refresh_credentials(ctx)
            return request_page(ctx, id, false)
        end
        session.reject_credentials(ctx)
        error("Steam Community session expired; sign in again")
    end
    if not response:ok() then
        error("Steam subscription status failed with HTTP " .. tostring(response:status()))
    end
    local html = response:text() or ""
    session.capture_profile(ctx, html)
    local result, reason = parse_page_state(ctx, html)
    if reason == "signed_out" then
        if retry_login then
            session.refresh_credentials(ctx)
            return request_page(ctx, id, false)
        end
        session.reject_credentials(ctx)
        error("Steam Community session expired; sign in again")
    end
    return result
end

function M.status(ctx, ids)
    sync_cache()
    if #ids == 0 then return {} end
    session.ensure_http(ctx)
    local now = ctx.time.now()
    local result = {}
    for _, id in ipairs(ids) do
        local key = tostring(id)
        local cached = cache[key]
        local recorded = workshop.state(key)
        if cached and cached.expires_at > now then
            result[key] = cached.state
        elseif recorded then
            -- Steam already wrote this subscription down on this machine, so
            -- the detail page would only confirm what the client knows.
            result[key] = recorded
        else
            local ok, value = pcall(request_page, ctx, key, true)
            if not ok then
                local message = tostring(value)
                if message:find("session expired; sign in again", 1, true) then error(value) end
                ctx.log("wallpaper_engine: Steam subscription status failed: " .. message)
                value = "unknown"
            end
            result[key] = value
            cache[key] = {
                state = value,
                expires_at = now + (value == "unknown" and UNKNOWN_TTL_MS or STATUS_TTL_MS),
            }
        end
    end
    return result
end

local function set_subscription(ctx, id, subscribed)
    sync_cache()
    local response = session.authorized(ctx, function(access_token, http)
        return http
            :post(subscribed and SUBSCRIBE or UNSUBSCRIBE)
            :form({
                access_token = access_token,
                publishedfileid = tostring(id),
                list_type = "1",
                notify_client = "1",
            })
            :timeout(20)
            :send()
    end)
    if not response:ok() then
        error("Steam subscription request failed with HTTP " .. tostring(response:status()))
    end
    cache[tostring(id)] = {
        state = subscribed and "subscribed" or "unsubscribed",
        expires_at = ctx.time.now() + MUTATION_GRACE_MS,
    }
    workshop.mark(id, subscribed)
    return { accepted = true }
end

function M.subscribe(ctx, id)
    return set_subscription(ctx, id, true)
end

function M.unsubscribe(ctx, id)
    return set_subscription(ctx, id, false)
end

return M

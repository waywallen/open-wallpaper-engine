local session = import("wallpaper_engine.session")

local M = {}
local PLAYER_LINK_DETAILS = "https://api.steampowered.com/IPlayerService/GetPlayerLinkDetails/v1/"
local REQUEST_TIMEOUT = 4
local profiles = {}

local function avatar_url(ctx, digest)
    if type(digest) ~= "string" or digest == "" then return "" end
    local ok, bytes = pcall(ctx.base64.decode, digest)
    if not ok or type(bytes) ~= "string" or #bytes ~= 20 or bytes == string.rep("\0", 20) then return "" end
    local hex = bytes:gsub(".", function(byte) return string.format("%02x", byte:byte()) end)
    return "https://avatars.steamstatic.com/" .. hex .. "_full.jpg"
end

local function fetch(ctx, ids)
    local input_json = ctx.json.encode({ steamids = ids })
    local response = session.authorized(ctx, function(access_token, http)
        return http:get(PLAYER_LINK_DETAILS)
            :query({ access_token = access_token, input_json = input_json })
            :timeout(REQUEST_TIMEOUT)
            :send()
    end)
    if not response:ok() then
        error("Steam player details http " .. tostring(response:status()))
    end
    local body = response:json() or {}
    local result = body.response or {}
    return result.accounts or {}
end

function M.get(ctx, ids)
    local resolved, pending, seen = {}, {}, {}
    for _, id in ipairs(ids) do
        local key = tostring(id or "")
        if key ~= "" and not seen[key] then
            seen[key] = true
            if profiles[key] then
                resolved[key] = profiles[key]
            else
                pending[#pending + 1] = key
            end
        end
    end
    if #pending == 0 then return resolved end

    local ok, accounts = pcall(fetch, ctx, pending)
    if not ok then
        ctx.log("wallpaper_engine: Steam player details unavailable")
        return resolved
    end
    if type(accounts) ~= "table" then return resolved end
    for _, account in ipairs(accounts) do
        local data = type(account) == "table" and account.public_data
        if type(data) == "table" then
            local id, name = tostring(data.steamid or ""), data.persona_name
            if seen[id] and type(name) == "string" and name ~= "" then
                local profile = {
                    id = id,
                    name = name,
                    avatar_url = avatar_url(ctx, data.sha_digest_avatar),
                    url = "https://steamcommunity.com/profiles/" .. id .. "/",
                }
                profiles[id] = profile
                resolved[id] = profile
            end
        end
    end
    return resolved
end

return M

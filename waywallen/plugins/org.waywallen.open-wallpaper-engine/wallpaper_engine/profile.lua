local session = import("wallpaper_engine.session")

local M = {}
local PLAYER_LINK_DETAILS = "https://api.steampowered.com/IPlayerService/GetPlayerLinkDetails/v1/"
local REQUEST_TIMEOUT = 4
local names = {}

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

function M.names(ctx, ids)
    local resolved, pending, seen = {}, {}, {}
    for _, id in ipairs(ids) do
        local key = tostring(id or "")
        if key ~= "" and not seen[key] then
            seen[key] = true
            if names[key] then
                resolved[key] = names[key]
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
                names[id] = name
                resolved[id] = name
            end
        end
    end
    return resolved
end

return M

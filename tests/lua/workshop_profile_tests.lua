local source = assert(arg[1], "plugin directory required")
local requests, logs, next_response = {}, {}, nil
local session = {
    authorized = function(ctx, callback)
        return callback("test-token", ctx.http)
    end,
}
function import(name)
    assert(name == "wallpaper_engine.session")
    return session
end
local profile = assert(loadfile(source .. "/wallpaper_engine/profile.lua"))()
local ctx = {
    json = { encode = function(value) return value end },
    log = function(message) logs[#logs + 1] = message end,
    http = {},
}
function ctx.http:get(url)
    local request = { url = url }
    function request:query(value) self.query_value = value; return self end
    function request:timeout(value) self.timeout_value = value; return self end
    function request:send()
        requests[#requests + 1] = self
        if type(next_response) == "string" then error(next_response) end
        return next_response
    end
    return request
end
local function response(accounts)
    return {
        ok = function() return true end,
        json = function() return { response = { accounts = accounts } } end,
    }
end
local function account(id, name)
    return { public_data = { steamid = id, persona_name = name } }
end

next_response = response({ account("1", "Alice"), account("2", "Bob") })
local result = profile.names(ctx, { "1", "2", "1", "" })
assert(result["1"] == "Alice" and result["2"] == "Bob")
assert(#requests == 1 and #requests[1].query_value.input_json.steamids == 2)
assert(requests[1].query_value.access_token == "test-token")
assert(requests[1].timeout_value == 4)
assert(requests[1].url:find("GetPlayerLinkDetails", 1, true))
profile.names(ctx, { "2", "1" })
profile.names(ctx, {})
assert(#requests == 1)

next_response = response({ account("3", "Carol"), account("unexpected", "Ignored") })
result = profile.names(ctx, { "1", "3", "4" })
assert(result["1"] == "Alice" and result["3"] == "Carol" and not result["4"])
assert(not result.unexpected and #requests == 2)
next_response = "network unavailable"
result = profile.names(ctx, { "1", "4" })
assert(result["1"] == "Alice" and not result["4"] and #logs == 1)
next_response = response({ account("4", "Dave") })
assert(profile.names(ctx, { "4" })["4"] == "Dave")
assert(#requests == 4)
next_response = { ok = function() return false end, status = function() return 503 end }
assert(not profile.names(ctx, { "5" })["5"] and #logs == 2)
next_response = response({ account("5", "Eve") })
assert(profile.names(ctx, { "5" })["5"] == "Eve")
print("workshop profile tests passed")

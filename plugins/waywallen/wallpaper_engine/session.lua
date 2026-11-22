local M = {}

local FORMAT_VERSION = "steam-session-v5"
local LEGACY_LINE_FORMAT_VERSION = "steam-session-v2"
local PLATFORM_WEB_BROWSER = "web_browser"
local PLATFORM_LEGACY = "legacy"
local FINALIZE_LOGIN = "https://login.steampowered.com/jwt/finalizelogin"
local COMMUNITY_ROOT = "https://steamcommunity.com/"
local COMMUNITY_HEADERS = {
    Accept = "application/json, text/plain, */*",
    Origin = "https://steamcommunity.com",
    Referer = "https://steamcommunity.com/",
    ["Sec-Fetch-Site"] = "cross-site",
    ["Sec-Fetch-Mode"] = "cors",
    ["Sec-Fetch-Dest"] = "empty",
}
local TRANSFER_ATTEMPTS = 5

local state = {
    account_name = "",
    steamid = "",
    platform = "",
    access_token = "",
    refresh_token = "",
    community_access_token = "",
    avatar_url = "",
}
local generation = 0
local credentials_rejected = false
local last_check = { state = "signed_out", display_value = tr("Not signed in") }

local function remember(check)
    last_check = check
    return check
end

local function drop_web_session(ctx)
    if ctx and ctx.http then ctx.http:clear_cookies() end
    state.community_access_token = ""
end

local function clear()
    state.account_name = ""
    state.steamid = ""
    state.platform = ""
    state.access_token = ""
    state.refresh_token = ""
    state.community_access_token = ""
    state.avatar_url = ""
    credentials_rejected = false
    generation = generation + 1
    last_check = { state = "signed_out", display_value = tr("Not signed in") }
end

local function jwt_payload(ctx, token)
    local encoded = token and token:match("^[^.]+%.([^.]+)%.[^.]+$")
    if not encoded then return nil end
    local decoded_ok, decoded = pcall(ctx.base64.decode, encoded)
    if not decoded_ok or not decoded then return nil end
    local parsed_ok, payload = pcall(ctx.json.parse, decoded)
    if not parsed_ok or type(payload) ~= "table" then return nil end
    return payload
end

local function token_valid(ctx, token, expected_steamid)
    local payload = jwt_payload(ctx, token)
    local expires = payload and tonumber(payload.exp)
    if not expires or expires <= ctx.time.unix() + 60 then return nil end
    if expected_steamid and tostring(payload.sub or "") ~= expected_steamid then return nil end
    return payload
end

local function audience_contains(payload, expected)
    if type(payload.aud) == "string" then return payload.aud == expected end
    if type(payload.aud) ~= "table" then return false end
    for _, audience in ipairs(payload.aud) do
        if audience == expected then return true end
    end
    return false
end

local function validate_saved_session(ctx)
    if state.platform ~= PLATFORM_WEB_BROWSER or state.refresh_token == "" then
        error("Steam sign-in is required")
    end
    local refresh = token_valid(ctx, state.refresh_token, state.steamid)
    if not refresh or not audience_contains(refresh, "web")
        or not audience_contains(refresh, "derive") then
        error("Steam session expired; sign in again")
    end
end

local function read_fields(blob, version, count)
    if blob:sub(1, #version + 1) ~= version .. "\n" then return nil end
    local fields = {}
    local offset = #version + 2
    for index = 1, count do
        local separator = blob:find("\n", offset, true)
        if not separator then return nil end
        local length = tonumber(blob:sub(offset, separator - 1))
        if not length or length < 0 or length % 1 ~= 0 then return nil end
        local first = separator + 1
        local last = first + length - 1
        if last > #blob then return nil end
        fields[index] = blob:sub(first, last)
        offset = last + 1
    end
    if offset <= #blob then return nil end
    return fields
end

local function mark_legacy(account_name, steamid)
    state.account_name = account_name or ""
    state.steamid = steamid or ""
    state.platform = PLATFORM_LEGACY
    state.access_token = ""
    state.refresh_token = ""
    state.community_access_token = ""
    state.avatar_url = ""
    last_check = {
        state = "expired",
        display_value = state.account_name ~= "" and state.account_name or "Steam",
        error = tr("Steam sign-in must be renewed"),
    }
end

function M.load(blob)
    clear()
    if type(blob) ~= "string" or blob == "" then return end

    local fields = read_fields(blob, FORMAT_VERSION, 7)
    if fields then
        state.account_name = fields[1]
        state.steamid = fields[2]
        state.platform = fields[3]
        state.access_token = fields[4]
        state.refresh_token = fields[5]
        state.community_access_token = fields[6]
        state.avatar_url = fields[7]
        if state.platform == PLATFORM_LEGACY then
            mark_legacy(state.account_name, state.steamid)
        elseif state.platform ~= PLATFORM_WEB_BROWSER then
            clear()
        end
        return
    end

    local v4_fields = read_fields(blob, "steam-session-v4", 6)
    if v4_fields then
        state.account_name = v4_fields[1]
        state.steamid = v4_fields[2]
        state.platform = v4_fields[3]
        state.access_token = v4_fields[4]
        state.refresh_token = v4_fields[5]
        state.community_access_token = v4_fields[6]
        if state.platform == PLATFORM_LEGACY then
            mark_legacy(state.account_name, state.steamid)
        elseif state.platform ~= PLATFORM_WEB_BROWSER then
            clear()
        end
        return
    end

    local old_fields = read_fields(blob, "steam-session-v3", 4)
    if old_fields then
        mark_legacy(old_fields[1], old_fields[2])
        return
    end

    local lines = {}
    for line in (blob .. "\n"):gmatch("(.-)\n") do table.insert(lines, line) end
    if lines[1] == LEGACY_LINE_FORMAT_VERSION then
        mark_legacy(lines[2], lines[3])
    end
end

function M.save()
    local fields = {
        state.account_name,
        state.steamid,
        state.platform,
        state.access_token,
        state.refresh_token,
        state.community_access_token,
        state.avatar_url,
    }
    local chunks = { FORMAT_VERSION, "\n" }
    for _, value in ipairs(fields) do
        table.insert(chunks, tostring(#value))
        table.insert(chunks, "\n")
        table.insert(chunks, value)
    end
    return table.concat(chunks)
end

local function legacy_json_string(raw, key)
    local _, last = raw:find('"' .. key .. '"%s*:%s*"')
    if not last then return "" end
    local out = {}
    local index = last + 1
    while index <= #raw do
        local char = raw:sub(index, index)
        if char == '"' then return table.concat(out) end
        if char ~= "\\" then
            table.insert(out, char)
            index = index + 1
        else
            local escaped = raw:sub(index + 1, index + 1)
            local replacements = {
                ['"'] = '"', ["\\"] = "\\", ["/"] = "/",
                b = "\b", f = "\f", n = "\n", r = "\r", t = "\t",
            }
            if replacements[escaped] then
                table.insert(out, replacements[escaped])
                index = index + 2
            elseif escaped == "u" then
                local codepoint = tonumber(raw:sub(index + 2, index + 5), 16)
                if not codepoint then return "" end
                table.insert(out, utf8.char(codepoint))
                index = index + 6
            else
                return ""
            end
        end
    end
    return ""
end

function M.migrate(schema_id, raw)
    if schema_id ~= "waywallen-steam-session-v1" then
        error("unsupported Steam session schema")
    end
    clear()
    mark_legacy(
        legacy_json_string(raw, "account_name"),
        legacy_json_string(raw, "steamid")
    )
    return M.save()
end

function M.set_auth(ctx, account_name, access_token, refresh_token)
    local refresh = token_valid(ctx, refresh_token)
    local steamid = refresh and tostring(refresh.sub or "") or ""
    if steamid == "" or not steamid:match("^%d+$")
        or not audience_contains(refresh, "web")
        or not audience_contains(refresh, "derive") then
        error("Steam returned an invalid WebBrowser refresh token")
    end
    access_token = access_token or ""
    if type(access_token) ~= "string" then
        error("Steam returned an invalid WebBrowser access token")
    end
    if access_token ~= "" then
        local access = token_valid(ctx, access_token, steamid)
        if not access or audience_contains(access, "derive") then
            error("Steam returned an invalid WebBrowser access token")
        end
    end

    drop_web_session(ctx)
    state.account_name = account_name or ""
    state.steamid = steamid
    state.platform = PLATFORM_WEB_BROWSER
    state.access_token = access_token
    state.refresh_token = refresh_token
    state.avatar_url = ""
    credentials_rejected = false
    generation = generation + 1
    last_check = {
        state = "signed_in",
        display_value = state.account_name ~= "" and state.account_name or "Steam",
    }
end

local function response_json_text(ctx, response)
    local text = response:text() or ""
    if text == "" then return nil end
    local ok, parsed = pcall(ctx.json.parse, text)
    if ok and type(parsed) == "table" then return parsed end
    return nil
end

local function execute_transfer(ctx, client, transfer)
    if type(transfer.url) ~= "string" or type(transfer.params) ~= "table" then
        error("Steam returned an invalid Web transfer")
    end
    local body = { steamID = state.steamid }
    for key, value in pairs(transfer.params) do body[key] = value end

    local last_error = "Steam Web transfer failed"
    for attempt = 1, TRANSFER_ATTEMPTS do
        local ok, result = pcall(function()
            local response = client
                :post(transfer.url)
                :multipart(body)
                :timeout(20)
                :send()
            local final_url = response:url()
            if not response:ok() then
                error("Steam Web transfer failed with HTTP " .. tostring(response:status()))
            end
            local parsed = response_json_text(ctx, response)
            if parsed and parsed.result and tonumber(parsed.result) ~= 1 then
                error("Steam Web transfer was rejected")
            end
            if not client:cookie(final_url, "steamLoginSecure") then
                error("Steam Web transfer returned no login cookie")
            end
            return final_url
        end)
        if ok then return result end
        last_error = tostring(result)
        if attempt < TRANSFER_ATTEMPTS then ctx.time.sleep(500) end
    end
    error(last_error)
end

local function community_token(ctx, client)
    local cookie = client:cookie(COMMUNITY_ROOT, "steamLoginSecure")
    if type(cookie) ~= "string" or cookie == "" then
        error("Steam Web session has no Community login cookie")
    end
    local decoded = ctx.url.decode_component(cookie)
    local steamid, token = decoded:match("^(%d+)%|%|(.+)$")
    if steamid ~= state.steamid then
        error("Steam Community session belongs to another account")
    end
    local payload = token_valid(ctx, token, state.steamid)
    if not payload or not audience_contains(payload, "web:community") then
        error("Steam Community session is invalid; sign in again")
    end
    return token
end

local function restore_web_session(ctx, token)
    local payload = token_valid(ctx, token, state.steamid)
    if not payload or not audience_contains(payload, "web:community") then return nil end

    local candidate = ctx.http
    local sessionid = ctx.random.hex(12)
    candidate:set_cookie(
        COMMUNITY_ROOT,
        "steamLoginSecure="
            .. ctx.url.encode_component(state.steamid .. "||" .. token)
            .. "; Path=/; Secure; HttpOnly; SameSite=None; Domain=steamcommunity.com"
    )
    candidate:set_cookie(
        COMMUNITY_ROOT,
        "sessionid=" .. sessionid
            .. "; Path=/; Secure; SameSite=None; Domain=steamcommunity.com"
    )
    state.community_access_token = token
    return candidate
end

local function build_web_session(ctx)
    validate_saved_session(ctx)
    local candidate = ctx.http
    local sessionid = ctx.random.hex(12)
    local finalize = candidate
        :post(FINALIZE_LOGIN)
        :headers(COMMUNITY_HEADERS)
        :multipart({
            nonce = state.refresh_token,
            sessionid = sessionid,
            redir = "https://steamcommunity.com/login/home/?goto=",
        })
        :timeout(20)
        :send()
    if not finalize:ok() then
        local status = finalize:status()
        if status == 401 or status == 403 then
            error("Steam session expired; sign in again")
        end
        error("Steam Web finalization failed with HTTP " .. tostring(status))
    end
    local response = finalize:json() or {}
    if response.error then error("Steam session expired; sign in again") end
    if response.steamID and tostring(response.steamID) ~= state.steamid then
        error("Steam Web finalization returned another account")
    end
    if type(response.transfer_info) ~= "table" or #response.transfer_info == 0 then
        error("Steam returned an incomplete Web finalization response")
    end

    local domains = {}
    for _, transfer in ipairs(response.transfer_info) do
        local transfer_url = execute_transfer(ctx, candidate, transfer)
        local host = ctx.url.host(transfer_url)
        if type(host) ~= "string" or host == "" then
            error("Steam Web transfer returned an invalid URL")
        end
        if host ~= "login.steampowered.com" then domains[host] = transfer_url end
    end
    for host, url in pairs(domains) do
        candidate:set_cookie(url, "sessionid=" .. sessionid
            .. "; Path=/; Secure; SameSite=None; Domain=" .. host)
    end

    local token = community_token(ctx, candidate)
    state.community_access_token = token
end

function M.refresh_credentials(ctx)
    validate_saved_session(ctx)
    drop_web_session(ctx)
    local ok, error_value = pcall(build_web_session, ctx)
    if not ok then
        if tostring(error_value):find("expired; sign in again", 1, true) then
            M.reject_credentials(ctx)
        end
        error(error_value, 0)
    end
    credentials_rejected = false
    generation = generation + 1
    return ctx.http
end

function M.complete_login(ctx, account_name, access_token, refresh_token)
    M.set_auth(ctx, account_name, access_token, refresh_token)
    local ok, error_value = pcall(M.refresh_credentials, ctx)
    if ok then return ctx.http end
    drop_web_session(ctx)
    clear()
    error(error_value, 0)
end

function M.ensure_http(ctx)
    validate_saved_session(ctx)
    if credentials_rejected then
        error("Steam Community session expired; sign in again")
    end
    local valid_cookie, token = pcall(community_token, ctx, ctx.http)
    if valid_cookie then
        state.community_access_token = token
        return ctx.http
    end
    if restore_web_session(ctx, state.community_access_token) then return ctx.http end
    if restore_web_session(ctx, state.access_token) then return ctx.http end
    return M.refresh_credentials(ctx)
end

function M.ensure_access_token(ctx)
    M.ensure_http(ctx)
    return state.community_access_token
end

function M.authorized(ctx, callback)
    local http = M.ensure_http(ctx)
    local response = callback(state.community_access_token, http)
    local status = response:status()
    if status ~= 401 and status ~= 403 then return response end
    http = M.refresh_credentials(ctx)
    response = callback(state.community_access_token, http)
    status = response:status()
    if status == 401 or status == 403 then M.reject_credentials(ctx) end
    return response
end

function M.reject_credentials(ctx)
    drop_web_session(ctx)
    credentials_rejected = true
    generation = generation + 1
    last_check = {
        state = "expired",
        display_value = state.account_name ~= "" and state.account_name or "Steam",
        error = tr("Steam session expired; sign in again"),
    }
end

function M.capture_profile(ctx, html)
    local element = ctx.html.query_one(
        html or "",
        "a.user_avatar img, .playerAvatarAutoSizeInner img"
    )
    local src = element and element.attrs and element.attrs.src or ""
    if type(src) == "string" and src:match("^https://") then
        state.avatar_url = src
    end
    return state.avatar_url
end

function M.refresh_profile(ctx)
    local ok, result = pcall(function()
        local response = M.authorized(ctx, function(_, http)
            return http:get(COMMUNITY_ROOT .. "profiles/" .. state.steamid .. "/")
                :timeout(20)
                :send()
        end)
        if not response:ok() then return "" end
        return M.capture_profile(ctx, response:text() or "")
    end)
    if not ok then
        ctx.log("wallpaper_engine: Steam profile image unavailable")
        return ""
    end
    return result
end

function M.sign_out(ctx)
    drop_web_session(ctx)
    clear()
end

function M.account_name()
    return state.account_name
end

function M.signed_in()
    return state.platform == PLATFORM_WEB_BROWSER and state.refresh_token ~= ""
end

function M.generation()
    return generation
end

function M.check(ctx)
    if state.platform == PLATFORM_LEGACY then
        return remember({
            state = "expired",
            display_value = state.account_name ~= "" and state.account_name or "Steam",
            error = tr("Steam sign-in must be renewed"),
        })
    end
    if not M.signed_in() then
        return remember({ state = "signed_out", display_value = tr("Not signed in") })
    end
    if credentials_rejected then
        return remember({
            state = "expired",
            display_value = state.account_name ~= "" and state.account_name or "Steam",
            error = tr("Steam session expired; sign in again"),
        })
    end
    local ok, error_value = pcall(validate_saved_session, ctx)
    if not ok then
        local message = tostring(error_value)
        local expired = message:find("expired; sign in again", 1, true) ~= nil
            or message:find("invalid; sign in again", 1, true) ~= nil
        return remember({
            state = expired and "expired" or "error",
            display_value = state.account_name ~= "" and state.account_name or "Steam",
            error = expired and tr("Steam session expired; sign in again")
                or tr("Steam session check failed"),
        })
    end
    local display = state.account_name ~= "" and state.account_name or "Steam"
    return remember({
        state = "signed_in",
        display_value = display,
        avatar_url = state.avatar_url,
    })
end

function M.current_check()
    return last_check
end

return M

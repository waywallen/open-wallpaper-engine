local plugin_root = assert(arg[1], "plugin root argument required")
local cache = {}

function tr(msgid)
    return msgid
end

function import(name)
    if cache[name] ~= nil then return cache[name] end
    local path = plugin_root .. "/" .. name:gsub("%.", "/") .. ".lua"
    local chunk = assert(loadfile(path, "t", _ENV))
    local module = assert(chunk(), "module did not return a value: " .. name)
    cache[name] = module
    return module
end

local fixtures = assert(loadfile(plugin_root .. "/tests/fixtures.lua", "t", _ENV))()

local function equal(actual, expected, message)
    if actual ~= expected then
        error((message or "values differ") .. ": expected " .. tostring(expected)
            .. ", got " .. tostring(actual), 2)
    end
end

local function truthy(value, message)
    if not value then error(message or "expected a truthy value", 2) end
end

local function has_indexed_value(values, prefix, expected)
    for key, value in pairs(values) do
        if key:match("^" .. prefix .. "%[%d+%]$") and value == expected then return true end
    end
    return false
end

local function fake_context()
    local queued = {}
    local requests = {}
    local encoded_values = {}
    local now = 1000
    local unix_now = 2
    local sleep_calls = 0
    local html_query_calls = 0
    local decoded = {
        qr_access = {
            exp = 4102444800,
            sub = fixtures.steamid,
            aud = { "web:community" },
        },
        refresh = {
            exp = 4102444800,
            sub = fixtures.steamid,
            aud = { "web", "renew", "derive" },
        },
        community = {
            exp = 4102444800,
            sub = fixtures.steamid,
            aud = { "web:community" },
        },
        short_community = {
            exp = 120,
            sub = fixtures.steamid,
            aud = { "web:community" },
        },
        short_access = {
            exp = 120,
            sub = fixtures.steamid,
            aud = { "web:community" },
        },
        store = {
            exp = 4102444800,
            sub = fixtures.steamid,
            aud = { "web:store" },
        },
        expired_refresh = {
            exp = 1,
            sub = fixtures.steamid,
            aud = { "web", "derive" },
        },
        no_derive = {
            exp = 4102444800,
            sub = fixtures.steamid,
            aud = { "web", "renew" },
        },
        ["transfer-ok"] = { result = 1 },
    }

    local function url_host(url)
        return url:match("^https://([^/:?#]+)")
    end

    local function domain_matches(host, domain)
        domain = domain:gsub("^%.", ""):lower()
        host = (host or ""):lower()
        return host == domain or host:sub(-#domain - 1) == "." .. domain
    end

    local function request(client, method, url)
        local value = { client = client, method = method, url = url }
        function value:form(form)
            self.form_data = form
            return self
        end
        function value:multipart(form)
            self.multipart_data = form
            return self
        end
        function value:query(query)
            self.query_data = query
            return self
        end
        function value:headers(headers)
            self.header_data = headers
            return self
        end
        function value:timeout(seconds)
            self.timeout_seconds = seconds
            return self
        end
        function value:send()
            table.insert(requests, self)
            local fixture = table.remove(queued, 1)
            if not fixture then error("no fake HTTP response queued for " .. self.url) end
            local final_url = fixture.url or self.url
            for _, cookie in ipairs(fixture.cookies or {}) do
                client:set_cookie(final_url, cookie)
            end
            local response = {
                status_code = fixture.status or 200,
                json_body = fixture.json,
                text_body = fixture.text or "",
                final_url = final_url,
            }
            function response:ok()
                return self.status_code >= 200 and self.status_code < 300
            end
            function response:status() return self.status_code end
            function response:json() return self.json_body end
            function response:text() return self.text_body end
            function response:url() return self.final_url end
            return response
        end
        return value
    end

    local function new_client()
        local client = { cookies = {} }
        function client:get(url) return request(self, "GET", url) end
        function client:post(url) return request(self, "POST", url) end
        function client:set_cookie(url, value)
            local name, cookie_value = value:match("^([^=;]+)=([^;]*)")
            local domain = value:match("[Dd][Oo][Mm][Aa][Ii][Nn]=([^;]+)") or url_host(url)
            if not name or not domain then error("invalid fake cookie") end
            self.cookies[name .. "\0" .. domain] = {
                name = name,
                value = cookie_value,
                domain = domain,
            }
        end
        function client:cookie(url, name)
            local host = url_host(url)
            for _, cookie in pairs(self.cookies) do
                if cookie.name == name and domain_matches(host, cookie.domain) then
                    return cookie.value
                end
            end
            return nil
        end
        function client:clear_cookies() self.cookies = {} end
        return client
    end

    local ctx = {
        http = new_client(),
        log = function() end,
    }
    ctx.time = {
        unix = function() return unix_now end,
        now = function() return now end,
        sleep = function(milliseconds)
            sleep_calls = sleep_calls + 1
            now = now + milliseconds
        end,
    }
    ctx.random = { hex = function(bytes) return string.rep("ab", bytes) end }
    ctx.base64 = { decode = function(value) return value end }
    ctx.json = {
        parse = function(value)
            local result = decoded[value]
            if result == nil then error("invalid fixture JSON") end
            return result
        end,
        encode = function(value)
            table.insert(encoded_values, value)
            return "fixture-json"
        end,
    }
    ctx.url = {
        host = function(value) return value:match("^https?://([^/:?#]+)") end,
        parse = function(value)
            local scheme, host, path = value:match("^(https?)://([^/:?#]+)(.*)$")
            if not scheme then return nil end
            return { scheme = scheme, host = host, path = path }
        end,
        encode_component = function(value)
            return tostring(value):gsub("([^%w%-_%.~])", function(char)
                return string.format("%%%02X", string.byte(char))
            end)
        end,
        decode_component = function(value)
            return value:gsub("+", " "):gsub("%%(%x%x)", function(hex)
                return string.char(tonumber(hex, 16))
            end)
        end,
    }
    local function query_one(html, selector)
            if selector:find("a.user_avatar img", 1, true) then
                local src = html:match('<a[^>]-class="[^"]*user_avatar[^"]*"[^>]*>%s*<img[^>]-src="([^"]+)"')
                if src then return { attrs = { src = src } } end
            end
            local id = selector:match("#([%w_%-]+)")
            if not id or not html:find('id="' .. id .. '"', 1, true) then return nil end
            local class = html:match('id="' .. id .. '"[^>]-class="([^"]*)"') or ""
            for required in selector:gmatch("%.([%w_%-]+)") do
                if selector:find(":not(." .. required .. ")", 1, true) == nil
                    and not class:match("%f[%w]" .. required .. "%f[%W]") then return nil end
            end
            if selector:find(":not(.subscribed)", 1, true)
                and class:match("%f[%w]subscribed%f[%W]") then return nil end
            return { attrs = { id = id, class = class } }
    end
    ctx.html = {
        query_one = query_one,
        query = function(html, selector)
            html_query_calls = html_query_calls + 1
            local result = {}
            for part in selector:gmatch("[^,]+") do
                local element = query_one(html, part)
                if element then table.insert(result, element) end
            end
            return result
        end,
    }
    function ctx.queue(json, status, text, url, cookies)
        table.insert(queued, {
            json = json,
            status = status,
            text = text,
            url = url,
            cookies = cookies,
        })
    end
    function ctx.queue_response(fixture) table.insert(queued, fixture) end
    function ctx.last_request() return requests[#requests] end
    function ctx.request_at(index) return requests[index] end
    function ctx.request_count() return #requests end
    function ctx.queued_count() return #queued end
    function ctx.encoded_at(index) return encoded_values[index] end
    function ctx.cookie_snapshot()
        local snapshot = {}
        for key, value in pairs(ctx.http.cookies) do
            snapshot[key] = {
                name = value.name,
                value = value.value,
                domain = value.domain,
            }
        end
        return snapshot
    end
    function ctx.restore_cookies(snapshot)
        ctx.http.cookies = snapshot
    end
    function ctx.sleep_count() return sleep_calls end
    function ctx.html_query_count() return html_query_calls end
    function ctx.advance(milliseconds) now = now + milliseconds end
    function ctx.set_unix(value) unix_now = value end
    return ctx
end

local function queue_web_session(ctx, finalize, community_cookie)
    ctx.queue(finalize or fixtures.web_finalize)
    ctx.queue_response({
        text = "transfer-ok",
        url = "https://steamcommunity.com/login/settoken",
        cookies = { community_cookie or fixtures.community_cookie },
    })
    ctx.queue_response({
        text = "transfer-ok",
        url = "https://store.steampowered.com/login/settoken",
        cookies = { fixtures.store_cookie },
    })
    ctx.queue_response({
        text = "transfer-ok",
        url = "https://help.steampowered.com/login/settoken",
        cookies = { fixtures.help_cookie },
    })
    ctx.queue_response({
        text = "transfer-ok",
        url = "https://checkout.steampowered.com/login/settoken",
        cookies = { fixtures.checkout_cookie },
    })
    ctx.queue_response({
        text = "transfer-ok",
        url = "https://steam.tv/login/settoken",
        cookies = { fixtures.tv_cookie },
    })
end

local main = assert(loadfile(plugin_root .. "/main.lua", "t", _ENV))()
local info = main.info()
equal(info.display_name, "Workshop", "display name")
equal(info.capabilities.discover.subscription, true, "subscription capability")
equal(info.capabilities.discover.download, nil, "download capability must be absent")
equal(info.capabilities.discover.resolve, nil, "resolve capability must be absent")
equal(info.capabilities.discover.remote_hint, nil, "remote hint must be absent")
equal(#info.capabilities.discover.filters, 5, "discover filter count")
equal(info.capabilities.discover.filters[1].id, "type", "type filter")
equal(info.capabilities.discover.filters[1].type, "multi_select", "type filter control")
equal(info.capabilities.discover.filters[2].id, "content_rating", "content rating filter")
equal(info.capabilities.discover.filters[2].type, "multi_select", "content rating control")
equal(info.capabilities.discover.filters[2].options[1].value, "Everyone", "everyone value")
equal(info.capabilities.discover.filters[2].options[2].value, "Questionable", "questionable value")
equal(info.capabilities.discover.filters[2].options[3].value, "Mature", "mature value")
for _, filter in ipairs(info.capabilities.discover.filters) do
    for _, option in ipairs(filter.options) do
        equal(option.label, option.value,
            filter.id .. " option label preserves the Steam filter value")
    end
end
equal(info.status[1].group, "steam_account", "Steam account status group")
equal(info.status[1].group_label, "Steam account", "Steam account status group label")
equal(info.actions[1].label, "Log in to Steam", "Steam login label")
equal(info.actions[1].group, "steam_account", "Steam login group")
equal(info.actions[1].group_label, "Steam account", "Steam login group label")
equal(info.actions[1].browse_button_label, "Log in to Steam", "Steam browse login button label")
equal(info.actions[1].description, nil, "Steam manage action description")
equal(
    info.actions[1].browse_description,
    "Waywallen only manages Workshop subscriptions and does not download wallpapers. " ..
        "Keep the Steam desktop client running to download subscribed items.",
    "Steam browse login description"
)
equal(main.discover.download, nil, "download callback must be absent")
equal(main.discover.resolve, nil, "resolve callback must be absent")
equal(main.discover.tags, nil, "dynamic tag callback must be absent")
equal(main.source.remove, nil, "source remove callback must be absent")
truthy(type(main.subscription.status) == "function", "subscription.status missing")
truthy(type(main.qrlogin.begin) == "function", "qrlogin.begin missing")

local session = import("wallpaper_engine.session")
local auth = import("wallpaper_engine.auth")
local subscription = import("wallpaper_engine.subscription")

local function login(ctx)
    queue_web_session(ctx)
    session.complete_login(
        ctx,
        "fixture-account",
        "header.qr_access.signature",
        "header.refresh.signature"
    )
end

session.sign_out()
local auth_ctx = fake_context()
auth_ctx.queue(fixtures.qr_begin)
local begin = auth.begin(auth_ctx)
equal(begin.challenge, "https://s.team/q/example", "QR challenge")
equal(begin.poll_after_ms, 1500, "QR interval")
equal(begin.title, "Log in to Steam", "QR title")
equal(
    begin.instruction,
    "Scan and approve this login in the Steam mobile app.",
    "QR instruction"
)
local request = auth_ctx.last_request()
equal(request.method, "POST", "QR begin method")
equal(request.form_data.input_json, "fixture-json", "QR input_json envelope")
equal(request.header_data.Origin, "https://steamcommunity.com", "QR origin")
local qr_input = auth_ctx.encoded_at(1)
equal(qr_input.website_id, nil, "QR website id must not be sent")
equal(qr_input.platform_type, nil, "QR platform must be nested")
equal(qr_input.device_details.platform_type, 2, "QR WebBrowser platform")
equal(qr_input.device_details.device_friendly_name, "waywallen", "QR device name")

auth_ctx.queue(fixtures.qr_pending)
equal(auth.poll(auth_ctx, begin.key).state, "awaiting_scan", "pending QR state")
auth_ctx.queue(fixtures.qr_confirmation)
equal(auth.poll(auth_ctx, begin.key).state, "awaiting_confirmation", "confirmation QR state")
auth_ctx.queue(fixtures.qr_rotation)
local rotated = auth.poll(auth_ctx, begin.key)
equal(rotated.state, "challenge_changed", "rotation QR state")
equal(rotated.challenge, "https://s.team/q/rotated", "rotated challenge")
auth_ctx.queue(fixtures.qr_success)
queue_web_session(auth_ctx)
auth_ctx.queue(nil, 200, fixtures.profile_page)
equal(auth.poll(auth_ctx, begin.key).state, "succeeded", "successful QR state")
equal(session.account_name(), "fixture-account", "QR account")
local qr_check = session.check(auth_ctx)
equal(qr_check.display_value, "fixture-account", "QR profile name")
equal(
    qr_check.avatar_url,
    "https://avatars.steamstatic.com/avatar.jpg",
    "QR profile avatar"
)
local qr_state = session.save()
truthy(qr_state:find("header.qr_access.signature", 1, true), "QR access token not saved")
truthy(qr_state:find("header.refresh.signature", 1, true), "QR refresh token not saved")
local invalid_auth_ok = pcall(
    session.set_auth,
    auth_ctx,
    "other-account",
    "header.qr_access.signature",
    "header.no_derive.signature"
)
equal(invalid_auth_ok, false, "refresh audience validation")
equal(session.account_name(), "fixture-account", "invalid auth must not replace session")

session.set_auth(
    auth_ctx,
    "line one\nline two",
    "header.qr_access.signature",
    "header.refresh.signature"
)
local serialized = session.save()
session.sign_out()
session.load(serialized)
equal(session.account_name(), "line one\nline two", "opaque state round trip")
truthy(session.signed_in(), "v5 session did not restore")

session.load(table.concat({
    "steam-session-v2",
    "line-format-account",
    fixtures.steamid,
    "header.qr_access.signature",
    "header.refresh.signature",
}, "\n"))
equal(session.account_name(), "line-format-account", "line state compatibility")
equal(session.signed_in(), false, "Client state must require a new sign-in")
equal(session.check(auth_ctx).state, "expired", "Client state lifecycle")
local migrated = session.migrate(
    "waywallen-steam-session-v1",
    '{"account_name":"escaped\\nname","access_token":"secret",'
        .. '"refresh_token":"secret","steamid":"' .. fixtures.steamid .. '"}'
)
truthy(not migrated:find("secret", 1, true), "legacy token must not migrate")
session.sign_out()
session.load(migrated)
equal(session.account_name(), "escaped\nname", "legacy JSON migration")
equal(session.check(auth_ctx).state, "expired", "migrated lifecycle")

local web_ctx = fake_context()
session.set_auth(
    web_ctx,
    "fixture-account",
    "header.qr_access.signature",
    "header.refresh.signature"
)
equal(session.check(web_ctx).state, "signed_in", "local Web lifecycle")
equal(web_ctx.request_count(), 0, "lifecycle check must not create a Web session")
queue_web_session(web_ctx)
session.complete_login(
    web_ctx,
    "fixture-account",
    "header.qr_access.signature",
    "header.refresh.signature"
)
equal(session.ensure_access_token(web_ctx), "header.community.signature", "Community token")
equal(web_ctx.request_count(), 6, "all Web transfers")
request = web_ctx.request_at(1)
truthy(request.url:find("/jwt/finalizelogin", 1, true), "finalization endpoint")
equal(request.multipart_data.nonce, "header.refresh.signature", "finalization nonce")
equal(#request.multipart_data.sessionid, 24, "sessionid length")
equal(request.header_data.Origin, "https://steamcommunity.com", "finalization origin")
equal(web_ctx.request_at(2).multipart_data.steamID, fixtures.steamid, "transfer SteamID")
for index = 3, 6 do
    equal(web_ctx.request_at(index).multipart_data.steamID, fixtures.steamid, "all transfer SteamIDs")
end
equal(
    web_ctx.http:cookie("https://steamcommunity.com/", "sessionid"),
    request.multipart_data.sessionid,
    "shared Community sessionid"
)

local finalized_state = session.save()
session.load(finalized_state)
local restart_ctx = fake_context()
restart_ctx.restore_cookies(web_ctx.cookie_snapshot())
equal(session.ensure_access_token(restart_ctx), "header.community.signature", "restart restore")
equal(restart_ctx.request_count(), 0, "restart must reuse Community token")
equal(
    restart_ctx.http:cookie("https://steamcommunity.com/", "steamLoginSecure"),
    fixtures.steamid .. "%7C%7Cheader.community.signature",
    "restored Community cookie"
)

local refresh_ctx = fake_context()
queue_web_session(refresh_ctx, nil, fixtures.short_community_cookie)
session.complete_login(
    refresh_ctx,
    "fixture-account",
    "header.short_access.signature",
    "header.refresh.signature"
)
equal(session.ensure_access_token(refresh_ctx), "header.short_community.signature",
    "initial short-lived Community token")
refresh_ctx.set_unix(61)
queue_web_session(refresh_ctx)
equal(session.ensure_access_token(refresh_ctx), "header.community.signature",
    "expired Community credentials refresh")
equal(refresh_ctx.request_count(), 12, "credential refresh finalization count")
equal(session.ensure_access_token(refresh_ctx), "header.community.signature",
    "refreshed Community token reuse")
equal(refresh_ctx.request_count(), 12, "valid credentials must not repeat finalization")

local retry_ctx = fake_context()
session.set_auth(retry_ctx, "fixture-account", "header.qr_access.signature", "header.refresh.signature")
retry_ctx.queue(fixtures.web_finalize)
retry_ctx.queue({}, 503)
retry_ctx.queue_response({
    text = "transfer-ok",
    url = "https://steamcommunity.com/login/settoken",
    cookies = { fixtures.community_cookie },
})
retry_ctx.queue_response({
    text = "transfer-ok",
    url = "https://store.steampowered.com/login/settoken",
    cookies = { fixtures.store_cookie },
})
retry_ctx.queue_response({
    text = "transfer-ok",
    url = "https://help.steampowered.com/login/settoken",
    cookies = { fixtures.help_cookie },
})
retry_ctx.queue_response({
    text = "transfer-ok",
    url = "https://checkout.steampowered.com/login/settoken",
    cookies = { fixtures.checkout_cookie },
})
retry_ctx.queue_response({
    text = "transfer-ok",
    url = "https://steam.tv/login/settoken",
    cookies = { fixtures.tv_cookie },
})
equal(session.refresh_credentials(retry_ctx), retry_ctx.http, "transient transfer retry")
equal(session.ensure_access_token(retry_ctx), "header.community.signature", "refreshed token")
equal(retry_ctx.request_count(), 7, "transfer retry request count")
equal(retry_ctx.sleep_count(), 1, "transfer retry delay")

local exhausted_ctx = fake_context()
session.set_auth(
    exhausted_ctx,
    "fixture-account",
    "header.qr_access.signature",
    "header.refresh.signature"
)
exhausted_ctx.queue(fixtures.web_finalize)
for _ = 1, 5 do exhausted_ctx.queue({}, 503) end
local exhausted_ok = pcall(session.refresh_credentials, exhausted_ctx)
equal(exhausted_ok, false, "exhausted transfer retries")
equal(exhausted_ctx.request_count(), 6, "transfer retry limit")
equal(exhausted_ctx.sleep_count(), 4, "transfer retry delay limit")

local expired_ctx = fake_context()
session.set_auth(expired_ctx, "fixture-account", "header.qr_access.signature", "header.refresh.signature")
expired_ctx.queue({}, 403)
local expired_ok, expired_error = pcall(session.refresh_credentials, expired_ctx)
equal(expired_ok, false, "finalization auth failure")
truthy(tostring(expired_error):find("expired; sign in again", 1, true), "auth failure message")
local expired_request_count = expired_ctx.request_count()
equal(pcall(session.ensure_access_token, expired_ctx), false, "rejected credentials stay expired")
equal(expired_ctx.request_count(), expired_request_count, "rejected credentials must not retry login")
local transient_ctx = fake_context()
session.set_auth(
    transient_ctx,
    "fixture-account",
    "header.qr_access.signature",
    "header.refresh.signature"
)
transient_ctx.queue({}, 503)
local transient_ok = pcall(session.refresh_credentials, transient_ctx)
equal(transient_ok, false, "finalization HTTP failure")

local discover_ctx = fake_context()
login(discover_ctx)
local before_search = discover_ctx.request_count()
discover_ctx.queue(fixtures.query_files)
discover_ctx.queue(nil, 200, fixtures.profile_xml)
local search = main.discover.search(discover_ctx, {
    query = "fixture",
    sort = "trend_week",
    page = 1,
    tags = { "Scene" },
})
equal(#search.items, 1, "search item count")
equal(search.items[1].id, "3765064055", "search item id")
equal(search.items[1].wp_type, "scene", "search item type")
equal(search.items[1].author, "Fixture & Author", "search item author")
request = discover_ctx.request_at(before_search + 1)
equal(request.method, "GET", "QueryFiles method")
equal(request.query_data.access_token, "header.community.signature", "QueryFiles Community token")
equal(request.query_data.appid, "431960", "QueryFiles appid")
truthy(has_indexed_value(request.query_data, "excludedtags", "Video"), "Video type excluded")
truthy(has_indexed_value(request.query_data, "excludedtags", "Web"), "Web type excluded")
equal(has_indexed_value(request.query_data, "requiredtags", "Scene"), false,
    "selected type must not become an AND tag")
truthy(has_indexed_value(request.query_data, "requiredtags", "Everyone"),
    "empty rating selection defaults to Everyone")
truthy(has_indexed_value(request.query_data, "excludedtags", "Questionable"),
    "default rating excludes Questionable")
truthy(has_indexed_value(request.query_data, "excludedtags", "Mature"),
    "default rating excludes Mature")
equal(
    discover_ctx.request_at(before_search + 2).url,
    "https://steamcommunity.com/profiles/76561198000000001/?xml=1",
    "creator profile XML URL"
)
-- A name already looked up costs nothing: a second search that asked Steam
-- again would run out of queued responses and fail here.
discover_ctx.queue(fixtures.query_files)
local cached_search = main.discover.search(discover_ctx, {
    sort = "trend_week",
    page = 1,
    tags = { "Scene", "Video", "Questionable", "Anime" },
})
equal(cached_search.items[1].author, "Fixture & Author", "cached author reused")
equal(discover_ctx.queued_count(), 0, "cached author must not be looked up again")
local cached_query = discover_ctx.request_at(discover_ctx.request_count()).query_data
truthy(has_indexed_value(cached_query, "excludedtags", "Web"), "unselected type excluded")
equal(has_indexed_value(cached_query, "excludedtags", "Scene"), false, "Scene type included")
equal(has_indexed_value(cached_query, "excludedtags", "Video"), false, "Video type included")
truthy(has_indexed_value(cached_query, "excludedtags", "Everyone"),
    "Questionable-only rating excludes Everyone")
equal(has_indexed_value(cached_query, "excludedtags", "Questionable"), false,
    "Questionable-only rating included")
truthy(has_indexed_value(cached_query, "excludedtags", "Mature"),
    "Questionable-only rating excludes Mature")
truthy(has_indexed_value(cached_query, "requiredtags", "Questionable"),
    "single selected rating remains required")
truthy(has_indexed_value(cached_query, "requiredtags", "Anime"), "genre remains required")

discover_ctx.queue(fixtures.query_files)
main.discover.search(discover_ctx, {
    sort = "trend_week",
    page = 1,
    tags = { "Questionable", "Mature" },
})
local multi_rating_query = discover_ctx.request_at(discover_ctx.request_count()).query_data
truthy(has_indexed_value(multi_rating_query, "excludedtags", "Everyone"),
    "multiple ratings exclude Everyone")
equal(has_indexed_value(multi_rating_query, "excludedtags", "Questionable"), false,
    "multiple ratings include Questionable")
equal(has_indexed_value(multi_rating_query, "excludedtags", "Mature"), false,
    "multiple ratings include Mature")
equal(has_indexed_value(multi_rating_query, "requiredtags", "Questionable"), false,
    "multiple ratings must not require Questionable")
equal(has_indexed_value(multi_rating_query, "requiredtags", "Mature"), false,
    "multiple ratings must not require Mature")
local request_count = discover_ctx.request_count()
local details = main.discover.details(discover_ctx, "3765064055")
equal(details.size, "4096", "cached details size")
equal(
    details.web_url,
    "https://steamcommunity.com/sharedfiles/filedetails/?id=3765064055",
    "cached details web URL"
)
equal(discover_ctx.request_count(), request_count, "search metadata must be reused")
discover_ctx.queue(fixtures.file_details)
equal(main.discover.details(discover_ctx, "fallback").size, "8192", "fallback details")

local profile = import("wallpaper_engine.profile")
local profile_ctx = fake_context()

-- Steam answering that it has no such profile is an answer: remember it, so the
-- same dead creator is not looked up once per page for the rest of the session.
profile_ctx.queue(nil, 200, fixtures.profile_missing_xml)
equal(profile.names(profile_ctx, { "76561198000000002" })["76561198000000002"], nil,
    "missing profile yields no author")
local missing_requests = profile_ctx.request_count()
profile.names(profile_ctx, { "76561198000000002" })
equal(profile_ctx.request_count(), missing_requests, "missing profile must not be asked twice")

-- Anything else Steam sends back is not an answer, so nothing is remembered and
-- the grid keeps the empty author it had before.
profile_ctx.queue(nil, 200, "<html><body>Steam is having a moment</body></html>")
equal(next(profile.names(profile_ctx, { "76561198000000003" })), nil,
    "unparsable profile yields no author")

-- Steam being unreachable costs a search a couple of failed requests, not one
-- per creator, and is forgotten so a later search can try again.
profile_ctx.queue(nil, 503)
profile_ctx.queue(nil, 503)
equal(next(profile.names(profile_ctx, {
    "76561198000000004",
    "76561198000000005",
    "76561198000000006",
})), nil, "unreachable Steam yields no authors")
equal(profile_ctx.queued_count(), 0, "failing lookups stop at the failure limit")
profile_ctx.queue(nil, 200, fixtures.profile_xml)
equal(profile.names(profile_ctx, { "76561198000000004" })["76561198000000004"],
    "Fixture & Author", "a failed lookup is retried later")

local subscription_ctx = fake_context()
login(subscription_ctx)
subscription_ctx.queue(nil, 200, fixtures.subscribed_page)
local states = subscription.status(subscription_ctx, { "3765064055" })
equal(states["3765064055"], "subscribed", "subscribed page state")
equal(subscription_ctx.html_query_count(), 1, "subscription page parse count")
request = subscription_ctx.last_request()
equal(request.header_data["Sec-Fetch-Mode"], "navigate", "detail navigation headers")
equal(request.header_data["Sec-Fetch-Site"], "none", "detail navigation origin")
request_count = subscription_ctx.request_count()
equal(subscription.status(subscription_ctx, { "3765064055" })["3765064055"], "subscribed",
    "cached page state")
equal(subscription_ctx.request_count(), request_count, "status cache request count")
subscription_ctx.advance(60001)
subscription_ctx.queue(nil, 200, fixtures.unsubscribed_page)
equal(subscription.status(subscription_ctx, { "3765064055" })["3765064055"], "unsubscribed",
    "expired cache refresh")
subscription_ctx.queue(nil, 200, fixtures.unknown_page)
subscription_ctx.queue(nil, 200, fixtures.conflict_page)
states = subscription.status(subscription_ctx, { "missing-markers", "conflicting-markers" })
equal(states["missing-markers"], "unknown", "missing page markers")
equal(states["conflicting-markers"], "unknown", "conflicting page markers")

local partial_ctx = fake_context()
login(partial_ctx)
partial_ctx.queue(nil, 200, fixtures.subscribed_page)
partial_ctx.queue({}, 503)
states = subscription.status(partial_ctx, { "subscribed", "failed" })
equal(states.subscribed, "subscribed", "batch successful item")
equal(states.failed, "unknown", "batch failed item")

local login_retry_ctx = fake_context()
login(login_retry_ctx)
login_retry_ctx.queue(nil, 200, fixtures.signed_out_page)
queue_web_session(login_retry_ctx)
login_retry_ctx.queue(nil, 200, fixtures.subscribed_page)
equal(subscription.status(login_retry_ctx, { "retry" }).retry, "subscribed", "login page retry")

local rejected_ctx = fake_context()
login(rejected_ctx)
rejected_ctx.queue(nil, 200, fixtures.signed_out_page)
queue_web_session(rejected_ctx)
rejected_ctx.queue(nil, 200, fixtures.signed_out_page)
local rejected_ok = pcall(subscription.status, rejected_ctx, { "rejected" })
equal(rejected_ok, false, "rejected refreshed login")
local rejected_request_count = rejected_ctx.request_count()
equal(pcall(subscription.status, rejected_ctx, { "rejected" }), false,
    "rejected login remains expired")
equal(rejected_ctx.request_count(), rejected_request_count,
    "rejected login must not repeat finalization")

local mutation_ctx = fake_context()
login(mutation_ctx)
mutation_ctx.queue(fixtures.mutation_accepted)
equal(subscription.subscribe(mutation_ctx, "3765064055").accepted, true, "subscribe accepted")
request = mutation_ctx.last_request()
equal(request.form_data.access_token, "header.community.signature", "subscribe Community token")
equal(request.form_data.publishedfileid, "3765064055", "subscribe item id")
request_count = mutation_ctx.request_count()
equal(subscription.status(mutation_ctx, { "3765064055" })["3765064055"], "subscribed",
    "mutation grace state")
equal(mutation_ctx.request_count(), request_count, "mutation grace must avoid page request")
mutation_ctx.advance(30001)
mutation_ctx.queue(nil, 200, fixtures.unsubscribed_page)
equal(subscription.status(mutation_ctx, { "3765064055" })["3765064055"], "unsubscribed",
    "server state corrects mutation expectation")

local retry_mutation_ctx = fake_context()
login(retry_mutation_ctx)
retry_mutation_ctx.queue({}, 403)
queue_web_session(retry_mutation_ctx)
retry_mutation_ctx.queue(fixtures.mutation_accepted)
equal(subscription.unsubscribe(retry_mutation_ctx, "retry-item").accepted, true,
    "mutation re-finalization")
equal(retry_mutation_ctx.request_count(), 14, "mutation auth retry request count")

local failed_mutation_ctx = fake_context()
login(failed_mutation_ctx)
failed_mutation_ctx.queue({}, 500)
local mutation_ok, mutation_error = pcall(
    subscription.subscribe,
    failed_mutation_ctx,
    "failed-item"
)
equal(mutation_ok, false, "failed mutation")
truthy(not tostring(mutation_error):find("header.community.signature", 1, true),
    "mutation error leaked token")

main.actions.invoke(failed_mutation_ctx, "steam_sign_out")
equal(
    failed_mutation_ctx.http:cookie("https://steamcommunity.com/", "steamLoginSecure"),
    nil,
    "sign-out clears the host Community session"
)
local signed_out_ctx = fake_context()
local signed_out_ok, signed_out_error = pcall(
    subscription.status,
    signed_out_ctx,
    { "3765064055" }
)
equal(signed_out_ok, false, "sign-out invalidates subscription access")
equal(signed_out_ctx.request_count(), 0, "signed-out status must not call Steam")
truthy(not tostring(signed_out_error):find("header.", 1, true), "signed-out error leaked token")

local project = import("wallpaper_engine.project")
local original_classify = project.classify
local classify_calls = 0
project.classify = function(...)
    classify_calls = classify_calls + 1
    return original_classify(...)
end
equal(classify_calls, 0, "remote calls must not classify local projects")

local steam_root = "/fixture/steam"
local workshop = steam_root .. project.WORKSHOP
local item_dir = workshop .. "/3765064055"
local local_projects = steam_root .. project.PROJECTS_REL .. "/myprojects"
local local_item_dir = local_projects .. "/local-fixture"
local source_ctx = {
    libraries = function() return { steam_root } end,
    fs = {
        exists = function(path)
            return path == workshop
                or path == local_projects
                or path == item_dir .. "/project.json"
                or path == item_dir .. "/scene.pkg"
                or path == item_dir .. "/preview.jpg"
                or path == local_item_dir .. "/project.json"
                or path == local_item_dir .. "/scene.pkg"
                or path == local_item_dir .. "/preview.jpg"
        end,
        list_dirs = function(path)
            if path == workshop then return { item_dir } end
            if path == local_projects then return { local_item_dir } end
            return {}
        end,
        basename = function(path) return path:match("([^/]+)$") end,
        read = function(path)
            if path == item_dir .. "/project.json"
                or path == local_item_dir .. "/project.json"
            then
                return "fixture-project"
            end
            return nil
        end,
        glob = function() return {} end,
        extension = function(path) return path:match("%.([^./]+)$") end,
    },
    json = { parse = function(value)
        if value == "fixture-project" then
            return {
                title = "Local fixture",
                type = "Scene",
                file = "scene.json",
                preview = "preview.jpg",
            }
        end
        return nil
    end },
    log = function() end,
}
local source_items = main.source.scan(source_ctx)
equal(classify_calls, 2, "explicit source scan must classify local projects")
equal(#source_items, 2, "source scan item count")
equal(source_items[1].resource, item_dir .. "/scene.pkg", "source scan resource")
equal(source_items[1].external_id, "3765064055", "source Workshop subscription id")
equal(
    source_items[1].web_url,
    "https://steamcommunity.com/sharedfiles/filedetails/?id=3765064055",
    "source Workshop web URL"
)
equal(source_items[2].external_id, nil, "local project must not expose a subscription id")
equal(source_items[2].web_url, nil, "local project must not expose a remote URL")

local video_dir = "/fixture/video"
local video_ctx = {
    fs = {
        exists = function(path) return path == video_dir .. "/project.json" end,
        read = function(path)
            if path == video_dir .. "/project.json" then return "video-project" end
            return nil
        end,
    },
    json = {
        parse = function(value)
            if value ~= "video-project" then return nil end
            return {
                general = {
                    properties = {
                        schemecolor = {
                            type = "color",
                            value = "0.99608 0.72941 0.00000",
                        },
                    },
                },
            }
        end,
    },
}
local video_apply = main.wallpaper.apply({
    wp_type = "video",
    resource = video_dir .. "/wallpaper.mp4",
}, video_ctx)
equal(video_apply.extras.path, video_dir .. "/wallpaper.mp4", "video resource extra")
equal(
    video_apply.default_user_properties["waywallen.scheme_color"],
    "0.99608 0.72941 0.00000",
    "video default scheme color"
)

-- Registering a library that already points inside steamapps must still scan.
for _, suffix in ipairs({ "/", "/steamapps", "/steamapps/workshop/content/" .. project.WE_APPID }) do
    local nested_ctx = {}
    for key, value in pairs(source_ctx) do nested_ctx[key] = value end
    nested_ctx.libraries = function() return { steam_root .. suffix } end
    equal(#main.source.scan(nested_ctx), 2, "source scan from library path '" .. suffix .. "'")
end

project.classify = original_classify

-- Steam's own record of the Workshop directory: it marks the subscriptions and
-- leaves hand-placed directories out, so they never offer an unsubscribe.
local acf_path = steam_root .. "/steamapps/workshop/appworkshop_" .. project.WE_APPID .. ".acf"
local hand_placed_dir = workshop .. "/2589297069"
local unmeasured_dir = workshop .. "/3765064056"

local function workshop_ctx(acf)
    local ctx = {}
    for key, value in pairs(source_ctx) do ctx[key] = value end
    ctx.fs = {}
    for key, value in pairs(source_ctx.fs) do ctx.fs[key] = value end
    ctx.fs.exists = function(path)
        if path == acf_path then return acf ~= nil end
        if path == hand_placed_dir .. "/scene.pkg" then return true end
        if path == unmeasured_dir .. "/scene.pkg" then return true end
        return source_ctx.fs.exists(path)
    end
    ctx.fs.read = function(path)
        if path == acf_path then return acf end
        return source_ctx.fs.read(path)
    end
    ctx.fs.list_dirs = function(path)
        if path == workshop then return { item_dir, hand_placed_dir, unmeasured_dir } end
        return source_ctx.fs.list_dirs(path)
    end
    return ctx
end

local recorded_items = main.source.scan(workshop_ctx(fixtures.workshop_acf))
equal(#recorded_items, 4, "scan item count with Steam's record")
equal(recorded_items[1].external_id, "3765064055", "recorded subscription keeps its id")
equal(
    recorded_items[1].web_url,
    "https://steamcommunity.com/sharedfiles/filedetails/?id=3765064055",
    "recorded subscription keeps its web URL"
)
equal(recorded_items[1].size, 4096, "recorded item reports the size Steam measured")
equal(recorded_items[2].name, "Workshop 2589297069", "hand-placed Workshop directory")
equal(recorded_items[2].external_id, nil, "hand-placed directory must not offer unsubscribe")
equal(recorded_items[2].web_url, nil, "hand-placed directory must not expose a remote URL")
equal(recorded_items[2].size, nil, "hand-placed directory was never measured by Steam")
equal(recorded_items[3].size, nil, "record without a size leaves the item unmeasured")
equal(recorded_items[4].size, nil, "local project is outside Steam's Workshop record")

local truncated_items = main.source.scan(workshop_ctx(fixtures.workshop_acf_truncated))
equal(truncated_items[2].external_id, "2589297069", "truncated record keeps the previous behaviour")
equal(truncated_items[1].size, nil, "truncated record measures nothing")
local unrecorded_items = main.source.scan(workshop_ctx(nil))
equal(unrecorded_items[2].external_id, "2589297069", "missing record keeps the previous behaviour")
equal(unrecorded_items[1].size, nil, "missing record measures nothing")

main.source.scan(workshop_ctx(fixtures.workshop_acf))
local recorded_ctx = fake_context()
login(recorded_ctx)
request_count = recorded_ctx.request_count()
equal(subscription.status(recorded_ctx, { "3765064055" })["3765064055"], "subscribed",
    "state answered from Steam's record")
equal(recorded_ctx.request_count(), request_count, "recorded state must not call Steam")

recorded_ctx.queue(fixtures.mutation_accepted)
equal(subscription.unsubscribe(recorded_ctx, "3765064055").accepted, true, "unsubscribe accepted")
recorded_ctx.advance(30001)
recorded_ctx.queue(nil, 200, fixtures.unsubscribed_page)
request_count = recorded_ctx.request_count()
equal(subscription.status(recorded_ctx, { "3765064055" })["3765064055"], "unsubscribed",
    "unsubscribing here drops the item from the record")
equal(recorded_ctx.request_count(), request_count + 1, "dropped item falls back to the page")

main.source.scan(workshop_ctx(fixtures.workshop_acf))
local forget_ctx = fake_context()
main.actions.invoke(forget_ctx, "steam_sign_out")
login(forget_ctx)
forget_ctx.queue(nil, 200, fixtures.subscribed_page)
request_count = forget_ctx.request_count()
equal(subscription.status(forget_ctx, { "3765064055" })["3765064055"], "subscribed",
    "record is re-read from Steam after a sign-out")
equal(forget_ctx.request_count(), request_count + 1, "sign-out drops the recorded state")

-- Wallpaper property titles are keys into Wallpaper Engine's own locale
-- tables. The plugin used to read ui_en-us.json only, so a Russian user read
-- English property names next to a Russian UI. `$LANG` now picks the file,
-- with en-us left behind it because the translated files trail en-us by a few
-- dozen keys.
local locale_dir = steam_root .. "/steamapps/common/wallpaper_engine/locale/"
local locale_files = {
    ["ui_en-us.json"] = { ui_prop_speed = "Speed", ui_prop_only_en = "English only" },
    ["ui_ru-ru.json"] = { ui_prop_speed = "Скорость" },
}
local locale_ctx = {
    env = function(name) return name == "LANG" and "ru_RU.UTF-8" or nil end,
    config = { get = function() return nil end },
    fs = {
        exists = function(path)
            local name = path:match("([^/]+)$")
            if path:sub(1, #locale_dir) == locale_dir then
                return locale_files[name] ~= nil
            end
            return path == item_dir .. "/project.json"
        end,
        read = function(path)
            local name = path:match("([^/]+)$")
            if path:sub(1, #locale_dir) == locale_dir and locale_files[name] then
                return "locale:" .. name
            end
            if path == item_dir .. "/project.json" then return "locale-project" end
            return nil
        end,
    },
    json = {
        parse = function(value)
            local name = value:match("^locale:(.+)$")
            if name then return locale_files[name] end
            if value == "locale-project" then
                return { general = { properties = {
                    speed = { text = "ui_prop_speed", type = "slider", value = 1 },
                    only_en = { text = "ui_prop_only_en", type = "bool", value = true },
                    unknown = { text = "ui_prop_absent", type = "bool", value = true },
                } } }
            end
            return nil
        end,
        encode = function(value) return value end,
    },
}
local locale_props = main.wallpaper.properties(
    { wp_type = "scene", resource = item_dir .. "/scene.pkg", library_root = steam_root },
    locale_ctx
)
truthy(locale_props.speed.text:find("Скорость", 1, true),
    "property title takes the translation for the user's language")
truthy(locale_props.only_en.text:find("English only", 1, true),
    "key missing from the translated file falls back to en-us")
truthy(locale_props.unknown.text:find("ui_prop_absent", 1, true),
    "key in neither file is left alone")

print("OWE waywallen Lua contract fixtures passed")

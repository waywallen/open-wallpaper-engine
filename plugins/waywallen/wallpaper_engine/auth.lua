local session = import("wallpaper_engine.session")

local M = {}

local BEGIN = "https://api.steampowered.com/IAuthenticationService/BeginAuthSessionViaQR/v1/"
local POLL = "https://api.steampowered.com/IAuthenticationService/PollAuthSessionStatus/v1/"
local COMMUNITY_HEADERS = {
    Accept = "application/json, text/plain, */*",
    Origin = "https://steamcommunity.com",
    Referer = "https://steamcommunity.com",
}

function M.begin(ctx)
    local input_json = ctx.json.encode({
        device_details = {
            device_friendly_name = "waywallen",
            platform_type = 2,
        },
    })
    local rsp = ctx.http
        :post(BEGIN)
        :headers(COMMUNITY_HEADERS)
        :form({
            input_json = input_json,
        })
        :timeout(20)
        :send()
    if not rsp:ok() then
        error("Steam QR login failed with HTTP " .. tostring(rsp:status()))
    end
    local response = ((rsp:json() or {}).response or {})
    if not response.client_id or not response.request_id or not response.challenge_url then
        error("Steam returned an incomplete QR login challenge")
    end
    return {
        key = {
            client_id = tostring(response.client_id),
            request_id = response.request_id,
            challenge = response.challenge_url,
        },
        challenge = response.challenge_url,
        poll_after_ms = math.floor((tonumber(response.interval) or 5) * 1000),
        expires_in_ms = 180000,
        title = tr("Log in to Steam"),
        instruction = tr("Scan and approve this login in the Steam mobile app."),
    }
end

function M.poll(ctx, key)
    local rsp = ctx.http
        :post(POLL)
        :headers(COMMUNITY_HEADERS)
        :form({
            client_id = key.client_id,
            request_id = key.request_id,
            format = "json",
        })
        :timeout(20)
        :send()
    if not rsp:ok() then
        return {
            state = "failed",
            error = tr("Steam QR poll failed with HTTP %1", rsp:status()),
        }
    end
    local response = ((rsp:json() or {}).response or {})
    if type(response.refresh_token) == "string" and response.refresh_token ~= "" then
        session.complete_login(
            ctx,
            response.account_name,
            response.access_token,
            response.refresh_token
        )
        session.refresh_profile(ctx)
        return {
            state = "succeeded",
            display_value = response.account_name or "Steam",
        }
    end
    if response.new_client_id and tostring(response.new_client_id) ~= "0" then
        key.client_id = tostring(response.new_client_id)
    end
    if type(response.new_challenge_url) == "string"
        and response.new_challenge_url ~= ""
        and response.new_challenge_url ~= key.challenge then
        key.challenge = response.new_challenge_url
        return {
            state = "challenge_changed",
            challenge = key.challenge,
        }
    end
    if response.had_remote_interaction then
        return { state = "awaiting_confirmation" }
    end
    return { state = "awaiting_scan", challenge = key.challenge }
end

function M.cancel(ctx, key)
    key.cancelled = true
end

return M

local source = import("wallpaper_engine.source")
local wallpaper = import("wallpaper_engine.wallpaper")
local discover = import("wallpaper_engine.discover")
local api = import("wallpaper_engine.api")
local auth = import("wallpaper_engine.auth")
local session = import("wallpaper_engine.session")
local subscription = import("wallpaper_engine.subscription")

local M = {}
local steam_account_group = tr("Steam account")

local function expand_home(path)
    local getenv = os and os.getenv
    local home = getenv and getenv("HOME") or nil
    if type(home) == "string" and home ~= "" and string.sub(path, 1, 1) == "~" then
        if home == "/var/home" then
            home = "/home"
        elseif string.sub(home, 1, 10) == "/var/home/" then
            home = "/home/" .. string.sub(home, 11)
        end
        return home .. string.sub(path, 2)
    end
    return path
end

function M.info()
    return {
        name = "wallpaper_engine",
        display_name = tr("Workshop"),
        status = {
            {
                id = "steam_account",
                label = tr("Status"),
                group = "steam_account",
                group_label = steam_account_group,
                order = 20,
            },
        },
        actions = {
            {
                id = "steam_sign_in",
                kind = "qr_login",
                label = tr("Log in to Steam"),
                browse_button_label = tr("Log in to Steam"),
                browse_description = tr([[Waywallen only manages Workshop subscriptions and does not download wallpapers. Keep the Steam desktop client running to download subscribed items.]]),
                group = "steam_account",
                group_label = steam_account_group,
                order = 21,
                required_for_browsing = true,
            },
            {
                id = "steam_sign_out",
                kind = "invoke",
                label = tr("Sign out"),
                group = "steam_account",
                group_label = steam_account_group,
                order = 22,
            },
        },
        state_migrations = {
            { schema_id = "waywallen-steam-session-v1", file = "steam-session.json" },
        },
        capabilities = {
            source = {
                types = { "scene", "video", "web" },
                scan = true,
                auto_detect = true,
                library_label = tr("Steam Library Path"),
                library_hint = tr([[Pick the directory that contains the `steamapps` folder.
Typically `%1` or `%2` (or `%3` for Flatpak Steam).]],
                    expand_home("~/.steam/steam"),
                    expand_home("~/.local/share/Steam"),
                    expand_home("~/.var/app/com.valvesoftware.Steam/data/Steam")
                ),
            },
            discover = {
                search = true,
                details = true,
                subscription = true,
                sorts = {
                    { key = "trend_day", label = tr("Trending today") },
                    { key = "trend_week", label = tr("Trending this week") },
                    { key = "trend_month", label = tr("Trending this month") },
                    { key = "trend_3months", label = tr("Trending 3 months") },
                    { key = "trend_6months", label = tr("Trending 6 months") },
                    { key = "trend_year", label = tr("Trending this year") },
                    { key = "recent", label = tr("Most recent") },
                    { key = "most_subscribed", label = tr("Most subscribed") },
                    { key = "top_rated", label = tr("Top rated") },
                },
                filters = api.filters,
            },
            wallpaper = {
                properties = true,
                apply = true,
            },
        },
    }
end

M.source = source
M.wallpaper = wallpaper
M.discover = discover
M.subscription = subscription

M.lifecycle = {}
function M.lifecycle.load(blob)
    session.load(blob)
end
function M.lifecycle.save()
    return session.save()
end
function M.lifecycle.check(ctx)
    return session.check(ctx)
end
function M.lifecycle.migrate(schema_id, raw)
    return session.migrate(schema_id, raw)
end

M.actions = {}
function M.actions.status(ctx)
    local checked = session.current_check()
    local active = checked.state == "signed_in"
    return {
        status = { steam_account = checked.error or checked.display_value or "" },
        actions = {
            steam_sign_in = { visible = not active, enabled = not active },
            steam_sign_out = { visible = session.signed_in(), enabled = true },
        },
    }
end
function M.actions.invoke(ctx, action_id)
    if action_id ~= "steam_sign_out" then
        error("unsupported Steam action")
    end
    session.sign_out(ctx)
    subscription.clear()
end

M.qrlogin = {}
function M.qrlogin.begin(ctx, action_id)
    if action_id ~= "steam_sign_in" then
        error("unsupported Steam QR action")
    end
    return auth.begin(ctx)
end
M.qrlogin.poll = auth.poll
M.qrlogin.cancel = auth.cancel

return M

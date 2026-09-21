module;

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

export module viewer.stdin_control;

import rstd;
import wescene.json;
import wescene.scene_wallpaper;

namespace viewer
{
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::io::eprintln;
using rstd::io::println;
using rstd::io::error::Error;

export class StdinJsonControl {
public:
    explicit StdinJsonControl(bool enabled): m_enabled(enabled) {
        if (! m_enabled) return;
        m_original_flags = ::fcntl(STDIN_FILENO, F_GETFL, 0);
        if (m_original_flags < 0 ||
            ::fcntl(STDIN_FILENO, F_SETFL, m_original_flags | O_NONBLOCK) < 0) {
            eprintln("--stdin-json: cannot make stdin non-blocking");
            m_enabled = false;
        }
    }

    ~StdinJsonControl() {
        if (m_enabled && m_original_flags >= 0) {
            (void)::fcntl(STDIN_FILENO, F_SETFL, m_original_flags);
        }
    }

    void poll(owe::SceneWallpaper& wallpaper) {
        if (! m_enabled || m_eof) return;

        char buffer[4096];
        for (;;) {
            const auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count > 0) {
                for (ssize_t index = 0; index < count; ++index) {
                    if (buffer[index] == '\n')
                        consumePending(wallpaper);
                    else
                        m_pending.push(u8(static_cast<unsigned char>(buffer[index])));
                }
                continue;
            }
            if (count == 0) {
                m_eof = true;
                consumePending(wallpaper);
                return;
            }
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                eprintln("--stdin-json: stdin read failed: {}", Error::last_os_error());
                m_eof = true;
            }
            return;
        }
    }

private:
    void consumePending(owe::SceneWallpaper& wallpaper) {
        if (! m_pending.is_empty() && *m_pending.last().unwrap() == u8('\r')) m_pending.pop();
        if (m_pending.is_empty()) return;
        auto bytes = rstd::move(m_pending);
        m_pending  = Vec<u8>::make();
        auto text  = String::from_utf8(rstd::move(bytes));
        if (text.is_err()) {
            eprintln("--stdin-json: invalid UTF-8");
            return;
        }
        consumeLine(wallpaper, text->as_str());
    }

    static void consumeLine(owe::SceneWallpaper& wallpaper, ref<str> line) {
        auto parsed_result = owe::ParseJson(line);
        if (parsed_result.is_err()) {
            const auto error = parsed_result.unwrap_err();
            eprintln(
                "--stdin-json: invalid JSON at line {} column {}", error.line(), error.column());
            return;
        }
        auto command = parsed_result.unwrap();
        if (! command.is_object()) {
            eprintln("--stdin-json: command must be a JSON object");
            return;
        }

        auto command_name = command.get("command"_str);
        if (command_name.is_none() || ! (**command_name).is_string()) {
            eprintln("--stdin-json: command requires a string name");
            return;
        }

        auto name = *(**command_name).as_str();
        if (name == "set_user_property"_str) {
            setUserProperty(wallpaper, command);
        } else if (name == "set_mpris"_str) {
            setMpris(wallpaper, command);
        } else {
            eprintln("--stdin-json: unsupported command");
        }
    }

    static void setUserProperty(owe::SceneWallpaper& wallpaper, const owe::Json& command) {
        auto key   = command.get("key"_str);
        auto value = command.get("value"_str);
        if (key.is_none() || ! (**key).is_string() || value.is_none()) {
            eprintln("--stdin-json: set_user_property requires a string key and a value field");
            return;
        }

        auto property = *(**key).as_str();
        if (property.is_empty()) {
            eprintln("--stdin-json: set_user_property requires a non-empty key");
            return;
        }
        wallpaper.setUserPropertyJson(property, (**value).clone());
        println("scene-viewer: queued user property '{}'", property);
    }

    static bool readString(const owe::Json& command, rstd::ref<rstd::str> key, String& value) {
        auto field = command.get(key);
        if (field.is_none()) return true;
        auto text = (**field).as_str();
        if (text.is_none()) return false;
        value = String::make(*text);
        return true;
    }

    static void setMpris(owe::SceneWallpaper& wallpaper, const owe::Json& command) {
        auto state = command.get("state"_str);
        if (state.is_none()) {
            eprintln("--stdin-json: set_mpris requires state 0, 1, or 2");
            return;
        }
        auto state_value = (**state).as_u64();
        if (state_value.is_none() || *state_value > rstd::u64(2)) {
            eprintln("--stdin-json: set_mpris requires state 0, 1, or 2");
            return;
        }

        owe::MediaStatus status;
        status.state = static_cast<rstd::uint32_t>(state_value->to_primitive());
        if (! readString(command, "title"_str, status.title) ||
            ! readString(command, "artist"_str, status.artist) ||
            ! readString(command, "album"_str, status.album) ||
            ! readString(command, "album_artist"_str, status.album_artist) ||
            ! readString(command, "art_url"_str, status.art_url) ||
            ! readString(command, "previous_art_url"_str, status.previous_art_url)) {
            eprintln("--stdin-json: set_mpris metadata fields must be strings");
            return;
        }

        auto title   = status.title.clone();
        auto art_url = status.art_url.clone();
        wallpaper.setMediaStatus(rstd::move(status));
        println("scene-viewer: queued MPRIS title '{}' art '{}'", title, art_url);
    }

    bool    m_enabled { false };
    bool    m_eof { false };
    int     m_original_flags { -1 };
    Vec<u8> m_pending;
};

} // namespace viewer
